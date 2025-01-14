/*
 * Copyright (c) 2017 Mellanox Technologies, Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap/pcap.h>
#include <unistd.h>
#include <fcntl.h>
#include <libmnl/libmnl.h>
#include <linux/psample.h>
#include <linux/genetlink.h>
#include <linux/filter.h>
#include <linux/if_arp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <psample.h>
#include "mnlg.h"

#define LOG(level, ...) \
		psample_log(level, __FILE__, __LINE__, __func__, __VA_ARGS__)

#define LOG_DEBUG(...) LOG(PSAMPLE_LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...)  LOG(PSAMPLE_LOG_INFO, __VA_ARGS__)
#define LOG_WARN(...)  LOG(PSAMPLE_LOG_WARN, __VA_ARGS__)
#define LOG_ERR(...)   LOG(PSAMPLE_LOG_ERR, __VA_ARGS__)

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static void logfn_stderr(enum psample_log_level level, const char *file,
			 int line, const char *fn, const char *format,
			 va_list args);

enum psample_log_level psample_loglevel = PSAMPLE_LOG_WARN;
logfn psample_logfunc = logfn_stderr;

struct psample_msg {
	struct nlattr **tb;
};

struct psample_config {
	__u8 cmd;
	struct nlattr **tb;
};

struct psample_pcap {
	long snaplen;
	char *pcap_buf;
	pcap_t *pcap_handle;
	pcap_dumper_t *pcap_dumper;
};

struct psample_handle {
	struct mnlg_socket *sample_nlh;
	struct mnlg_socket *control_nlh;
	struct sock_fprog sample_filter_fprog;
	struct psample_pcap psample_pcap;
};

void psample_set_log_level(enum psample_log_level level)
{
	psample_loglevel = level;
}

enum psample_log_level get_log_level(void)
{
	return psample_loglevel;
}

void psample_set_log_func(logfn func)
{
	psample_logfunc = func;
}

static const char *loglevel_str(enum psample_log_level level)
{
	switch (level) {
	case PSAMPLE_LOG_DEBUG:
		return "DEBUG";
	case PSAMPLE_LOG_INFO:
		return "INFO";
	case PSAMPLE_LOG_WARN:
		return "WARN";
	case PSAMPLE_LOG_ERR:
		return "ERROR";
	default:
		return "UNKNOWN";
	}
}

static void logfn_stderr(enum psample_log_level level, const char *file,
			 int line, const char *fn, const char *format,
			 va_list args)
{
	fprintf(stderr, "libpsample %s %s: ", loglevel_str(level), fn);
	vfprintf(stderr, format, args);
	fprintf(stderr, "\n");
}

void psample_log(enum psample_log_level level,
		 const char *file, int line, const char *fn,
		 const char *format, ...)
{
	va_list args;

	va_start(args, format);
	if (level >= psample_loglevel)
		psample_logfunc(level, file, line, fn, format, args);
	va_end(args);
}

struct psample_handle *psample_open()
{
	struct psample_handle *handle;
	int err;

	handle = (struct psample_handle *)calloc(sizeof(*handle), 1);
	if (!handle) {
		LOG_ERR("Could not allocate memory");
		return NULL;
	}

	handle->sample_nlh = mnlg_socket_open(PSAMPLE_GENL_NAME,
					      PSAMPLE_GENL_VERSION);
	if (!handle->sample_nlh) {
		LOG_ERR("Could not open netlink socket");
		free(handle);
		return NULL;
	}

	err = mnlg_socket_group_add(handle->sample_nlh,
				    PSAMPLE_NL_MCGRP_CONFIG_NAME);
	if (err < 0) {
		LOG_ERR("Could not bind to config multicast group");
		mnlg_socket_close(handle->sample_nlh);
		free(handle);
		return NULL;
	}

	err = mnlg_socket_group_add(handle->sample_nlh,
				    PSAMPLE_NL_MCGRP_SAMPLE_NAME);
	if (err < 0) {
		LOG_ERR("Could not bind to sample multicast group");
		mnlg_socket_close(handle->sample_nlh);
		free(handle);
		return NULL;
	}

	handle->control_nlh = mnlg_socket_open(PSAMPLE_GENL_NAME,
					       PSAMPLE_GENL_VERSION);
	if (!handle->control_nlh) {
		LOG_ERR("Could not open control nlsock");
		mnlg_socket_close(handle->sample_nlh);
		free(handle);
		return NULL;
	}

	return handle;
}

void psample_close(struct psample_handle *handle)
{
	if (!handle)
		return;

	mnlg_socket_close(handle->sample_nlh);
	mnlg_socket_close(handle->control_nlh);

	if (handle->sample_filter_fprog.filter)
		free(handle->sample_filter_fprog.filter);
	free(handle);
}

/* Based on https://www.tcpdump.org/linktypes/LINKTYPE_LINUX_SLL.html */
struct linux_sll {
	__be16 pkttype;
	__be16 hatype;
	__be16 halen;
	unsigned char addr[8];
	__be16 family;
};

