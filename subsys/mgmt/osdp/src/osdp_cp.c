/*
 * Copyright (c) 2020 Siddharth Chandrasekaran <siddharth@embedjournal.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <logging/log.h>
LOG_MODULE_DECLARE(osdp, CONFIG_OSDP_LOG_LEVEL);

#include "osdp_common.h"

#define OSDP_PD_POLL_TIMEOUT_MS        (1000 / CONFIG_OSDP_PD_POLL_RATE)
#define OSDP_CMD_RETRY_WAIT_MS         (CONFIG_OSDP_CMD_RETRY_WAIT_SEC * 1000)

#ifdef CONFIG_OSDP_SC_ENABLED
#define OSDP_PD_SC_RETRY_MS            (CONFIG_OSDP_SC_RETRY_WAIT_SEC * 1000)
#endif

#define CMD_POLL_LEN                   1
#define CMD_LSTAT_LEN                  1
#define CMD_ISTAT_LEN                  1
#define CMD_OSTAT_LEN                  1
#define CMD_RSTAT_LEN                  1
#define CMD_ID_LEN                     2
#define CMD_CAP_LEN                    2
#define CMD_DIAG_LEN                   2
#define CMD_OUT_LEN                    5
#define CMD_LED_LEN                    15
#define CMD_BUZ_LEN                    6
#define CMD_TEXT_LEN                   7   /* variable length command */
#define CMD_COMSET_LEN                 6
#define CMD_KEYSET_LEN                 19
#define CMD_CHLNG_LEN                  9
#define CMD_SCRYPT_LEN                 17

#define REPLY_ACK_DATA_LEN             0
#define REPLY_PDID_DATA_LEN            12
#define REPLY_PDCAP_ENTITY_LEN         3
#define REPLY_LSTATR_DATA_LEN          2
#define REPLY_RSTATR_DATA_LEN          1
#define REPLY_COM_DATA_LEN             5
#define REPLY_NAK_DATA_LEN             1
#define REPLY_CCRYPT_DATA_LEN          32
#define REPLY_RMAC_I_DATA_LEN          16
#define REPLY_KEYPPAD_DATA_LEN         2   /* variable length command */
#define REPLY_RAW_DATA_LEN             4   /* variable length command */
#define REPLY_FMT_DATA_LEN             3   /* variable length command */
#define REPLY_BUSY_DATA_LEN            0

#define OSDP_CP_ERR_GENERIC           -1
#define OSDP_CP_ERR_NO_DATA            1
#define OSDP_CP_ERR_RETRY_CMD          2
#define OSDP_CP_ERR_CAN_YIELD          3
#define OSDP_CP_ERR_INPROG             4

static struct osdp_cmd *cp_cmd_alloc(struct osdp_pd *pd)
{
	struct osdp_cmd *cmd = NULL;

	if (k_mem_slab_alloc(&pd->cmd.slab, (void **)&cmd, K_MSEC(100))) {
		LOG_ERR("Memory allocation time-out");
		return NULL;
	}
	return cmd;
}

static void cp_cmd_free(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	k_mem_slab_free(&pd->cmd.slab, (void **)&cmd);
}

static void cp_cmd_enqueue(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	sys_slist_append(&pd->cmd.queue, &cmd->node);
}

static int cp_cmd_dequeue(struct osdp_pd *pd, struct osdp_cmd **cmd)
{
	sys_snode_t *node;

	node = sys_slist_peek_head(&pd->cmd.queue);
	if (node == NULL) {
		return -1;
	}
	sys_slist_remove(&pd->cmd.queue, NULL, node);
	*cmd = CONTAINER_OF(node, struct osdp_cmd, node);
	return 0;
}

static struct osdp_cmd *cp_cmd_get_last(struct osdp_pd *pd)
{
	return (struct osdp_cmd *)sys_slist_peek_tail(&pd->cmd.queue);
}

int osdp_extract_address(int *address)
{
	int pd_offset = 0;
	unsigned long addr;
	char *tok, *s1, *s2, addr_buf[32 * CONFIG_OSDP_NUM_CONNECTED_PD];

	strncpy(addr_buf, CONFIG_OSDP_PD_ADDRESS_LIST, sizeof(addr_buf) - 1);
	addr_buf[sizeof(addr_buf) - 1] = '\0';
	tok = strtok_r(addr_buf, ", ", &s1);
	while (tok && pd_offset < CONFIG_OSDP_NUM_CONNECTED_PD) {
		addr = strtoul(tok, &s2, 10);
		if (*s2 != '\0') { /* tok must be number-ish */
			return -1;
		}
		address[pd_offset] = addr;
		pd_offset++;
		tok = strtok_r(NULL, ", ", &s1);
	}
	return (pd_offset == CONFIG_OSDP_NUM_CONNECTED_PD) ? 0 : -1;
}

/**
 * Returns:
 * +ve: length of command
 * -ve: error
 */
