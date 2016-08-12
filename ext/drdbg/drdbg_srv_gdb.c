/* **********************************************************
 * Copyright (c) 2016 Google, Inc.   All rights reserved.
 * **********************************************************/

/*
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * * Neither the name of Google, Inc. nor the names of its contributors may be
 *   used to endorse or promote products derived from this software without
 *   specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL GOOGLE, INC. OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 */

/* DynamoRIO Debugger Transparency Extension: GDB Server */

#include "dr_api.h"
#include "drdbg.h"
#include "drdbg_server_int.h"
#include "drdbg_srv_gdb.h"

#include <string.h>

#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>

/* Server constants */
#define MAX_PACKET_SIZE 0x4000
#define NUM_SUPPORTED_CMDS 1

/* Server data */
static int drdbg_srv_gdb_sock = -1;
static int drdbg_srv_gdb_conn = -1;
struct sockaddr_in drdbg_srv_gdb_client_addr;

/* GDB Helper functions */
static
void
gdb_sendack(char ack)
{
    send(drdbg_srv_gdb_conn, &ack, 1, 0);
}

static
bool
gdb_recvack(void)
{
    char ack;
    int ret = recv(drdbg_srv_gdb_conn, &ack, 1, 0);
    if (ret == -1)
        return false;
    return ack == '+';
}

static
unsigned char
gdb_chksum(const char *buf, int len)
{
    int i = 0;
    unsigned char chksum = 0;
    for (i = 0; i < len; i++)
        chksum += buf[i];
    return chksum;
}

static
int
gdb_hexify(char *out, ssize_t len_out, char *buf, ssize_t len_buf)
{
    int i = 0;
    if (len_buf*2 >= len_out)
        return 0;
    for (i = 0; i < len_buf; i++) {
        /* XXX: Account for endianess */
        snprintf(out+(i*2), len_out-(i*2), "%02x", buf[i]);
    }
    return i*2;
}

/*
 * Compare @search to @str and ensure at least one character from @delim
 * is present in @str. This ensures we don't false match on a command with
 * a common prefix.
 * Return scheme is similar to strcmp.
 */
static
int
gdb_cmdcmp(const char *str, const char *search, const char *delim)
{
    size_t str_len = strlen(str);
    size_t search_len = strlen(search);
    const char *delim_itr = delim;

    /* Compare strings normally */
    int res = strncmp(str, search, search_len);
    if (res != 0 || str_len <= search_len)
        return res;

    /* Check for delimiter to avoid prefix matching */
    do {
        if (*delim_itr == str[search_len])
            return 0;
    } while (++delim_itr);
    return -1;
}

static
drdbg_status_t
gdb_sendpkt(const char *buf, int len)
{
    do {
        dr_fprintf(STDERR, "SENDING PACKET %s\n", buf);
        int bread = -1;
        char *pkt = (char *)calloc(MAX_PACKET_SIZE, sizeof(char));
        pkt[0] = '$';
        memcpy(pkt+1, buf, len);
        pkt[1+len] = '#';
        snprintf(pkt+2+len, MAX_PACKET_SIZE-len-2, "%02x", gdb_chksum(buf, len));
        /* send packet */
        bread = send(drdbg_srv_gdb_conn, pkt, strlen(pkt), 0);
        if (bread != strlen(pkt)) {
            dr_fprintf(STDERR, "Didn't send all the bytes\n");
            return DRDBG_ERROR;
        }
        dr_fprintf(STDERR, "PACKET SENT!\n");
        free(pkt);
    } while (!gdb_recvack());
    return DRDBG_SUCCESS;
}