static int psample_pcap_buf_init(struct psample_handle *handle)
{
	struct linux_sll sll;

	/* The wireshark netlink dissector expects netlink messages to start
	 * with a Linux cooked header (SLL), so include it before each packet.
	 */
	memset(&sll, 0, sizeof(sll));
	sll.pkttype = htons(PACKET_OUTGOING);
	sll.hatype = htons(ARPHRD_NETLINK);
	sll.family = htons(AF_NETLINK);

	handle->psample_pcap.pcap_buf = calloc(1, handle->psample_pcap.snaplen);
	if (!handle->psample_pcap.pcap_buf) {
		perror("calloc failed");
		return -1;
	}

	memcpy(handle->psample_pcap.pcap_buf, &sll, sizeof(sll));

	return 0;
}

static void psample_pcap_buf_fini(struct psample_handle *handle)
{
	free(handle->psample_pcap.pcap_buf);
}

static void psample_pcap_write(void *data, unsigned char *buf, int len)
{
	struct psample_handle *handle = data;
	struct pcap_pkthdr hdr;
	int pkt_len;

	if (len + sizeof(struct linux_sll) < handle->psample_pcap.snaplen)
		pkt_len = len + sizeof(struct linux_sll);
	else
		pkt_len = handle->psample_pcap.snaplen - sizeof(struct linux_sll);

	memcpy(handle->psample_pcap.pcap_buf + sizeof(struct linux_sll), buf, pkt_len);

	hdr.caplen = pkt_len;
	hdr.len = pkt_len;
	gettimeofday(&hdr.ts, NULL);

	pcap_dump((unsigned char *) handle->psample_pcap.pcap_dumper, &hdr,
		  (const unsigned char *) handle->psample_pcap.pcap_buf);
	/* In case packets are written to stdout, make sure each packet is
	 * immediately written and not buffered.
	 */
	fflush(NULL);
}

static int psample_pcap_genl_init(struct psample_handle *handle)
{
	/* In order for wireshark to be able to invoke the psample dissector,
	 * it must learn about the mapping between the generic netlink
	 * family ID and its name from this dump.
	 *
	 * Reference:
	 * https://www.wireshark.org/lists/wireshark-users/201907/msg00027.html
	 */
	struct nlmsghdr *nlh;
	int err;

	nlh = mnlg_msg_prepare(handle->sample_nlh, CTRL_CMD_GETFAMILY,
			       NLM_F_REQUEST | NLM_F_ACK, GENL_ID_CTRL, 1);

	mnl_attr_put_u16(nlh, CTRL_ATTR_FAMILY_ID, handle->sample_nlh->id);
	err = mnlg_socket_send(handle->sample_nlh, nlh);
	if (err < 0)
		return err;

	do {
		err = mnl_socket_recvfrom(handle->sample_nlh->nl,
					  handle->sample_nlh->buf,
					  MNL_SOCKET_BUFFER_SIZE);
		if (err <= 0)
			break;

		psample_pcap_write(handle, handle->sample_nlh->buf,
				   MNL_SOCKET_BUFFER_SIZE);

	} while (err > 0);

	return err;
}

