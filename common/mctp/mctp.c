#include "mctp.h"
#include <logging/log.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/printk.h>
#include <zephyr.h>

LOG_MODULE_REGISTER(mctp);

typedef struct __attribute__((packed)) {
	uint8_t hdr_ver;
	uint8_t dest_ep;
	uint8_t src_ep;
	union {
		struct {
			uint8_t msg_tag : 3;
			uint8_t to : 1;
			uint8_t pkt_seq : 2;
			uint8_t eom : 1;
			uint8_t som : 1;
		};
		uint8_t flags_seq_to_tag;
	};
} mctp_hdr;

/* set thread name */
static uint8_t set_thread_name(mctp *mctp_inst)
{
	if (!mctp_inst)
		return MCTP_ERROR;

	if (mctp_inst->medium_type <= MCTP_MEDIUM_TYPE_UNKNOWN ||
	    mctp_inst->medium_type >= MCTP_MEDIUM_TYPE_MAX)
		return MCTP_ERROR;

	if (mctp_inst->medium_type == MCTP_MEDIUM_TYPE_SMBUS) {
		mctp_smbus_conf *smbus_conf = (mctp_smbus_conf *)&mctp_inst->medium_conf;
		snprintf(mctp_inst->mctp_rx_task_name, sizeof(mctp_inst->mctp_rx_task_name),
			 "mctprx_%02x_%02x_%02x", mctp_inst->medium_type, smbus_conf->bus,
			 smbus_conf->addr);
		snprintf(mctp_inst->mctp_tx_task_name, sizeof(mctp_inst->mctp_tx_task_name),
			 "mctptx_%02x_%02x_%02x", mctp_inst->medium_type, smbus_conf->bus,
			 smbus_conf->addr);
	}

	return MCTP_SUCCESS;
}

/* init the medium related resources */
static uint8_t mctp_medium_init(mctp *mctp_inst, mctp_medium_conf medium_conf)
{
	if (!mctp_inst)
		return MCTP_ERROR;

	uint8_t ret = MCTP_ERROR;
	switch (mctp_inst->medium_type) {
	case MCTP_MEDIUM_TYPE_SMBUS:
		ret = mctp_smbus_init(mctp_inst, medium_conf);
		break;
	default:
		break;
	}

	return ret;
}

static uint8_t mctp_medium_deinit(mctp *mctp_inst)
{
	if (!mctp_inst)
		return MCTP_ERROR;

	switch (mctp_inst->medium_type) {
	case MCTP_MEDIUM_TYPE_SMBUS:
		mctp_smbus_deinit(mctp_inst);
		break;
	default:
		break;
	}

	return MCTP_SUCCESS;
}

static uint8_t bridge_msg(mctp *mctp_inst, uint8_t *buf, uint16_t len)
{
	if (!mctp_inst || !buf || !len)
		return MCTP_ERROR;

	if (!mctp_inst->ep_resolve)
		return MCTP_ERROR;

	mctp *target_mctp = NULL;
	mctp_ext_params target_ext_params;
	memset(&target_ext_params, 0, sizeof(target_ext_params));

	mctp_hdr *hdr = (mctp_hdr *)buf;
	uint8_t ret =
		mctp_inst->ep_resolve(hdr->dest_ep, (void **)&target_mctp, &target_ext_params);
	if (ret != MCTP_SUCCESS) {
		LOG_ERR("can't bridge endpoint %x", hdr->dest_ep);
		return MCTP_ERROR;
	}

	LOG_DBG("ret = %d, bridget msg to mctp = %p", ret, target_mctp);
	return mctp_bridge_msg(target_mctp, buf, len, target_ext_params);
}