static
drdbg_status_t
gdb_recvpkt(char *buf, ssize_t len, ssize_t *bread)
{
    int ret;
    *bread = 0;

    while (*bread < len) {
        ret = recv(drdbg_srv_gdb_conn, buf+(*bread), 1, 0);
        if (ret == -1) {
            dr_fprintf(STDERR, "RECV ERROR %d\n", errno);
            gdb_sendack('-');
            return DRDBG_ERROR;
        }
        if (*(buf+(*bread)) == '#') {
            *bread += 1;
            ret = recv(drdbg_srv_gdb_conn, buf+(*bread), 2, 0);
            if (ret == -1) {
                dr_fprintf(STDERR, "CHKSUM ERROR %d\n", errno);
                gdb_sendack('-');

                return DRDBG_ERROR;
            }
            *bread += 2;
            gdb_sendack('+');
            buf[*bread] = '\0';
            return DRDBG_SUCCESS;
        }
        *bread += 1;
    }
    gdb_sendack('-');
    return DRDBG_ERROR;
}

/* Server API functions */
static
drdbg_status_t
drdbg_srv_gdb_accept(void)
{
    socklen_t client_len;
    dr_fprintf(STDERR, "WAITING ON CONN...\n");
    drdbg_srv_gdb_conn = accept(drdbg_srv_gdb_sock,
                                   (struct sockaddr *)&drdbg_srv_gdb_client_addr,
                                   &client_len);
    if (drdbg_srv_gdb_conn == -1) {
        dr_fprintf(STDERR, "FAILED TO ACCEPT CONN %d\n", errno);
        return DRDBG_ERROR;
    }
    while (!gdb_recvack());
    dr_fprintf(STDERR, "ACCEPTED CONNCTION!\n");
    return DRDBG_SUCCESS;
}

static
drdbg_status_t
drdbg_srv_gdb_start(uint port)
{
    struct sockaddr_in addr;

    /* Creat socket */
    drdbg_srv_gdb_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (drdbg_srv_gdb_sock == -1)
        return DRDBG_ERROR;

    /* Bind to socket with port */
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(drdbg_srv_gdb_sock, (struct sockaddr *)&addr,
             sizeof(struct sockaddr_in)) == -1) {
        close(drdbg_srv_gdb_sock);
        return DRDBG_ERROR;
    }

    /* Start listening on socket */
    if (listen(drdbg_srv_gdb_sock, 1) == -1) {
        close(drdbg_srv_gdb_sock);
        return DRDBG_ERROR;
    }

    return DRDBG_SUCCESS;
}

static
drdbg_status_t
drdbg_srv_gdb_stop(void)
{
    int ret = close(drdbg_srv_gdb_sock);
    if (ret == -1)
        return DRDBG_ERROR;
    ret = close(drdbg_srv_gdb_conn);
    if (ret == -1)
        return DRDBG_ERROR;
    return DRDBG_SUCCESS;
}

/* Command implementations */
static
drdbg_status_t
drdbg_srv_gdb_cmd_continue(int cmd_index, char *buf, int len,
                           drdbg_srv_int_cmd_t *cmd, void **cmd_args)
{
    unsigned int *tids = NULL;
    char *cur = buf;
    char *tmp = buf;
    int ctr = 0;
    const gdb_cmd_t *gdb_cmd = &SUPPORTED_CMDS[cmd_index];
    *cmd = gdb_cmd->cmd_id;

    // Specifying multiple actions is an error
    cur = buf+strlen(gdb_cmd->cmd_str);
    if (*cur != ':') {
        return DRDBG_ERROR;
    }

    // Fill in array of tids to continue
    tids = (unsigned int *)calloc(10, sizeof(unsigned int));
    while (*cur == ':') {
        // Advance to beginning of tid
        cur++;
        // Get tid and convert from BE hexstr to normal int
        tids[ctr] = (unsigned int)strtoul(cur,&tmp,16);
        if (tmp == cur && tids[ctr] == 0) {
            return DRDBG_ERROR;
        }
        tids[ctr] = END_SWAP_UINT32(tids[ctr]);
        // Advance to next delimiter
        cur = tmp;
    }
    *cmd_args = (void *)tids;
    return DRDBG_SUCCESS;
}