int psample_pcap_init(const char *out_file, struct psample_handle *handle)
{
	int err;

	handle->psample_pcap.snaplen = 0xffff;

	handle->psample_pcap.pcap_handle =
		pcap_open_dead(DLT_NETLINK, handle->psample_pcap.snaplen);
	if (!handle->psample_pcap.pcap_handle) {
		perror("pcap_open_dead");
		return -1;
	}

	handle->psample_pcap.pcap_dumper =
		pcap_dump_open(handle->psample_pcap.pcap_handle, out_file);
	if (!handle->psample_pcap.pcap_dumper) {
		pcap_perror(handle->psample_pcap.pcap_handle, "pcap_dump_open");
		goto err_dump_open;
	}

	err = psample_pcap_buf_init(handle);
	if (err)
		goto err_buf_init;

	err = psample_pcap_genl_init(handle);
	if (err) {
		fprintf(stderr, "Failed to dump generic netlink families\n");
		goto err_genl_init;
	}

	return 0;

err_genl_init:
	psample_pcap_buf_fini(handle);
err_buf_init:
	pcap_dump_close(handle->psample_pcap.pcap_dumper);
err_dump_open:
	pcap_close(handle->psample_pcap.pcap_handle);
	return -1;
}

void psample_pcap_fini(struct psample_handle *handle)
{
	psample_pcap_buf_fini(handle);
	pcap_dump_close(handle->psample_pcap.pcap_dumper);
	pcap_close(handle->psample_pcap.pcap_handle);
}