static uint8_t mctp_pkt_assembling(mctp *mctp_inst, uint8_t *buf, uint16_t len)
{
	if (!mctp_inst || !buf || !len)
		return MCTP_ERROR;

	mctp_hdr *hdr = (mctp_hdr *)buf;
	uint8_t **buf_p = &mctp_inst->temp_msg_buf[hdr->msg_tag].buf;
	uint16_t *offset_p = &mctp_inst->temp_msg_buf[hdr->msg_tag].offset;

	/* one packet message, do nothing */
	if (hdr->som && hdr->eom)
		return MCTP_SUCCESS;
	/* first packet, allocate memory to hold data */
	if (hdr->som && !hdr->eom) {
		if (*buf_p)
			free(*buf_p);
		*offset_p = 0;

		*buf_p = (uint8_t *)malloc(MSG_ASSEMBLY_BUF_SIZE);
		if (!*buf_p) {
			LOG_WRN("cannot create memory...\n");
			return MCTP_ERROR;
		}
		memset(*buf_p, 0, MSG_ASSEMBLY_BUF_SIZE);
	}

	/* Appending other packet after the first packet */
	memcpy(*buf_p + *offset_p, buf + sizeof(hdr), len - sizeof(hdr));
	*offset_p += len - sizeof(hdr);

	return MCTP_SUCCESS;
}

/* mctp rx task */
static void mctp_rx_task(void *arg, void *dummy0, void *dummy1)
{
	ARG_UNUSED(dummy0);
	ARG_UNUSED(dummy1);
	if (!arg) {
		LOG_WRN("mctp_rx_task without mctp_inst!");
		return;
	}

	mctp *mctp_inst = (mctp *)arg;
	if (!mctp_inst->read_data) {
		LOG_WRN("mctp_rx_task without medium read function!");
		return;
	}

	LOG_INF("mctp_rx_task start %p", mctp_inst);

	while (1) {
		uint8_t read_buf[256] = { 0 };
		mctp_ext_params ext_params;
		uint8_t ret = MCTP_ERROR;
		memset(&ext_params, 0, sizeof(ext_params));

		uint16_t read_len =
			mctp_inst->read_data(mctp_inst, read_buf, sizeof(read_buf), &ext_params);

		if (!read_len)
			continue;

		LOG_HEXDUMP_DBG(read_buf, read_len, "mctp receive data");

		mctp_hdr *hdr = (mctp_hdr *)read_buf;
		LOG_DBG("dest_ep = %x, src_ep = %x, flags = %x\n", hdr->dest_ep, hdr->src_ep,
			hdr->flags_seq_to_tag);

		/* Set the tranport layer extra parameters */
		ext_params.msg_tag = hdr->msg_tag;
		/*
* The high-level application won't modify the tag_owner flag, change the
* tag_owner for response if needs
*/
		ext_params.tag_owner = 0;
		ext_params.ep = hdr->src_ep;

		if ((hdr->dest_ep != mctp_inst->endpoint) && (hdr->dest_ep != MCTP_NULL_EID)) {
			/* try to bridge this packet */
			ret = bridge_msg(mctp_inst, read_buf, read_len);
			if (ret == MCTP_ERROR)
				LOG_WRN("Bridge to endpoint 0x%x failed ", hdr->dest_ep);
			continue;
		}

		/* handle this packet by self */

		/* assembling the mctp message */
		if (mctp_pkt_assembling(mctp_inst, read_buf, read_len) == MCTP_ERROR)
			LOG_WRN("Packet assemble failed ");

		/* if it is not last packet, waiting for the remain data */
		if (!hdr->eom)
			continue;

		if (mctp_inst->rx_cb) {
			/* default process read data buffer directly */
			uint8_t *p = read_buf + sizeof(hdr);
			uint16_t len = read_len - sizeof(hdr);
			/* this is assembly message */
			if (mctp_inst->temp_msg_buf[hdr->msg_tag].buf) {
				p = mctp_inst->temp_msg_buf[hdr->msg_tag].buf;
				len = mctp_inst->temp_msg_buf[hdr->msg_tag].offset;

				LOG_HEXDUMP_DBG(p, len, "mctp assembly data");
			}

			/* handle the mctp messsage */
			mctp_inst->rx_cb(mctp_inst, p, len, ext_params);
		}

		if (mctp_inst->temp_msg_buf[hdr->msg_tag].buf) {
			free(mctp_inst->temp_msg_buf[hdr->msg_tag].buf);
			mctp_inst->temp_msg_buf[hdr->msg_tag].buf = NULL;
			mctp_inst->temp_msg_buf[hdr->msg_tag].offset = 0;
		}
	}
}