static
drdbg_status_t
drdbg_srv_gdb_cmd_query(char *buf, int len)
{
    dr_fprintf(STDERR,"QUERY COMMAND\n");
    if (!gdb_cmdcmp(buf+1, "qSupported", ":;?#")) {
        const char *pkt = "PacketSize=3fff;multiprocess+;vContSupported+";
        gdb_sendpkt(pkt,strlen(pkt));
    } else {
        gdb_sendpkt("", 0);
    }
    return DRDBG_SUCCESS;
}

static
drdbg_status_t
drdbg_srv_gdb_cmd_put_query_stop_rsn(drdbg_srv_int_cmd_t *cmd, void **cmd_args)
{
    typedef drdbg_cmd_data_query_stop_rsn_t mydata_t;
    mydata_t *data = (mydata_t *)*cmd_args;
    char *pkt = (char *)calloc(MAX_PACKET_SIZE, sizeof(char));
    ssize_t len = 0;

    switch (data->stop_rsn) {
    case DRDBG_STOP_RSN_RECV_SIG:
        len = snprintf(pkt, MAX_PACKET_SIZE, "S%02x", data->signum);\
        return gdb_sendpkt(pkt, len);
        break;
    default:
        break;
    }
    return DRDBG_ERROR;
}

static
drdbg_status_t
drdbg_srv_gdb_cmd_put_reg_read(drdbg_srv_int_cmd_t *cmd, void **cmd_args)
{
    typedef dr_mcontext_t mydata_t;
    mydata_t *data = (mydata_t *)*cmd_args;
    char *pkt = (char *)calloc(MAX_PACKET_SIZE, sizeof(char));
    ssize_t len = 0;
    ssize_t ret = 0;
    ret = snprintf(pkt, MAX_PACKET_SIZE, PFMT PFMT PFMT PFMT PFMT PFMT PFMT PFMT,
                   data->xax,data->xbx,data->xcx,data->xdx,data->xsi,data->xdi,
                   data->xbp,data->xsp);
    len += ret;
#ifdef X64
    ret = snprintf(pkt+len, MAX_PACKET_SIZE-len, PFMT PFMT PFMT PFMT PFMT PFMT PFMT PFMT,
                   data->r8, data->r9, data->r10, data->r11, data->r12,
                   data->r13, data->r14, data->r15);
    len += ret;
#endif
    ret = snprintf(pkt+len, MAX_PACKET_SIZE-len, PFMT PFMT,
                   (uint64)data->xip, data->xflags);
    len += ret;

    gdb_sendpkt(pkt, len);

    return DRDBG_ERROR;
}

static
drdbg_status_t
drdbg_srv_gdb_cmd_mem_read(char *buf, int len, void **cmd_args)
{
    typedef drdbg_cmd_data_mem_read_t my_data_t;
    my_data_t *data = dr_global_alloc(sizeof(my_data_t));
    sscanf(buf+2, PFMT","PFMT, (uint64*)&data->data, (uint64*)&data->len);
    *cmd_args = data;
    return DRDBG_SUCCESS;
}

static
drdbg_status_t
drdbg_srv_gdb_cmd_put_mem_read(drdbg_srv_int_cmd_t *cmd, void **cmd_args)
{
    typedef drdbg_cmd_data_mem_read_t my_data_t;
    char pkt[MAX_PACKET_SIZE];
    int len = 0;

    my_data_t *data = (my_data_t *)*cmd_args;
    len = gdb_hexify(pkt, MAX_PACKET_SIZE, data->data, data->len);

    gdb_sendpkt(pkt, len);

    return DRDBG_SUCCESS;
}