static int attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type;

	type = mnl_attr_get_type(attr);

	if (mnl_attr_type_valid(attr, PSAMPLE_ATTR_MAX) < 0)
		return MNL_CB_ERROR;

	if (type == PSAMPLE_ATTR_IIFINDEX &&
	    mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
		return MNL_CB_ERROR;
	if (type == PSAMPLE_ATTR_OIFINDEX &&
	    mnl_attr_validate(attr, MNL_TYPE_U16) < 0)
		return MNL_CB_ERROR;
	if (type == PSAMPLE_ATTR_SAMPLE_RATE &&
	    mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
		return MNL_CB_ERROR;
	if (type == PSAMPLE_ATTR_ORIGSIZE &&
	    mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
		return MNL_CB_ERROR;
	if (type == PSAMPLE_ATTR_SAMPLE_GROUP &&
	    mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
		return MNL_CB_ERROR;
	if (type == PSAMPLE_ATTR_GROUP_SEQ &&
	    mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
		return MNL_CB_ERROR;
	if (type == PSAMPLE_ATTR_GROUP_REFCOUNT &&
	    mnl_attr_validate(attr, MNL_TYPE_U32) < 0)
		return MNL_CB_ERROR;

	tb[type] = attr;
	return MNL_CB_OK;
}

struct psample_event_handler_data {
	psample_msg_cb msg_cb;
	psample_config_cb config_cb;
	void *config_cb_data;
	void *msg_cb_data;
	int cb_retval;
};

static int psample_event_handler(const struct nlmsghdr *nlhdr, void *data)
{
	struct psample_event_handler_data *event_handler_data = data;
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlhdr);
	struct nlattr *tb[PSAMPLE_ATTR_MAX + 1] = {};
	int ret;

	mnl_attr_parse(nlhdr, sizeof(struct genlmsghdr), attr_cb, tb);

	if ((genl->cmd == PSAMPLE_CMD_SAMPLE) && event_handler_data->msg_cb) {
		void *cb_data = event_handler_data->msg_cb_data;
		struct psample_msg msg;

		msg.tb = tb;
		ret = event_handler_data->msg_cb(&msg, cb_data);
	} else if (event_handler_data->config_cb) {
		void *cb_data = event_handler_data->config_cb_data;
		struct psample_config config;

		config.tb = tb;
		config.cmd = genl->cmd;
		ret = event_handler_data->config_cb(&config, cb_data);
	} else {
		return MNL_CB_OK;
	}

	event_handler_data->cb_retval = ret;
	if (ret != 0)
		return MNL_CB_STOP;

	return MNL_CB_OK;
}

static struct sock_filter psample_group_filter[] = {
	BPF_STMT(BPF_LD + BPF_IMM, sizeof(struct nlmsghdr) +
				   sizeof(struct genlmsghdr)),
	BPF_STMT(BPF_LDX + BPF_IMM, PSAMPLE_ATTR_SAMPLE_GROUP),
	BPF_STMT(BPF_LD + BPF_ABS, SKF_AD_OFF + SKF_AD_NLATTR),
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0, 4, 0),		 /* pass */
	BPF_STMT(BPF_MISC + BPF_TAX, 0),
	BPF_STMT(BPF_LD + BPF_W + BPF_IND, 4),

	/* This command should be edited with the right group value */
#define FILTER_GROUP_COMMAND 6
	BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, 0x38000000, 1, 0),	 /* pass */

	/* drop */
	BPF_STMT(BPF_RET + BPF_K, (u_int) 0),

	/* pass */
	BPF_STMT(BPF_RET + BPF_K, (u_int) -1),
};

int psample_bind_group(struct psample_handle *handle, int group)
{
	struct sock_fprog *fprog;
	int err;
	int fd;

	if (!handle) {
		LOG_ERR("Called with invalid handle");
		return -EINVAL;
	}

	fd = mnlg_socket_get_fd(handle->sample_nlh);

	fprog = &handle->sample_filter_fprog;
	if (fprog->filter) {
		err = setsockopt(fd, SOL_SOCKET, SO_DETACH_FILTER,
				 fprog, sizeof(*fprog));
		if (err) {
			LOG_ERR("Could not detach filter prog: %s",
				strerror(errno));
			return -errno;
		}

		free(fprog->filter);
	}

	fprog->filter = malloc(sizeof(psample_group_filter));
	memcpy(fprog->filter, psample_group_filter,
	       sizeof(psample_group_filter));
	fprog->filter[FILTER_GROUP_COMMAND].k = ntohl(group);
	fprog->len = ARRAY_SIZE(psample_group_filter);

	err = setsockopt(fd, SOL_SOCKET, SO_ATTACH_FILTER, fprog,
			 sizeof(*fprog));
	if (err) {
		LOG_ERR("Could not attach filter prog: %s", strerror(errno));
		return -errno;
	}

	return 0;
}

static int psample_set_blocking(struct psample_handle *handle, bool block)
{
	int fd;
	int flags;

	fd = mnlg_socket_get_fd(handle->sample_nlh);
	flags = fcntl(fd, F_GETFL);
	if (!block)
		flags |= O_NONBLOCK;
	else
		flags &= ~O_NONBLOCK;

	if (fcntl(fd, F_SETFL, flags) < 0) {
		LOG_ERR("Could not set O_NONBLOCK: %s", strerror(errno));
		return -1;
	}

	return 0;
}

int psample_dispatch(struct psample_handle *handle, psample_msg_cb msg_cb,
		     void *msg_data, psample_config_cb config_cb,
		     void *config_data, bool block)
{
	struct psample_event_handler_data event_handler_data;
	int err;

	if (!handle) {
		LOG_ERR("handle not initalized");
		return -ENOMEM;
	}

	event_handler_data.msg_cb = msg_cb;
	event_handler_data.msg_cb_data = msg_data;
	event_handler_data.config_cb = config_cb;
	event_handler_data.config_cb_data = config_data;

	psample_set_blocking(handle, block);
	err = mnlg_socket_recv_run(handle->sample_nlh, psample_event_handler,
				   &event_handler_data);
	if (err < 0) {
		if (errno != EWOULDBLOCK || block) {
			LOG_ERR("Could not recv: %s", strerror(errno));
			return -errno;
		}
	}

	return event_handler_data.cb_retval;
}

static int psample_socket_recv_write(struct mnlg_socket *nlg, void* data_write)
{
	int err;

	do {
		err = mnl_socket_recvfrom(nlg->nl, nlg->buf,
					  MNL_SOCKET_BUFFER_SIZE);
		if (err <= 0)
			break;

		psample_pcap_write(data_write, nlg->buf,
				   MNL_SOCKET_BUFFER_SIZE);

	} while (err > 0);

	return err;
}

int psample_write_pcap_dispatch(struct psample_handle *handle)
{
	int err;

	if (!handle) {
		LOG_ERR("handle not initalized");
		return -ENOMEM;
	}

	psample_set_blocking(handle, true);
	err = psample_socket_recv_write(handle->sample_nlh, handle);
	if (err < 0) {
		LOG_ERR("Could not recv: %s", strerror(errno));
		return -errno;
	}

	return 0;
}

struct psample_group_handler_data {
	psample_group_cb cb;
	void *cb_data;
	int cb_retval;
};

static int group_handle(const struct nlmsghdr *nlhdr, void *data)
{
	struct psample_group_handler_data *group_handler_data = data;
	struct nlattr *tb[PSAMPLE_ATTR_MAX + 1] = {};
	struct genlmsghdr *genl = mnl_nlmsg_get_payload(nlhdr);
	struct psample_group group;
	int ret;

	mnl_attr_parse(nlhdr, sizeof(*genl), attr_cb, tb);
	if (!tb[PSAMPLE_ATTR_SAMPLE_GROUP] ||
	    !tb[PSAMPLE_ATTR_GROUP_REFCOUNT] ||
	    !tb[PSAMPLE_ATTR_GROUP_SEQ])
		return MNL_CB_ERROR;

	group.num = mnl_attr_get_u32(tb[PSAMPLE_ATTR_SAMPLE_GROUP]);
	group.refcount = mnl_attr_get_u32(tb[PSAMPLE_ATTR_GROUP_REFCOUNT]);
	group.seq = mnl_attr_get_u32(tb[PSAMPLE_ATTR_GROUP_SEQ]);

	ret = group_handler_data->cb(&group, &group_handler_data->cb_data);
	group_handler_data->cb_retval = ret;
	if (ret != 0)
		return MNL_CB_STOP;

	return MNL_CB_OK;
}

int psample_group_foreach(struct psample_handle *handle,
			  psample_group_cb group_cb, void *data)
{
	uint16_t flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_DUMP;
	struct psample_group_handler_data group_handler_data;
	struct nlmsghdr *nlhdr;
	int err;

	nlhdr = mnlg_msg_prepare(handle->control_nlh, PSAMPLE_CMD_GET_GROUP,
				 flags, handle->control_nlh->id,
				 handle->control_nlh->version);

	err = mnlg_socket_send(handle->control_nlh, nlhdr);
	if (err < 0) {
		LOG_ERR("failed to call mnlg_socket_send: %s", strerror(errno));
		return -errno;
	}

	group_handler_data.cb = group_cb;
	group_handler_data.cb_data = data;

	err = mnlg_socket_recv_run(handle->control_nlh, group_handle,
				   &group_handler_data);
	if (err < 0) {
		LOG_ERR("failed to recv message: %s", strerror(errno));
		return -errno;
	}

	return group_handler_data.cb_retval;
}

bool psample_msg_group_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_SAMPLE_GROUP];
}