static int cp_build_command(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	struct osdp_cmd *cmd = NULL;
	int data_off, i, ret = -1, len = 0;

	data_off = osdp_phy_packet_get_data_offset(pd, buf);

#ifdef CONFIG_OSDP_SC_ENABLED
	uint8_t *smb = osdp_phy_packet_get_smb(pd, buf);
#endif

	buf += data_off;
	max_len -= data_off;

	#define ASSERT_BUF_LEN(need)                                         \
		if (max_len < need) {                                        \
			LOG_ERR("OOM at build CMD(%02x) - have:%d, need:%d", \
				pd->cmd_id, max_len, need);                  \
			return OSDP_CP_ERR_GENERIC;                          \
		}

	switch (pd->cmd_id) {
	case CMD_POLL:
		ASSERT_BUF_LEN(CMD_POLL_LEN);
		buf[len++] = pd->cmd_id;
		ret = 0;
		break;
	case CMD_LSTAT:
		ASSERT_BUF_LEN(CMD_LSTAT_LEN);
		buf[len++] = pd->cmd_id;
		ret = 0;
		break;
	case CMD_ISTAT:
		ASSERT_BUF_LEN(CMD_ISTAT_LEN);
		buf[len++] = pd->cmd_id;
		ret = 0;
		break;
	case CMD_OSTAT:
		ASSERT_BUF_LEN(CMD_OSTAT_LEN);
		buf[len++] = pd->cmd_id;
		ret = 0;
		break;
	case CMD_RSTAT:
		ASSERT_BUF_LEN(CMD_RSTAT_LEN);
		buf[len++] = pd->cmd_id;
		ret = 0;
		break;
	case CMD_ID:
		ASSERT_BUF_LEN(CMD_ID_LEN);
		buf[len++] = pd->cmd_id;
		buf[len++] = 0x00;
		ret = 0;
		break;
	case CMD_CAP:
		ASSERT_BUF_LEN(CMD_CAP_LEN);
		buf[len++] = pd->cmd_id;
		buf[len++] = 0x00;
		ret = 0;
		break;
	case CMD_DIAG:
		ASSERT_BUF_LEN(CMD_DIAG_LEN);
		buf[len++] = pd->cmd_id;
		buf[len++] = 0x00;
		ret = 0;
		break;
	case CMD_OUT:
		ASSERT_BUF_LEN(CMD_OUT_LEN);
		cmd = (struct osdp_cmd *)pd->cmd_data;
		buf[len++] = pd->cmd_id;
		buf[len++] = cmd->output.output_no;
		buf[len++] = cmd->output.control_code;
		buf[len++] = BYTE_0(cmd->output.timer_count);
		buf[len++] = BYTE_1(cmd->output.timer_count);
		ret = 0;
		break;
	case CMD_LED:
		ASSERT_BUF_LEN(CMD_LED_LEN);
		cmd = (struct osdp_cmd *)pd->cmd_data;
		buf[len++] = pd->cmd_id;
		buf[len++] = cmd->led.reader;
		buf[len++] = cmd->led.led_number;

		buf[len++] = cmd->led.temporary.control_code;
		buf[len++] = cmd->led.temporary.on_count;
		buf[len++] = cmd->led.temporary.off_count;
		buf[len++] = cmd->led.temporary.on_color;
		buf[len++] = cmd->led.temporary.off_color;
		buf[len++] = BYTE_0(cmd->led.temporary.timer_count);
		buf[len++] = BYTE_1(cmd->led.temporary.timer_count);

		buf[len++] = cmd->led.permanent.control_code;
		buf[len++] = cmd->led.permanent.on_count;
		buf[len++] = cmd->led.permanent.off_count;
		buf[len++] = cmd->led.permanent.on_color;
		buf[len++] = cmd->led.permanent.off_color;
		ret = 0;
		break;
	case CMD_BUZ:
		ASSERT_BUF_LEN(CMD_BUZ_LEN);
		cmd = (struct osdp_cmd *)pd->cmd_data;
		buf[len++] = pd->cmd_id;
		buf[len++] = cmd->buzzer.reader;
		buf[len++] = cmd->buzzer.control_code;
		buf[len++] = cmd->buzzer.on_count;
		buf[len++] = cmd->buzzer.off_count;
		buf[len++] = cmd->buzzer.rep_count;
		ret = 0;
		break;
	case CMD_TEXT:
		cmd = (struct osdp_cmd *)pd->cmd_data;
		ASSERT_BUF_LEN(CMD_TEXT_LEN + cmd->text.length);
		buf[len++] = pd->cmd_id;
		buf[len++] = cmd->text.reader;
		buf[len++] = cmd->text.control_code;
		buf[len++] = cmd->text.temp_time;
		buf[len++] = cmd->text.offset_row;
		buf[len++] = cmd->text.offset_col;
		buf[len++] = cmd->text.length;
		for (i = 0; i < cmd->text.length; i++) {
			buf[len++] = cmd->text.data[i];
		}
		ret = 0;
		break;
	case CMD_COMSET:
		ASSERT_BUF_LEN(CMD_COMSET_LEN);
		cmd = (struct osdp_cmd *)pd->cmd_data;
		buf[len++] = pd->cmd_id;
		buf[len++] = cmd->comset.address;
		buf[len++] = BYTE_0(cmd->comset.baud_rate);
		buf[len++] = BYTE_1(cmd->comset.baud_rate);
		buf[len++] = BYTE_2(cmd->comset.baud_rate);
		buf[len++] = BYTE_3(cmd->comset.baud_rate);
		ret = 0;
		break;
#ifdef CONFIG_OSDP_SC_ENABLED
	case CMD_KEYSET:
		if (!ISSET_FLAG(pd, PD_FLAG_SC_ACTIVE)) {
			LOG_ERR("Cannot perform KEYSET without SC!");
			return -1;
		}
		ASSERT_BUF_LEN(CMD_KEYSET_LEN);
		buf[len++] = pd->cmd_id;
		buf[len++] = 1;  /* key type (1: SCBK) */
		buf[len++] = 16; /* key length in bytes */
		osdp_compute_scbk(pd, buf + len);
		len += 16;
		ret = 0;
		break;
	case CMD_CHLNG:
		ASSERT_BUF_LEN(CMD_CHLNG_LEN);
		if (smb == NULL) {
			break;
		}
		osdp_fill_random(pd->sc.cp_random, 8);
		smb[0] = 3;       /* length */
		smb[1] = SCS_11;  /* type */
		smb[2] = ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD) ? 0 : 1;
		buf[len++] = pd->cmd_id;
		for (i = 0; i < 8; i++)
			buf[len++] = pd->sc.cp_random[i];
		ret = 0;
		break;
	case CMD_SCRYPT:
		ASSERT_BUF_LEN(CMD_SCRYPT_LEN);
		if (smb == NULL) {
			break;
		}
		osdp_compute_cp_cryptogram(pd);
		smb[0] = 3;       /* length */
		smb[1] = SCS_13;  /* type */
		smb[2] = ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD) ? 0 : 1;
		buf[len++] = pd->cmd_id;
		for (i = 0; i < 16; i++)
			buf[len++] = pd->sc.cp_cryptogram[i];
		ret = 0;
		break;