/* GDB Parsing functions */
static
drdbg_status_t
drdbg_srv_gdb_parse_cmd(char *buf, int len,
                        drdbg_srv_int_cmd_t *cmd, void **cmd_args)
{
    int i = 0;

    for (i = 0; i < NUM_SUPPORTED_CMDS; i++) {
        switch (buf[1]) {
        case DRDBG_GDB_CMD_PREFIX_MULTI:
            /* Multi-letter command */
            if (!gdb_cmdcmp(buf+1, SUPPORTED_CMDS[i].cmd_str, ";?#")) {
                return SUPPORTED_CMDS[i].func(i, buf, len, cmd, cmd_args);
            }
            break;
        case DRDBG_GDB_CMD_PREFIX_QUERY:
        case DRDBG_GDB_CMD_PREFIX_QUERY_SET:
            /* Query Command */
            *cmd = DRDBG_CMD_SERVER_INTERNAL;
            return drdbg_srv_gdb_cmd_query(buf, len);
            break;
        case 'g':
            *cmd = DRDBG_CMD_REG_READ;
            return DRDBG_SUCCESS;
        case 'm':
            *cmd = DRDBG_CMD_MEM_READ;
            return drdbg_srv_gdb_cmd_mem_read(buf, len, cmd_args);
            break;
        case '?':
            *cmd = DRDBG_CMD_QUERY_STOP_RSN;
            return DRDBG_SUCCESS;
        default:
            /* Normal command */
            break;
        }
    }
    /* Command not supported */
    gdb_sendpkt("", 0);
    return DRDBG_ERROR;
}

static
drdbg_status_t
drdbg_srv_gdb_get_cmd(drdbg_srv_int_cmd_t *cmd, void **cmd_args)
{
    char buf[MAX_PACKET_SIZE];
    ssize_t bread = 0;
    int chksum = 0;
    int ret = 0;

    if (drdbg_srv_gdb_conn == -1) {
        return DRDBG_ERROR;
    }

    /* recv packet */
    //bread = recv(drdbg_srv_gdb_conn, buf, MAX_PACKET_SIZE, 0);
    gdb_recvpkt(buf, MAX_PACKET_SIZE, &bread);
    dr_fprintf(STDERR, "Received packet '%s'\n", buf);
    if (bread == -1) {
        dr_fprintf(STDERR, "recv error %d\n", errno);
        return DRDBG_ERROR;
    }
    if (buf[0] != '$') {
        return DRDBG_ERROR;
    }
    /* verify checksum */
    ret = sscanf(buf, "%*[^#]#%x", &chksum);
    if (ret < 1 || (unsigned char)chksum != gdb_chksum(buf+1, bread-4)) {
        dr_fprintf(STDERR, "Invalid checksum %d vs %d\n", chksum, gdb_chksum(buf+1, bread-4));
        return DRDBG_ERROR;
    }
    /* Parse command */
    return drdbg_srv_gdb_parse_cmd(buf, bread, cmd, cmd_args);
}

static
drdbg_status_t
drdbg_srv_gdb_put_cmd(drdbg_srv_int_cmd_t *cmd, void **cmd_args)
{
    switch (*cmd) {
    case DRDBG_CMD_QUERY_STOP_RSN:
        return drdbg_srv_gdb_cmd_put_query_stop_rsn(cmd, cmd_args);
        break;
    case DRDBG_CMD_REG_READ:
        return drdbg_srv_gdb_cmd_put_reg_read(cmd, cmd_args);
        break;
    case DRDBG_CMD_MEM_READ:
        return drdbg_srv_gdb_cmd_put_mem_read(cmd, cmd_args);
        break;
    default:
        break;
    }
    return DRDBG_ERROR;
}

drdbg_status_t
drdbg_srv_gdb_init(drdbg_srv_int_t *dbg_server)
{
    /* Server management */
    dbg_server->start = drdbg_srv_gdb_start;
    dbg_server->accept = drdbg_srv_gdb_accept;
    dbg_server->stop = drdbg_srv_gdb_stop;
    dbg_server->get_cmd = drdbg_srv_gdb_get_cmd;
    dbg_server->put_cmd = drdbg_srv_gdb_put_cmd;

    return DRDBG_SUCCESS;
}

const gdb_cmd_t SUPPORTED_CMDS[NUM_SUPPORTED_CMDS] =
    {
        {DRDBG_CMD_CONTINUE, "vCont", drdbg_srv_gdb_cmd_continue}
    };