/* mctp tx task */
static void mctp_tx_task(void *arg, void *dummy0, void *dummy1)
{
	ARG_UNUSED(dummy0);
	ARG_UNUSED(dummy1);
	if (!arg) {
		LOG_WRN("mctp_tx_task without mctp_inst!");
		return;
	}

	mctp *mctp_inst = (mctp *)arg;

	if (!mctp_inst->write_data) {
		LOG_WRN("mctp_tx_task without medium write function!");
		return;
	}

	LOG_INF("mctp_tx_task start %p ", mctp_inst);

	while (1) {
		mctp_tx_msg mctp_msg = { 0 };
		int ret = k_msgq_get(&mctp_inst->mctp_tx_queue, &mctp_msg, K_FOREVER);
		if (ret)
			continue;

		if (!mctp_msg.buf)
			continue;

		if (!mctp_msg.len) {
			free(mctp_msg.buf);
			continue;
		}

		LOG_DBG("tx endpoint %x", mctp_msg.ext_params.ep);
		LOG_HEXDUMP_DBG(mctp_msg.buf, mctp_msg.len, "mctp tx task receive data");

		/*
* The bridge meesage already has the mctp transport header, and the bridge
     * message also doesn't need to split packet.
*/
		if (mctp_msg.is_bridge_packet) {
			mctp_inst->write_data(mctp_inst, mctp_msg.buf, mctp_msg.len,
					      mctp_msg.ext_params);
			free(mctp_msg.buf);
			continue;
		}

		/* Setup MCTP header and send to destination endpoint */
		static uint8_t msg_tag;
		uint16_t max_msg_size = mctp_inst->max_msg_size;
		uint8_t i;
		uint8_t split_pkt_num =
			(mctp_msg.len / max_msg_size) + ((mctp_msg.len % max_msg_size) ? 1 : 0);
		LOG_DBG("mctp_msg.len = %d", mctp_msg.len);
		LOG_DBG("split_pkt_num = %d", split_pkt_num);
		for (i = 0; i < split_pkt_num; i++) {
			uint8_t buf[max_msg_size + MCTP_TRANSPORT_HEADER_SIZE];
			mctp_hdr *hdr = (mctp_hdr *)buf;
			uint8_t cp_msg_size = max_msg_size;

			memset(buf, 0, sizeof(buf));

			/* The first packet should set SOM */
			if (!i)
				hdr->som = 1;

			/* The last packet should set EOM */
			if (i == (split_pkt_num - 1)) {
				hdr->eom = 1;
				uint8_t remain = mctp_msg.len % max_msg_size;
				cp_msg_size = remain ? remain : max_msg_size; /* remain data */
			}

			hdr->to = mctp_msg.ext_params.tag_owner;
			hdr->pkt_seq = i & MCTP_HDR_SEQ_MASK;

			/*
* TODO: should avoid the msg_tag if there are pending mctp
* response packets?
       * If the message is response, keep the original msg_tag of ext_params
*/
			hdr->msg_tag = (hdr->to) ? (msg_tag & MCTP_HDR_TAG_MASK) :
							 mctp_msg.ext_params.msg_tag;

			hdr->dest_ep = mctp_msg.ext_params.ep;
			hdr->src_ep = mctp_inst->endpoint;
			hdr->hdr_ver = MCTP_HDR_HDR_VER;

			LOG_DBG("i = %d, cp_msg_size = %d", i, cp_msg_size);
			LOG_DBG("hdr->flags_seq_to_tag = %x", hdr->flags_seq_to_tag);
			memcpy(buf + MCTP_TRANSPORT_HEADER_SIZE, mctp_msg.buf + i * max_msg_size,
			       cp_msg_size);
			mctp_inst->write_data(mctp_inst, buf,
					      cp_msg_size + MCTP_TRANSPORT_HEADER_SIZE,
					      mctp_msg.ext_params);
		}

		free(mctp_msg.buf);

		/* Only request mctp message needs to increase msg_tag */
		if (mctp_msg.ext_params.tag_owner)
			msg_tag++;
	}
}