#endif /* CONFIG_OSDP_SC_ENABLED */
	default:
		LOG_ERR("Unknown/Unsupported CMD(%02x)", pd->cmd_id);
		return OSDP_CP_ERR_GENERIC;
	}

#ifdef CONFIG_OSDP_SC_ENABLED
	if (smb && (smb[1] > SCS_14) && ISSET_FLAG(pd, PD_FLAG_SC_ACTIVE)) {
		/**
		 * When SC active and current cmd is not a handshake (<= SCS_14)
		 * then we must set SCS type to 17 if this message has data
		 * bytes and 15 otherwise.
		 */
		smb[0] = 2;
		smb[1] = (len > 1) ? SCS_17 : SCS_15;
	}
#endif /* CONFIG_OSDP_SC_ENABLED */
	if (ret < 0) {
		LOG_ERR("Unable to build CMD(%02x)", pd->cmd_id);
		return OSDP_CP_ERR_GENERIC;
	}

	return len;
}

static int cp_decode_response(struct osdp_pd *pd, uint8_t *buf, int len)
{
	uint32_t temp32;
	struct osdp_cp *cp = TO_CTX(pd)->cp;
	int i, ret = OSDP_CP_ERR_GENERIC, pos = 0, t1, t2;

	pd->reply_id = buf[pos++];
	len--;		/* consume reply id from the head */

	#define ASSERT_LENGTH(got, exp)                                      \
		if (got != exp) {                                            \
			LOG_ERR("REPLY(%02x) length error! Got:%d, Exp:%d",  \
				pd->reply_id, got, exp);                     \
			return OSDP_CP_ERR_GENERIC;                          \
		}

	switch (pd->reply_id) {
	case REPLY_ACK:
		ASSERT_LENGTH(len, REPLY_ACK_DATA_LEN);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_NAK:
		ASSERT_LENGTH(len, REPLY_NAK_DATA_LEN);
		LOG_WRN("PD replied with NAK(%d) for CMD(%02x)",
			buf[pos], pd->cmd_id);
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_PDID:
		ASSERT_LENGTH(len, REPLY_PDID_DATA_LEN);
		pd->id.vendor_code  = buf[pos++];
		pd->id.vendor_code |= buf[pos++] << 8;
		pd->id.vendor_code |= buf[pos++] << 16;

		pd->id.model = buf[pos++];
		pd->id.version = buf[pos++];

		pd->id.serial_number  = buf[pos++];
		pd->id.serial_number |= buf[pos++] << 8;
		pd->id.serial_number |= buf[pos++] << 16;
		pd->id.serial_number |= buf[pos++] << 24;

		pd->id.firmware_version  = buf[pos++] << 16;
		pd->id.firmware_version |= buf[pos++] << 8;
		pd->id.firmware_version |= buf[pos++];
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_PDCAP:
		if ((len % REPLY_PDCAP_ENTITY_LEN) != 0) {
			LOG_ERR("PDCAP response length is not a multiple of 3",
				buf[pos], pd->cmd_id);
			return OSDP_CP_ERR_GENERIC;
		}
		while (pos < len) {
			t1 = buf[pos++]; /* func_code */
			if (t1 > OSDP_PD_CAP_SENTINEL) {
				break;
			}
			pd->cap[t1].function_code    = t1;
			pd->cap[t1].compliance_level = buf[pos++];
			pd->cap[t1].num_items        = buf[pos++];
		}
		/* post-capabilities hooks */
		t2 = OSDP_PD_CAP_COMMUNICATION_SECURITY;
		if (pd->cap[t2].compliance_level & 0x01) {
			SET_FLAG(pd, PD_FLAG_SC_CAPABLE);
		} else {
			CLEAR_FLAG(pd, PD_FLAG_SC_CAPABLE);
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_LSTATR:
		ASSERT_LENGTH(len, REPLY_LSTATR_DATA_LEN);
		if (buf[pos++]) {
			SET_FLAG(pd, PD_FLAG_TAMPER);
		} else {
			CLEAR_FLAG(pd, PD_FLAG_TAMPER);
		}
		if (buf[pos++]) {
			SET_FLAG(pd, PD_FLAG_POWER);
		} else {
			CLEAR_FLAG(pd, PD_FLAG_POWER);
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_RSTATR:
		ASSERT_LENGTH(len, REPLY_RSTATR_DATA_LEN);
		if (buf[pos++]) {
			SET_FLAG(pd, PD_FLAG_R_TAMPER);
		} else {
			CLEAR_FLAG(pd, PD_FLAG_R_TAMPER);
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_COM:
		ASSERT_LENGTH(len, REPLY_COM_DATA_LEN);
		t1 = buf[pos++];
		temp32  = buf[pos++];
		temp32 |= buf[pos++] << 8;
		temp32 |= buf[pos++] << 16;
		temp32 |= buf[pos++] << 24;
		LOG_WRN("COMSET responded with ID:%d Baud:%d", t1, temp32);
		pd->address = t1;
		pd->baud_rate = temp32;
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_KEYPPAD:
		if (len < REPLY_KEYPPAD_DATA_LEN) {
			break;
		}
		pos++;	         /* reader number; skip */
		t1 = buf[pos++]; /* key length */
		if ((len - REPLY_KEYPPAD_DATA_LEN) != t1) {
			break;
		}
		if (cp->notifier.keypress) {
			for (i = 0; i < t1; i++) {
				t2 = buf[pos + i]; /* key data */
				cp->notifier.keypress(pd->offset, t2);
			}
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_RAW:
		if (len < REPLY_RAW_DATA_LEN) {
			break;
		}
		pos++;	                /* reader number; skip */
		t1 = buf[pos++];        /* format */
		t2 = buf[pos++];        /* length LSB */
		t2 |= buf[pos++] << 8; /* length MSB */
		if ((len - REPLY_RAW_DATA_LEN) != t2) {
			break;
		}
		if (cp->notifier.cardread) {
			cp->notifier.cardread(pd->offset, t1, buf + pos, t2);
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_FMT:
		if (len < REPLY_FMT_DATA_LEN) {
			break;
		}
		pos++;	/* reader number; skip */
		pos++;	/* skip one byte -- TODO: handle reader direction */
		t1 = buf[pos++]; /* Key length */
		if ((len - REPLY_FMT_DATA_LEN) != t1) {
			break;
		}
		if (cp->notifier.cardread) {
			cp->notifier.cardread(pd->offset, OSDP_CARD_FMT_ASCII,
					      buf + pos, t1);
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_BUSY:
		/* PD busy; signal upper layer to retry command */
		ASSERT_LENGTH(len, REPLY_BUSY_DATA_LEN);
		ret = OSDP_CP_ERR_RETRY_CMD;
		break;
#ifdef CONFIG_OSDP_SC_ENABLED
	case REPLY_CCRYPT:
		if (len != REPLY_CCRYPT_DATA_LEN) {
			break;
		}
		for (i = 0; i < 8; i++) {
			pd->sc.pd_client_uid[i] = buf[pos++];
		}
		for (i = 0; i < 8; i++) {
			pd->sc.pd_random[i] = buf[pos++];
		}
		for (i = 0; i < 16; i++) {
			pd->sc.pd_cryptogram[i] = buf[pos++];
		}
		osdp_compute_session_keys(TO_CTX(pd));
		if (osdp_verify_pd_cryptogram(pd) != 0) {
			LOG_ERR("Failed to verify PD cryptogram");
			return OSDP_CP_ERR_GENERIC;
		}
		ret = OSDP_CP_ERR_NONE;
		break;
	case REPLY_RMAC_I:
		ASSERT_LENGTH(len, REPLY_RMAC_I_DATA_LEN);
		for (i = 0; i < 16; i++) {
			pd->sc.r_mac[i] = buf[pos++];
		}
		SET_FLAG(pd, PD_FLAG_SC_ACTIVE);
		ret = OSDP_CP_ERR_NONE;
		break;
#endif /* CONFIG_OSDP_SC_ENABLED */
	default:
		LOG_DBG("Unexpected REPLY(%02x)", pd->reply_id);
		return OSDP_CP_ERR_GENERIC;
	}

	if (ret == OSDP_CP_ERR_GENERIC) {
		LOG_ERR("Format error in REPLY(%02x) for CMD(%02x)",
			pd->reply_id, pd->cmd_id);
		return OSDP_CP_ERR_GENERIC;
	}

	if (pd->cmd_id != CMD_POLL) {
		LOG_DBG("CMD(%02x) REPLY(%02x)", pd->cmd_id, pd->reply_id);
	}

	return ret;
}

static int cp_send_command(struct osdp_pd *pd)
{
	int ret, len;

	/* init packet buf with header */
	len = osdp_phy_packet_init(pd, pd->rx_buf, sizeof(pd->rx_buf));
	if (len < 0) {
		return OSDP_CP_ERR_GENERIC;
	}

	/* fill command data */
	ret = cp_build_command(pd, pd->rx_buf, sizeof(pd->rx_buf));
	if (ret < 0) {
		return OSDP_CP_ERR_GENERIC;
	}
	len += ret;

	/* finalize packet */
	len = osdp_phy_packet_finalize(pd, pd->rx_buf, len, sizeof(pd->rx_buf));
	if (len < 0) {
		return OSDP_CP_ERR_GENERIC;
	}

	/* flush rx to remove any invalid data. */
	if (pd->channel.flush) {
		pd->channel.flush(pd->channel.data);
	}

	ret = pd->channel.send(pd->channel.data, pd->rx_buf, len);
	if (ret != len) {
		LOG_ERR("Channel send for %d bytes failed! ret: %d", len, ret);
		return OSDP_CP_ERR_GENERIC;
	}

	if (IS_ENABLED(CONFIG_OSDP_PACKET_TRACE)) {
		if (pd->cmd_id != CMD_POLL) {
			LOG_DBG("bytes sent");
			osdp_dump(NULL, pd->rx_buf, len);
		}
	}

	return OSDP_CP_ERR_NONE;
}

static int cp_process_reply(struct osdp_pd *pd)
{
	uint8_t *buf;
	int rec_bytes, ret, max_len;

	buf = pd->rx_buf + pd->rx_buf_len;
	max_len = sizeof(pd->rx_buf) - pd->rx_buf_len;

	rec_bytes = pd->channel.recv(pd->channel.data, buf, max_len);
	if (rec_bytes <= 0) {	/* No data received */
		return OSDP_CP_ERR_NO_DATA;
	}
	pd->rx_buf_len += rec_bytes;

	if (IS_ENABLED(CONFIG_OSDP_PACKET_TRACE)) {
		if (pd->cmd_id != CMD_POLL) {
			LOG_DBG("bytes received");
			osdp_dump(NULL, pd->rx_buf, pd->rx_buf_len);
		}
	}

	/* Valid OSDP packet in buffer */
	ret = osdp_phy_decode_packet(pd, pd->rx_buf, pd->rx_buf_len);
	if (ret == OSDP_ERR_PKT_FMT) {
		return -1; /* fatal errors */
	} else if (ret == OSDP_ERR_PKT_WAIT) {
		/* rx_buf_len != pkt->len; wait for more data */
		return OSDP_CP_ERR_NO_DATA;
	} else if (ret == OSDP_ERR_PKT_SKIP) {
		/* soft fail - discard this message */
		pd->rx_buf_len = 0;
		if (pd->channel.flush) {
			pd->channel.flush(pd->channel.data);
		}
		return OSDP_CP_ERR_NO_DATA;
	}
	pd->rx_buf_len = ret;

	return cp_decode_response(pd, pd->rx_buf, pd->rx_buf_len);
}

static void cp_flush_command_queue(struct osdp_pd *pd)
{
	struct osdp_cmd *cmd;

	while (cp_cmd_dequeue(pd, &cmd) == 0) {
		cp_cmd_free(pd, cmd);
	}
}

static inline void cp_set_offline(struct osdp_pd *pd)
{
	CLEAR_FLAG(pd, PD_FLAG_SC_ACTIVE);
	pd->state = OSDP_CP_STATE_OFFLINE;
	pd->tstamp = osdp_millis_now();
}

static inline void cp_reset_state(struct osdp_pd *pd)
{
	pd->state = OSDP_CP_STATE_INIT;
	osdp_phy_state_reset(pd);
}

static inline void cp_set_state(struct osdp_pd *pd, enum osdp_cp_state_e state)
{
	pd->state = state;
	CLEAR_FLAG(pd, PD_FLAG_AWAIT_RESP);
}

static void cp_reset_channel(struct osdp_pd *pd)
{
	pd->rx_buf_len = 0;
	if (pd->channel.flush) {
		pd->channel.flush(pd->channel.data);
	}
}

/**
 * Note: This method must not dequeue cmd unless it reaches an invalid state.
 */
static int cp_phy_state_update(struct osdp_pd *pd)
{
	int ret = OSDP_CP_ERR_INPROG, tmp;
	struct osdp_cmd *cmd = NULL;

	switch (pd->phy_state) {
	case OSDP_CP_PHY_STATE_ERR_WAIT:
		ret = OSDP_CP_ERR_GENERIC;
		break;
	case OSDP_CP_PHY_STATE_IDLE:
		if (cp_cmd_dequeue(pd, &cmd)) {
			ret = 0;
			break;
		}
		pd->cmd_id = cmd->id;
		memcpy(pd->cmd_data, cmd, sizeof(struct osdp_cmd));
		cp_cmd_free(pd, cmd);
		cp_reset_channel(pd);
		/* fall-thru */
	case OSDP_CP_PHY_STATE_SEND_CMD:
		if ((cp_send_command(pd)) < 0) {
			LOG_ERR("Failed to send CMD(%d)", pd->cmd_id);
			pd->phy_state = OSDP_CP_PHY_STATE_ERR;
			ret = OSDP_CP_ERR_GENERIC;
			break;
		}
		pd->phy_state = OSDP_CP_PHY_STATE_REPLY_WAIT;
		pd->rx_buf_len = 0; /* reset buf_len for next use */
		pd->phy_tstamp = osdp_millis_now();
		break;
	case OSDP_CP_PHY_STATE_REPLY_WAIT:
		tmp = cp_process_reply(pd);
		if (tmp == 0) { /* success */
			pd->phy_state = OSDP_CP_PHY_STATE_CLEANUP;
			break;
		}
		if (tmp == OSDP_CP_ERR_RETRY_CMD) {
			LOG_INF("PD busy; retry last command");
			pd->phy_tstamp = osdp_millis_now();
			pd->phy_state = OSDP_CP_PHY_STATE_WAIT;
			ret = 2;
			break;
		}
		if (tmp == OSDP_CP_ERR_GENERIC) {
			pd->phy_state = OSDP_CP_PHY_STATE_ERR;
			break;
		}
		if (osdp_millis_since(pd->phy_tstamp) > OSDP_RESP_TOUT_MS) {
			LOG_ERR("CMD: %02x - response timeout", pd->cmd_id);
			pd->phy_state = OSDP_CP_PHY_STATE_ERR;
		}
		break;
	case OSDP_CP_PHY_STATE_WAIT:
		if (osdp_millis_since(pd->phy_tstamp) < cp_cmd_RETRY_WAIT_MS) {
			break;
		}
		pd->phy_state = OSDP_CP_PHY_STATE_IDLE;
		break;
	case OSDP_CP_PHY_STATE_ERR:
		cp_reset_channel(pd);
		cp_flush_command_queue(pd);
		pd->phy_state = OSDP_CP_PHY_STATE_ERR_WAIT;
		ret = OSDP_CP_ERR_GENERIC;
		break;
	case OSDP_CP_PHY_STATE_CLEANUP:
		pd->phy_state = OSDP_CP_PHY_STATE_IDLE;
		ret = OSDP_CP_ERR_CAN_YIELD; /* in between commands */
		break;
	}

	return ret;
}

/**
 * Returns:
 *   0: nothing done
 *   1: dispatched
 *  -1: error
 */
static int cp_cmd_dispatcher(struct osdp_pd *pd, int cmd)
{
	struct osdp_cmd *c;

	if (ISSET_FLAG(pd, PD_FLAG_AWAIT_RESP)) {
		CLEAR_FLAG(pd, PD_FLAG_AWAIT_RESP);
		return OSDP_CP_ERR_NONE; /* nothing to be done here */
	}

	c = cp_cmd_alloc(pd);
	if (c == NULL) {
		return OSDP_CP_ERR_GENERIC;
	}

	c->id = cmd;
	cp_cmd_enqueue(pd, c);
	SET_FLAG(pd, PD_FLAG_AWAIT_RESP);
	return OSDP_CP_ERR_INPROG;
}

static int state_update(struct osdp_pd *pd)
{
	int phy_state, soft_fail;

	phy_state = cp_phy_state_update(pd);
	if (phy_state == OSDP_CP_ERR_INPROG ||
	    phy_state == OSDP_CP_ERR_CAN_YIELD) {
		return OSDP_CP_ERR_GENERIC;
	}

	/* Certain states can fail without causing PD offline */
	soft_fail = (pd->state == OSDP_CP_STATE_SC_CHLNG);

	/* phy state error -- cleanup */
	if (pd->state != OSDP_CP_STATE_OFFLINE &&
	    phy_state == OSDP_CP_ERR_GENERIC && soft_fail == 0) {
		cp_set_offline(pd);
	}

	/* command queue is empty and last command was successful */

	switch (pd->state) {
	case OSDP_CP_STATE_ONLINE:
#ifdef CONFIG_OSDP_SC_ENABLED
		if (ISSET_FLAG(pd, PD_FLAG_SC_ACTIVE)  == false &&
		    ISSET_FLAG(pd, PD_FLAG_SC_CAPABLE) == true  &&
		    osdp_millis_since(pd->sc_tstamp) > OSDP_PD_SC_RETRY_MS) {
			LOG_INF("Retry SC after retry timeout");
			cp_set_state(pd, OSDP_CP_STATE_SC_INIT);
			break;
		}
#endif
		if (osdp_millis_since(pd->tstamp) < OSDP_PD_POLL_TIMEOUT_MS) {
			break;
		}
		if (cp_cmd_dispatcher(pd, CMD_POLL) == 0) {
			pd->tstamp = osdp_millis_now();
		}
		break;
	case OSDP_CP_STATE_OFFLINE:
		if (osdp_millis_since(pd->tstamp) > OSDP_CMD_RETRY_WAIT_MS) {
			cp_reset_state(pd);
		}
		break;
	case OSDP_CP_STATE_INIT:
		cp_set_state(pd, OSDP_CP_STATE_IDREQ);
		/* FALLTHRU */
	case OSDP_CP_STATE_IDREQ:
		if (cp_cmd_dispatcher(pd, CMD_ID) != 0) {
			break;
		}
		if (pd->reply_id != REPLY_PDID) {
			LOG_ERR("Unexpected REPLY(%02x) for cmd "
				STR(CMD_CAP), pd->reply_id);
			cp_set_offline(pd);
		}
		cp_set_state(pd, OSDP_CP_STATE_CAPDET);
		/* FALLTHRU */
	case OSDP_CP_STATE_CAPDET:
		if (cp_cmd_dispatcher(pd, CMD_CAP) != 0) {
			break;
		}
		if (pd->reply_id != REPLY_PDCAP) {
			LOG_ERR("Unexpected REPLY(%02x) for cmd "
				STR(CMD_CAP), pd->reply_id);
			cp_set_offline(pd);
		}
#ifdef CONFIG_OSDP_SC_ENABLED
		if (ISSET_FLAG(pd, PD_FLAG_SC_CAPABLE)) {
			CLEAR_FLAG(pd, PD_FLAG_SC_SCBKD_DONE);
			CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
			cp_set_state(pd, OSDP_CP_STATE_SC_INIT);
			break;
		}
#endif /* CONFIG_OSDP_SC_ENABLED */
		cp_set_state(pd, OSDP_CP_STATE_ONLINE);
		break;
#ifdef CONFIG_OSDP_SC_ENABLED
	case OSDP_CP_STATE_SC_INIT:
		osdp_sc_init(pd);
		cp_set_state(pd, OSDP_CP_STATE_SC_CHLNG);
		/* FALLTHRU */
	case OSDP_CP_STATE_SC_CHLNG:
		if (cp_cmd_dispatcher(pd, CMD_CHLNG) != 0) {
			break;
		}
		if (phy_state < 0) {
			if (ISSET_FLAG(pd, PD_FLAG_SC_SCBKD_DONE)) {
				LOG_INF("SC Failed. Online without SC");
				pd->sc_tstamp = osdp_millis_now();
				cp_set_state(pd, OSDP_CP_STATE_ONLINE);
				break;
			}
			SET_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
			SET_FLAG(pd, PD_FLAG_SC_SCBKD_DONE);
			cp_set_state(pd, OSDP_CP_STATE_SC_INIT);
			pd->phy_state = 0; /* soft reset phy state */
			LOG_WRN("SC Failed. Retry with SCBK-D");
			break;
		}
		if (pd->reply_id != REPLY_CCRYPT) {
			LOG_ERR("CHLNG failed. Online without SC");
			pd->sc_tstamp = osdp_millis_now();
			cp_set_state(pd, OSDP_CP_STATE_ONLINE);
			break;
		}
		cp_set_state(pd, OSDP_CP_STATE_SC_SCRYPT);
		/* FALLTHRU */
	case OSDP_CP_STATE_SC_SCRYPT:
		if (cp_cmd_dispatcher(pd, CMD_SCRYPT) != 0) {
			break;
		}
		if (pd->reply_id != REPLY_RMAC_I) {
			LOG_ERR("SCRYPT failed. Online without SC");
			pd->sc_tstamp = osdp_millis_now();
			cp_set_state(pd, OSDP_CP_STATE_ONLINE);
			break;
		}
		if (ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD)) {
			LOG_WRN("SC ACtive with SCBK-D. Set SCBK");
			cp_set_state(pd, OSDP_CP_STATE_SET_SCBK);
			break;
		}
		LOG_INF("SC Active");
		pd->sc_tstamp = osdp_millis_now();
		cp_set_state(pd, OSDP_CP_STATE_ONLINE);
		break;
	case OSDP_CP_STATE_SET_SCBK:
		if (cp_cmd_dispatcher(pd, CMD_KEYSET) != 0) {
			break;
		}
		if (pd->reply_id == REPLY_NAK) {
			LOG_WRN("Failed to set SCBK; continue with SCBK-D");
			cp_set_state(pd, OSDP_CP_STATE_ONLINE);
			break;
		}
		LOG_INF("SCBK set; restarting SC to verify new SCBK");
		CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
		CLEAR_FLAG(pd, PD_FLAG_SC_ACTIVE);
		cp_set_state(pd, OSDP_CP_STATE_SC_INIT);
		pd->seq_number = -1;
		break;
#endif /* CONFIG_OSDP_SC_ENABLED */
	default:
		break;
	}

	return 0;
}

#ifdef CONFIG_OSDP_SC_ENABLED
static int osdp_cp_send_command_keyset(struct osdp_cmd_keyset *cmd)
{
	int i;
	struct osdp_cmd *p;
	struct osdp_pd *pd;
	struct osdp *ctx = osdp_get_ctx();

	if (osdp_get_sc_status_mask() != PD_MASK(ctx)) {
		LOG_WRN("CMD_KEYSET can be sent only when all PDs are "
			"ONLINE and SC_ACTIVE.");
		return 1;
	}

	for (i = 0; i < NUM_PD(ctx); i++) {
		pd = TO_PD(ctx, i);
		p = cp_cmd_alloc(pd);
		if (p == NULL) {
			return -1;
		}
		p->id = CMD_KEYSET;
		memcpy(&p->keyset, &cmd, sizeof(struct osdp_cmd_keyset));
		cp_cmd_enqueue(pd, p);
	}

	return 0;
}
#endif /* CONFIG_OSDP_SC_ENABLED */

void osdp_update(struct osdp *ctx)
{
	int i;

	for (i = 0; i < NUM_PD(ctx); i++) {
		SET_CURRENT_PD(ctx, i);
		state_update(GET_CURRENT_PD(ctx));
	}
}

int osdp_setup(struct osdp *ctx, uint8_t *key)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(key);

#ifdef CONFIG_OSDP_SC_ENABLED
	if (key == NULL) {
		LOG_ERR("Master key cannot be null");
		return -1;
	}
	memcpy(ctx->sc_master_key, key, 16);
#endif
	return 0;
}

/* --- Exported Methods --- */

int osdp_cp_set_callback_key_press(int (*cb)(int address, uint8_t key))
{
	struct osdp *ctx = osdp_get_ctx();

	ctx->cp->notifier.keypress = cb;

	return 0;
}

int osdp_cp_set_callback_card_read(
	int (*cb)(int address, int format, uint8_t *data, int len))
{
	struct osdp *ctx = osdp_get_ctx();

	TO_CP(ctx)->notifier.cardread = cb;

	return 0;
}

int osdp_cp_send_command(int pd, struct osdp_cmd *cmd)
{
	struct osdp *ctx = osdp_get_ctx();
	struct osdp_cmd *p;
	int cmd_id;

	if (pd < 0 || pd >= NUM_PD(ctx)) {
		LOG_ERR("Invalid PD number");
		return -1;
	}
	if (TO_PD(ctx, pd)->state != OSDP_CP_STATE_ONLINE) {
		LOG_WRN("PD not online");
		return -1;
	}

	switch (cmd->id) {
	case OSDP_CMD_OUTPUT:
		cmd_id = CMD_OUT;
		break;
	case OSDP_CMD_LED:
		cmd_id = CMD_LED;
		break;
	case OSDP_CMD_BUZZER:
		cmd_id = CMD_BUZ;
		break;
	case OSDP_CMD_TEXT:
		cmd_id = CMD_TEXT;
		break;
	case OSDP_CMD_COMSET:
		cmd_id = CMD_COMSET;
		break;
#ifdef CONFIG_OSDP_SC_ENABLED
	case OSDP_CMD_KEYSET:
		return osdp_cp_send_command_keyset(&cmd->keyset);
#endif
	default:
		LOG_ERR("Invalid CMD_ID:%d", p->id);
		return -1;
	}

	p = cp_cmd_alloc(TO_PD(ctx, pd));
	if (p == NULL) {
		return -1;
	}
	memcpy(p, cmd, sizeof(struct osdp_cmd));
	p->id = cmd_id; /* translate to internal */
	cp_cmd_enqueue(TO_PD(ctx, pd), p);
	return 0;
}