bool psample_msg_rate_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_SAMPLE_RATE];
}

bool psample_msg_iif_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_IIFINDEX];
}

bool psample_msg_oif_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_OIFINDEX];
}

bool psample_msg_origsize_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_ORIGSIZE];
}

bool psample_msg_seq_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_GROUP_SEQ];
}

bool psample_msg_data_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_DATA];
}

bool psample_msg_out_tc_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_OUT_TC];
}

bool psample_msg_out_tc_occ_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_OUT_TC_OCC];
}

bool psample_msg_latency_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_LATENCY];
}

bool psample_msg_timestamp_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_TIMESTAMP];
}

bool psample_msg_proto_exist(const struct psample_msg *msg)
{
	return msg->tb[PSAMPLE_ATTR_PROTO];
}

__u32 psample_msg_group(const struct psample_msg *msg)
{
	return mnl_attr_get_u32(msg->tb[PSAMPLE_ATTR_SAMPLE_GROUP]);
}

__u32 psample_msg_rate(const struct psample_msg *msg)
{
	return mnl_attr_get_u32(msg->tb[PSAMPLE_ATTR_SAMPLE_RATE]);
}

__u16 psample_msg_iif(const struct psample_msg *msg)
{
	return mnl_attr_get_u16(msg->tb[PSAMPLE_ATTR_IIFINDEX]);
}