/* mctp handle initial */
mctp *mctp_init(void)
{
	mctp *mctp_inst = (mctp *)malloc(sizeof(*mctp_inst));

	if (!mctp_inst)
		return NULL;

	memset(mctp_inst, 0, sizeof(*mctp_inst));
	mctp_inst->medium_type = MCTP_MEDIUM_TYPE_UNKNOWN;
	mctp_inst->max_msg_size = MCTP_DEFAULT_MSG_MAX_SIZE;
	mctp_inst->endpoint = MCTP_DEFAULT_ENDPOINT;

	LOG_DBG("mctp_inst = %p", mctp_inst);
	return mctp_inst;
}

/* mctp handle deinitial */
uint8_t mctp_deinit(mctp *mctp_inst)
{
	if (!mctp_inst)
		return MCTP_ERROR;

	LOG_DBG("mctp_inst = %p", mctp_inst);

	mctp_stop(mctp_inst);
	if (mctp_medium_deinit(mctp_inst) == MCTP_ERROR)
		LOG_WRN("mctp deinit failed ");

	free(mctp_inst);
	return MCTP_SUCCESS;
}

/* configure mctp handle with specific medium type */
uint8_t mctp_set_medium_configure(mctp *mctp_inst, MCTP_MEDIUM_TYPE medium_type,
				  mctp_medium_conf medium_conf)
{
	if (!mctp_inst)
		return MCTP_ERROR;

	if (medium_type <= MCTP_MEDIUM_TYPE_UNKNOWN || medium_type >= MCTP_MEDIUM_TYPE_MAX)
		return MCTP_ERROR;

	mctp_inst->medium_type = medium_type;
	if (mctp_medium_init(mctp_inst, medium_conf) == MCTP_ERROR)
		goto error;
	return MCTP_SUCCESS;

error:
	if (mctp_medium_deinit(mctp_inst) == MCTP_ERROR)
		LOG_WRN("mctp deinit failed ");
	mctp_inst->medium_type = MCTP_MEDIUM_TYPE_UNKNOWN;
	return MCTP_ERROR;
}

uint8_t mctp_get_medium_configure(mctp *mctp_inst, MCTP_MEDIUM_TYPE *medium_type,
				  mctp_medium_conf *medium_conf)
{
	if (!mctp_inst || !medium_type || !medium_conf)
		return MCTP_ERROR;

	*medium_type = mctp_inst->medium_type;
	*medium_conf = mctp_inst->medium_conf;
	return MCTP_SUCCESS;
}

uint8_t mctp_stop(mctp *mctp_inst)
{
	if (!mctp_inst)
		return MCTP_ERROR;

	if (mctp_inst->mctp_rx_task_tid) {
		k_thread_abort(mctp_inst->mctp_rx_task_tid);
		mctp_inst->mctp_rx_task_tid = NULL;
	}

	if (mctp_inst->mctp_tx_task_tid) {
		k_thread_abort(mctp_inst->mctp_tx_task_tid);
		mctp_inst->mctp_tx_task_tid = NULL;
	}

	if (mctp_inst->mctp_tx_queue.buffer_start) {
		free(mctp_inst->mctp_tx_queue.buffer_start);
		mctp_inst->mctp_tx_queue.buffer_start = NULL;
	}

	mctp_inst->is_servcie_start = 0;
	return MCTP_SUCCESS;
}

uint8_t mctp_start(mctp *mctp_inst)
{
	if (!mctp_inst)
		return MCTP_ERROR;

	if (mctp_inst->is_servcie_start) {
		LOG_WRN("The mctp_inst is already start!");
		return MCTP_ERROR;
	}

	set_thread_name(mctp_inst);

	uint8_t *msgq_buf = (uint8_t *)malloc(MCTP_TX_QUEUE_SIZE * sizeof(mctp_tx_msg));
	if (!msgq_buf) {
		LOG_WRN("msgq alloc failed!!");
		goto error;
	}

	k_msgq_init(&mctp_inst->mctp_tx_queue, msgq_buf, sizeof(mctp_tx_msg), MCTP_TX_QUEUE_SIZE);

	/* create rx service */
	mctp_inst->mctp_rx_task_tid =
		k_thread_create(&mctp_inst->rx_task_thread_data, mctp_inst->rx_task_stack_area,
				K_KERNEL_STACK_SIZEOF(mctp_inst->rx_task_stack_area), mctp_rx_task,
				mctp_inst, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_MSEC(1));
	if (!mctp_inst->mctp_rx_task_tid)
		goto error;

	k_thread_name_set(mctp_inst->mctp_rx_task_tid, mctp_inst->mctp_rx_task_name);

	/* create tx service */
	mctp_inst->mctp_tx_task_tid =
		k_thread_create(&mctp_inst->tx_task_thread_data, mctp_inst->tx_task_stack_area,
				K_KERNEL_STACK_SIZEOF(mctp_inst->tx_task_stack_area), mctp_tx_task,
				mctp_inst, NULL, NULL, K_PRIO_PREEMPT(10), 0, K_MSEC(1));

	if (!mctp_inst->mctp_tx_task_tid)
		goto error;
	k_thread_name_set(mctp_inst->mctp_tx_task_tid, mctp_inst->mctp_tx_task_name);

	mctp_inst->is_servcie_start = 1;
	return MCTP_SUCCESS;

error:
	LOG_ERR("mctp_start failed!!");
	mctp_stop(mctp_inst);
	return MCTP_ERROR;
}

uint8_t mctp_bridge_msg(mctp *mctp_inst, uint8_t *buf, uint16_t len, mctp_ext_params ext_params)
{
	if (!mctp_inst || !buf || !len)
		return MCTP_ERROR;

	if (!mctp_inst->is_servcie_start) {
		LOG_WRN("The mctp_inst isn't start service!");
		return MCTP_ERROR;
	}

	mctp_tx_msg mctp_msg = { 0 };
	mctp_msg.is_bridge_packet = 1;
	mctp_msg.len = len;
	mctp_msg.buf = (uint8_t *)malloc(len);
	if (!mctp_msg.buf)
		goto error;
	memcpy(mctp_msg.buf, buf, len);

	mctp_msg.ext_params = ext_params;

	int ret = k_msgq_put(&mctp_inst->mctp_tx_queue, &mctp_msg, K_NO_WAIT);
	if (!ret)
		return MCTP_SUCCESS;

error:
	if (mctp_msg.buf)
		free(mctp_msg.buf);

	return MCTP_ERROR;
}

uint8_t mctp_send_msg(mctp *mctp_inst, uint8_t *buf, uint16_t len, mctp_ext_params ext_params)
{
	if (!mctp_inst || !buf || !len)
		return MCTP_ERROR;

	if (!mctp_inst->is_servcie_start) {
		LOG_WRN("The mctp_inst isn't start service!");
		return MCTP_ERROR;
	}

	mctp_tx_msg mctp_msg = { 0 };
	mctp_msg.len = len;
	mctp_msg.buf = (uint8_t *)malloc(len);
	if (!mctp_msg.buf) {
		LOG_WRN("can't alloc buf!!");
		goto error;
	}

	memcpy(mctp_msg.buf, buf, len);
	mctp_msg.ext_params = ext_params;

	int ret = k_msgq_put(&mctp_inst->mctp_tx_queue, &mctp_msg, K_NO_WAIT);
	if (!ret)
		return MCTP_SUCCESS;

error:
	if (mctp_msg.buf)
		free(mctp_msg.buf);

	return MCTP_ERROR;
}

uint8_t mctp_reg_endpoint_resolve_func(mctp *mctp_inst, endpoint_resolve resolve_fn)
{
	if (!mctp_inst || !resolve_fn)
		return MCTP_ERROR;

	mctp_inst->ep_resolve = resolve_fn;
	return MCTP_SUCCESS;
}

uint8_t mctp_reg_msg_rx_func(mctp *mctp_inst, mctp_fn_cb rx_cb)
{
	if (!mctp_inst || !rx_cb)
		return MCTP_ERROR;

	mctp_inst->rx_cb = rx_cb;
	return MCTP_SUCCESS;
}