__u16 psample_msg_oif(const struct psample_msg *msg)
{
	return mnl_attr_get_u16(msg->tb[PSAMPLE_ATTR_OIFINDEX]);
}

__u32 psample_msg_origsize(const struct psample_msg *msg)
{
	return mnl_attr_get_u32(msg->tb[PSAMPLE_ATTR_ORIGSIZE]);
}

__u32 psample_msg_seq(const struct psample_msg *msg)
{
	return mnl_attr_get_u32(msg->tb[PSAMPLE_ATTR_GROUP_SEQ]);
}

__u32 psample_msg_data_len(const struct psample_msg *msg)
{
	return mnl_attr_get_payload_len(msg->tb[PSAMPLE_ATTR_DATA]);
}

__u8 *psample_msg_data(const struct psample_msg *msg)
{
	return mnl_attr_get_payload(msg->tb[PSAMPLE_ATTR_DATA]);
}

__u16 psample_msg_out_tc(const struct psample_msg *msg)
{
	return mnl_attr_get_u16(msg->tb[PSAMPLE_ATTR_OUT_TC]);
}

__u64 psample_msg_out_tc_occ(const struct psample_msg *msg)
{
	return mnl_attr_get_u64(msg->tb[PSAMPLE_ATTR_OUT_TC_OCC]);
}

__u64 psample_msg_latency(const struct psample_msg *msg)
{
	return mnl_attr_get_u64(msg->tb[PSAMPLE_ATTR_LATENCY]);
}

__u64 psample_msg_timestamp(const struct psample_msg *msg)
{
	return mnl_attr_get_u64(msg->tb[PSAMPLE_ATTR_TIMESTAMP]);
}

__u16 psample_msg_proto(const struct psample_msg *msg)
{
	return mnl_attr_get_u16(msg->tb[PSAMPLE_ATTR_PROTO]);
}

bool psample_config_group_exist(const struct psample_config *config)
{
	return config->tb[PSAMPLE_ATTR_SAMPLE_GROUP];
}

bool psample_config_group_seq_exist(const struct psample_config *config)
{
	return config->tb[PSAMPLE_ATTR_GROUP_SEQ];
}

bool psample_config_group_refcount_exist(const struct psample_config *config)
{
	return config->tb[PSAMPLE_ATTR_GROUP_REFCOUNT];
}

__u8 psample_config_cmd(const struct psample_config *config)
{
	return config->cmd;
}

__u32 psample_config_group(const struct psample_config *config)
{
	return mnl_attr_get_u32(config->tb[PSAMPLE_ATTR_SAMPLE_GROUP]);
}

__u32 psample_config_group_seq(const struct psample_config *config)
{
	return mnl_attr_get_u32(config->tb[PSAMPLE_ATTR_GROUP_SEQ]);
}

__u32 psample_config_group_refcount(const struct psample_config *config)
{
	return mnl_attr_get_u32(config->tb[PSAMPLE_ATTR_GROUP_REFCOUNT]);
}
