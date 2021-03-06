/*
 * Copyright (c) 2020 Siddharth Chandrasekaran <siddharth@embedjournal.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_DECLARE(osdp, CONFIG_OSDP_LOG_LEVEL);

#include <stdlib.h>
#include <string.h>

#include "osdp_common.h"

#define CMD_POLL_DATA_LEN              0
#define CMD_LSTAT_DATA_LEN             0
#define CMD_ISTAT_DATA_LEN             0
#define CMD_OSTAT_DATA_LEN             0
#define CMD_RSTAT_DATA_LEN             0
#define CMD_ID_DATA_LEN                1
#define CMD_CAP_DATA_LEN               1
#define CMD_OUT_DATA_LEN               4
#define CMD_LED_DATA_LEN               14
#define CMD_BUZ_DATA_LEN               5
#define CMD_TEXT_DATA_LEN              6   /* variable length command */
#define CMD_COMSET_DATA_LEN            5
#define CMD_KEYSET_DATA_LEN            18
#define CMD_CHLNG_DATA_LEN             8
#define CMD_SCRYPT_DATA_LEN            16

#define REPLY_ACK_LEN                  1
#define REPLY_PDID_LEN                 13
#define REPLY_PDCAP_LEN                1   /* variable length command */
#define REPLY_PDCAP_ENTITY_LEN         3
#define REPLY_LSTATR_LEN               3
#define REPLY_RSTATR_LEN               2
#define REPLY_COM_LEN                  6
#define REPLY_NAK_LEN                  2
#define REPLY_CCRYPT_LEN               33
#define REPLY_RMAC_I_LEN               17

#define OSDP_PD_ERR_NONE               0
#define OSDP_PD_ERR_NO_DATA            1
#define OSDP_PD_ERR_GENERIC           -1
#define OSDP_PD_ERR_REPLY             -2

static struct osdp_pd_id osdp_pd_id = {
	.version = CONFIG_OSDP_PD_ID_VERSION,
	.model = CONFIG_OSDP_PD_ID_MODEL,
	.vendor_code = CONFIG_OSDP_PD_ID_VENDOR_CODE,
	.serial_number = CONFIG_OSDP_PD_ID_SERIAL_NUMBER,
	.firmware_version = CONFIG_OSDP_PD_ID_FIRMWARE_VERSION,
};

static struct osdp_pd_cap osdp_pd_cap[] = {
	/* Driver Implicit cababilities */
	{
		OSDP_PD_CAP_CHECK_CHARACTER_SUPPORT,
		1, /* The PD supports the 16-bit CRC-16 mode */
		0, /* N/A */
	},
	{
		OSDP_PD_CAP_COMMUNICATION_SECURITY,
#ifdef CONFIG_OSDP_SC_ENABLED
		1, /* (Bit-0) AES128 support */
		1, /* (Bit-0) default AES128 key */
#else
		0, /* SC not supported */
		0, /* SC not supported */
#endif
	},
	/* Configured from Kconfig */
	{
		OSDP_PD_CAP_CONTACT_STATUS_MONITORING,
		CONFIG_OSDP_PD_CAP_CONTACT_STATUS_MONITORING_COMP_LEVEL,
		CONFIG_OSDP_PD_CAP_CONTACT_STATUS_MONITORING_NUM_ITEMS,
	},
	{
		OSDP_PD_CAP_OUTPUT_CONTROL,
		CONFIG_OSDP_PD_CAP_OUTPUT_CONTROL_COMP_LEVEL,
		CONFIG_OSDP_PD_CAP_OUTPUT_CONTROL_NUM_ITEMS,
	},
	{
		OSDP_PD_CAP_READER_LED_CONTROL,
		CONFIG_OSDP_PD_CAP_READER_LED_CONTROL_COMP_LEVEL,
		CONFIG_OSDP_PD_CAP_READER_LED_CONTROL_NUM_ITEMS,
	},
	{
		OSDP_PD_CAP_READER_AUDIBLE_OUTPUT,
		CONFIG_OSDP_PD_CAP_READER_AUDIBLE_OUTPUT_COMP_LEVEL,
		CONFIG_OSDP_PD_CAP_READER_AUDIBLE_OUTPUT_NUM_ITEMS,
	},
	{
		OSDP_PD_CAP_READER_TEXT_OUTPUT,
		CONFIG_OSDP_PD_CAP_READER_TEXT_OUTPUT_COMP_LEVEL,
		CONFIG_OSDP_PD_CAP_READER_TEXT_OUTPUT_NUM_ITEMS,
	},
	{
		OSDP_PD_CAP_CARD_DATA_FORMAT,
		CONFIG_OSDP_PD_CAP_CARD_DATA_FORMAT_COMP_LEVEL,
		0, /* N/A set to 0 */
	},
	{
		OSDP_PD_CAP_TIME_KEEPING,
		CONFIG_OSDP_PD_CAP_TIME_KEEPING_COMP_LEVEL,
		0, /* N/A set to 0 */
	},
	{ -1, 0, 0 } /* Sentinel */
};

struct pd_event_node {
	queue_node_t node;
	struct osdp_event object;
};

static int pd_event_queue_init(struct osdp_pd *pd)
{
	if (slab_init(&pd->event.slab, sizeof(struct pd_event_node),
		      OSDP_CP_CMD_POOL_SIZE)) {
		LOG_ERR("Failed to initialize command slab");
		return -1;
	}
	queue_init(&pd->event.queue);
	return 0;
}

static void pd_event_queue_del(struct osdp_pd *pd)
{
	slab_del(&pd->event.slab);
}

static struct osdp_event *pd_event_alloc(struct osdp_pd *pd)
{
	struct pd_event_node *event = NULL;

	if (slab_alloc(&pd->event.slab, (void **)&event)) {
		LOG_ERR("Event slab allocation failed");
		return NULL;
	}
	return &event->object;
}

static void pd_event_free(struct osdp_pd *pd, struct osdp_event *event)
{
	struct pd_event_node *n;

	n = CONTAINER_OF(event, struct pd_event_node, object);
	slab_free(&pd->event.slab, n);
}

static void pd_event_enqueue(struct osdp_pd *pd, struct osdp_event *event)
{
	struct pd_event_node *n;

	n = CONTAINER_OF(event, struct pd_event_node, object);
	queue_enqueue(&pd->event.queue, &n->node);
}

static int pd_event_dequeue(struct osdp_pd *pd, struct osdp_event **event)
{
	struct pd_event_node *n;
	queue_node_t *node;

	if (queue_dequeue(&pd->event.queue, &node)) {
		return -1;
	}
	n = CONTAINER_OF(node, struct pd_event_node, node);
	*event = &n->object;
	return 0;
}

static int pd_translate_event(struct osdp_event *event, uint8_t *data)
{
	int reply_code = 0;

	switch (event->type) {
	case OSDP_EVENT_CARDREAD:
		if (event->cardread.format == OSDP_CARD_FMT_RAW_UNSPECIFIED ||
		    event->cardread.format == OSDP_CARD_FMT_RAW_WIEGAND) {
			reply_code = REPLY_RAW;
		} else if (event->cardread.format == OSDP_CARD_FMT_ASCII) {
			reply_code = REPLY_FMT;
		} else {
			LOG_ERR("Event: cardread; Error: unknown format");
			break;
		}
		break;
	case OSDP_EVENT_KEYPRESS:
		reply_code = REPLY_KEYPPAD;
		break;
	default:
		LOG_ERR("Unknown event type %d", event->type);
		break;
	}
	if (reply_code == 0) {
		/* POLL command cannot fail even when there are errors here */
		return REPLY_ACK;
	}
	memcpy(data, event, sizeof(struct osdp_event));
	return reply_code;
}

static int pd_cmd_cap_ok(struct osdp_pd *pd, struct osdp_cmd *cmd)
{
	struct osdp_pd_cap *cap = NULL;

	/* Validate the cmd_id against a PD capabilities where applicable */
	switch (pd->cmd_id) {
	case CMD_ISTAT:
		cap = &pd->cap[OSDP_PD_CAP_CONTACT_STATUS_MONITORING];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 0; /* Remove this when REPLY_ISTATR is supported */
	case CMD_OSTAT:
		cap = &pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 0; /* Remove this when REPLY_OSTATR is supported */
	case CMD_OUT:
		cap = &pd->cap[OSDP_PD_CAP_OUTPUT_CONTROL];
		if (cmd->output.output_no + 1 > cap->num_items) {
			LOG_DBG("CAP check: output_no(%d) > cap->num_items(%d)",
				cmd->output.output_no + 1, cap->num_items);
			break;
		}
		if (cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_LED:
		cap = &pd->cap[OSDP_PD_CAP_READER_LED_CONTROL];
		if (cmd->led.led_number + 1 > cap->num_items) {
			LOG_DBG("CAP check: LED(%d) > cap->num_items(%d)",
				cmd->led.led_number + 1, cap->num_items);
			break;
		}
		if (cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_BUZ:
		cap = &pd->cap[OSDP_PD_CAP_READER_AUDIBLE_OUTPUT];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_TEXT:
		cap = &pd->cap[OSDP_PD_CAP_READER_TEXT_OUTPUT];
		if (cap->num_items == 0 || cap->compliance_level == 0) {
			break;
		}
		return 1;
	case CMD_CHLNG:
	case CMD_SCRYPT:
	case CMD_KEYSET:
		cap = &pd->cap[OSDP_PD_CAP_COMMUNICATION_SECURITY];
		if (cap->compliance_level == 0) {
			pd->reply_id = REPLY_NAK;
			pd->ephemeral_data[0] = OSDP_PD_NAK_SC_UNSUP;
			return 0;
		}
		return 1;
	}

	pd->reply_id = REPLY_NAK;
	pd->ephemeral_data[0] = OSDP_PD_NAK_CMD_UNKNOWN;
	return 0;
}

static int pd_decode_command(struct osdp_pd *pd, uint8_t *buf, int len)
{
	int i, ret = OSDP_PD_ERR_GENERIC, pos = 0;
	struct osdp_cmd cmd;

	pd->reply_id = 0;
	pd->cmd_id = buf[pos++];
	len--;

	/* helper macro, can be called from switch cases below */
	#define PD_CMD_CAP_CHECK(pd, cmd)                                    \
		if (!pd_cmd_cap_ok(pd, cmd)) {                               \
			LOG_INF("PD is not capable of handling CMD(%02x); "  \
				"Reply with NAK_CMD_UNKNOWN", pd->cmd_id);   \
			ret = OSDP_PD_ERR_REPLY;                             \
			break;                                               \
		}

	#define ASSERT_LENGTH(got, exp)                                      \
		if (got != exp) {                                            \
			LOG_ERR("CMD(%02x) length error! Got:%d, Exp:%d",    \
				pd->cmd_id, got, exp);                       \
			return OSDP_PD_ERR_GENERIC;                          \
		}

	switch (pd->cmd_id) {
	case CMD_POLL:
		ASSERT_LENGTH(len, CMD_POLL_DATA_LEN);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_LSTAT:
		ASSERT_LENGTH(len, CMD_LSTAT_DATA_LEN);
		pd->reply_id = REPLY_LSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ISTAT:
		ASSERT_LENGTH(len, CMD_ISTAT_DATA_LEN);
		PD_CMD_CAP_CHECK(pd, NULL);
		pd->reply_id = REPLY_ISTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_OSTAT:
		ASSERT_LENGTH(len, CMD_OSTAT_DATA_LEN);
		PD_CMD_CAP_CHECK(pd, NULL);
		pd->reply_id = REPLY_OSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_RSTAT:
		ASSERT_LENGTH(len, CMD_RSTAT_DATA_LEN);
		pd->reply_id = REPLY_RSTATR;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_ID:
		ASSERT_LENGTH(len, CMD_ID_DATA_LEN);
		pos++;		/* Skip reply type info. */
		pd->reply_id = REPLY_PDID;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_CAP:
		ASSERT_LENGTH(len, CMD_CAP_DATA_LEN);
		pos++;		/* Skip reply type info. */
		pd->reply_id = REPLY_PDCAP;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_OUT:
		ASSERT_LENGTH(len, CMD_OUT_DATA_LEN);
		cmd.id = OSDP_CMD_OUTPUT;
		cmd.output.output_no    = buf[pos++];
		cmd.output.control_code = buf[pos++];
		cmd.output.timer_count  = buf[pos++];
		cmd.output.timer_count |= buf[pos++] << 8;
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->cmd_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_LED:
		ASSERT_LENGTH(len, CMD_LED_DATA_LEN);
		cmd.id = OSDP_CMD_LED;
		cmd.led.reader = buf[pos++];
		cmd.led.led_number = buf[pos++];

		cmd.led.temporary.control_code = buf[pos++];
		cmd.led.temporary.on_count     = buf[pos++];
		cmd.led.temporary.off_count    = buf[pos++];
		cmd.led.temporary.on_color     = buf[pos++];
		cmd.led.temporary.off_color    = buf[pos++];
		cmd.led.temporary.timer_count  = buf[pos++];
		cmd.led.temporary.timer_count |= buf[pos++] << 8;

		cmd.led.permanent.control_code = buf[pos++];
		cmd.led.permanent.on_count     = buf[pos++];
		cmd.led.permanent.off_count    = buf[pos++];
		cmd.led.permanent.on_color     = buf[pos++];
		cmd.led.permanent.off_color    = buf[pos++];
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->cmd_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_BUZ:
		ASSERT_LENGTH(len, CMD_BUZ_DATA_LEN);
		cmd.id = OSDP_CMD_BUZZER;
		cmd.buzzer.reader       = buf[pos++];
		cmd.buzzer.control_code = buf[pos++];
		cmd.buzzer.on_count     = buf[pos++];
		cmd.buzzer.off_count    = buf[pos++];
		cmd.buzzer.rep_count    = buf[pos++];
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->cmd_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_TEXT:
		if (len < CMD_TEXT_DATA_LEN) {
			break;
		}
		cmd.id = OSDP_CMD_TEXT;
		cmd.text.reader       = buf[pos++];
		cmd.text.control_code = buf[pos++];
		cmd.text.temp_time    = buf[pos++];
		cmd.text.offset_row   = buf[pos++];
		cmd.text.offset_col   = buf[pos++];
		cmd.text.length       = buf[pos++];
		if (cmd.text.length > OSDP_CMD_TEXT_MAX_LEN ||
		    ((len - CMD_TEXT_DATA_LEN) < cmd.text.length) ||
		    cmd.text.length > OSDP_CMD_TEXT_MAX_LEN) {
			break;
		}
		for (i = 0; i < cmd.text.length; i++) {
			cmd.text.data[i] = buf[pos++];
		}
		PD_CMD_CAP_CHECK(pd, &cmd);
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->cmd_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		for (i = 0; i < cmd->text.length; i++) {
			cmd->text.data[i] = buf[pos++];
		}
		osdp_cmd_enqueue(pd, cmd);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_COMSET:
		ASSERT_LENGTH(len, CMD_COMSET_DATA_LEN);
		cmd.id = OSDP_CMD_COMSET;
		cmd.comset.address    = buf[pos++];
		cmd.comset.baud_rate  = buf[pos++];
		cmd.comset.baud_rate |= buf[pos++] << 8;
		cmd.comset.baud_rate |= buf[pos++] << 16;
		cmd.comset.baud_rate |= buf[pos++] << 24;
		if (cmd.comset.address >= 0x7F ||
		    (cmd.comset.baud_rate != 9600 &&
		     cmd.comset.baud_rate != 38400 &&
		     cmd.comset.baud_rate != 115200)) {
			LOG_ERR("COMSET Failed! command discarded");
			cmd.comset.address = pd->address;
			cmd.comset.baud_rate = pd->baud_rate;
		}
		ret = pd->command_callback(pd->command_callback_arg, &cmd);
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->cmd_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		memcpy(pd->cmd_data, &cmd, sizeof(struct osdp_cmd));
		pd->reply_id = REPLY_COM;
		ret = OSDP_PD_ERR_NONE;
		break;
#ifdef CONFIG_OSDP_SC_ENABLED
	case CMD_KEYSET:
		PD_CMD_CAP_CHECK(pd, NULL);
		ASSERT_LENGTH(len, CMD_KEYSET_DATA_LEN);
		/**
		 * For CMD_KEYSET to be accepted, PD must be
		 * ONLINE and SC_ACTIVE.
		 */
		if (ISSET_FLAG(pd, PD_FLAG_SC_ACTIVE) == 0) {
			pd->reply_id = REPLY_NAK;
			pd->cmd_data[0] = OSDP_PD_NAK_SC_COND;
			LOG_ERR("Keyset with SC inactive");
			break;
		}
		/* only key_type == 1 (SCBK) and key_len == 16 is supported */
		if (buf[pos] != 1 || buf[pos + 1] != 16) {
			LOG_ERR("Keyset invalid len/type: %d/%d",
			      buf[pos], buf[pos + 1]);
			break;
		}
		cmd.id = OSDP_CMD_KEYSET;
		cmd.keyset.type   = buf[pos++];
		cmd.keyset.length = buf[pos++];
		memcpy(cmd.keyset.data, buf + pos, 16);
		memcpy(pd->sc.scbk, buf + pos, 16);
		ret = OSDP_PD_ERR_NONE;
		if (pd->command_callback) {
			ret = pd->command_callback(pd->command_callback_arg,
						   &cmd);
		} else {
			LOG_WRN("Keyset without command callback trigger");
		}
		if (ret != 0) {
			pd->reply_id = REPLY_NAK;
			pd->cmd_data[0] = OSDP_PD_NAK_RECORD;
			ret = OSDP_PD_ERR_REPLY;
			break;
		}
		CLEAR_FLAG(pd, PD_FLAG_SC_USE_SCBKD);
		CLEAR_FLAG(pd, PD_FLAG_INSTALL_MODE);
		pd->reply_id = REPLY_ACK;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_CHLNG:
		PD_CMD_CAP_CHECK(pd, NULL);
		ASSERT_LENGTH(len, CMD_CHLNG_DATA_LEN);
		osdp_sc_init(pd);
		CLEAR_FLAG(pd, PD_FLAG_SC_ACTIVE);
		for (i = 0; i < 8; i++) {
			pd->sc.cp_random[i] = buf[pos++];
		}
		pd->reply_id = REPLY_CCRYPT;
		ret = OSDP_PD_ERR_NONE;
		break;
	case CMD_SCRYPT:
		PD_CMD_CAP_CHECK(pd, NULL);
		ASSERT_LENGTH(len, CMD_SCRYPT_DATA_LEN);
		for (i = 0; i < 16; i++) {
			pd->sc.cp_cryptogram[i] = buf[pos++];
		}
		pd->reply_id = REPLY_RMAC_I;
		ret = OSDP_PD_ERR_NONE;
		break;
#endif /* CONFIG_OSDP_SC_ENABLED */
	default:
		LOG_ERR("Unknown command ID %02x", pd->cmd_id);
		pd->reply_id = REPLY_NAK;
		pd->cmd_data[0] = OSDP_PD_NAK_CMD_UNKNOWN;
		ret = OSDP_PD_ERR_NONE;
		break;
	}

	if (ret != 0 && ret != OSDP_PD_ERR_REPLY) {
		LOG_ERR("Invalid command structure. CMD: %02x, Len: %d ret: %d",
			pd->cmd_id, len, ret);
		pd->reply_id = REPLY_NAK;
		pd->cmd_data[0] = OSDP_PD_NAK_CMD_LEN;
		return OSDP_PD_ERR_REPLY;
	}

	if (pd->cmd_id != CMD_POLL) {
		LOG_DBG("CMD: %02x REPLY: %02x", pd->cmd_id, pd->reply_id);
	}

	return ret;
}

/**
 * Returns:
 * +ve: length of command
 * -ve: error
 */
static int pd_build_reply(struct osdp_pd *pd, uint8_t *buf, int max_len)
{
	int i, data_off, len = 0, ret = -1;
	struct osdp_cmd *cmd;

	data_off = osdp_phy_packet_get_data_offset(pd, buf);
#ifdef CONFIG_OSDP_SC_ENABLED
	uint8_t *smb = osdp_phy_packet_get_smb(pd, buf);
#endif
	buf += data_off;
	max_len -= data_off;

	#define ASSERT_BUF_LEN(need)                                           \
		if (max_len < need) {                                          \
			LOG_ERR("OOM at build REPLY(%02x) - have:%d, need:%d", \
				pd->reply_id, max_len, need);                  \
			return OSDP_PD_ERR_GENERIC;                            \
		}

	switch (pd->reply_id) {
	case REPLY_ACK:
		ASSERT_BUF_LEN(REPLY_ACK_LEN);
		buf[len++] = pd->reply_id;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDID:
		ASSERT_BUF_LEN(REPLY_PDID_LEN);
		buf[len++] = pd->reply_id;

		buf[len++] = BYTE_0(pd->id.vendor_code);
		buf[len++] = BYTE_1(pd->id.vendor_code);
		buf[len++] = BYTE_2(pd->id.vendor_code);

		buf[len++] = pd->id.model;
		buf[len++] = pd->id.version;

		buf[len++] = BYTE_0(pd->id.serial_number);
		buf[len++] = BYTE_1(pd->id.serial_number);
		buf[len++] = BYTE_2(pd->id.serial_number);
		buf[len++] = BYTE_3(pd->id.serial_number);

		buf[len++] = BYTE_3(pd->id.firmware_version);
		buf[len++] = BYTE_2(pd->id.firmware_version);
		buf[len++] = BYTE_1(pd->id.firmware_version);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_PDCAP:
		ASSERT_BUF_LEN(REPLY_PDCAP_LEN);
		buf[len++] = pd->reply_id;
		for (i = 0; i < OSDP_PD_CAP_SENTINEL; i++) {
			if (pd->cap[i].function_code != i) {
				continue;
			}
			if (max_len < REPLY_PDCAP_ENTITY_LEN) {
				LOG_ERR("Out of buffer space!");
				break;
			}
			buf[len++] = i;
			buf[len++] = pd->cap[i].compliance_level;
			buf[len++] = pd->cap[i].num_items;
			max_len -= REPLY_PDCAP_ENTITY_LEN;
		}
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_LSTATR:
		ASSERT_BUF_LEN(REPLY_LSTATR_LEN);
		buf[len++] = pd->reply_id;
		buf[len++] = ISSET_FLAG(pd, PD_FLAG_TAMPER);
		buf[len++] = ISSET_FLAG(pd, PD_FLAG_POWER);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RSTATR:
		ASSERT_BUF_LEN(REPLY_RSTATR_LEN);
		buf[len++] = pd->reply_id;
		buf[len++] = ISSET_FLAG(pd, PD_FLAG_R_TAMPER);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_COM:
		ASSERT_BUF_LEN(REPLY_COM_LEN);
		/**
		 * If COMSET succeeds, the PD must reply with the old params and
		 * then switch to the new params from then then on. We have the
		 * new params in the commands struct that we just enqueued so
		 * we can peek at tail of command queue and set that to
		 * pd->addr/pd->baud_rate.
		 *
		 * TODO: Persist pd->address and pd->baud_rate via
		 * subsys/settings
		 */
		cmd = osdp_cmd_get_last(pd);
		if (cmd == NULL || cmd->id != OSDP_CMD_COMSET) {
			LOG_ERR("Failed to fetch queue tail for COMSET");
			break;
		}

		buf[len++] = pd->reply_id;
		buf[len++] = cmd->comset.address;
		buf[len++] = BYTE_0(cmd->comset.baud_rate);
		buf[len++] = BYTE_1(cmd->comset.baud_rate);
		buf[len++] = BYTE_2(cmd->comset.baud_rate);
		buf[len++] = BYTE_3(cmd->comset.baud_rate);

		pd->address = (int)cmd->comset.address;
		pd->baud_rate = (int)cmd->comset.baud_rate;
		LOG_INF("COMSET Succeeded! New PD-Addr: %d; Baud: %d",
			pd->address, pd->baud_rate);
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_NAK:
		ASSERT_BUF_LEN(REPLY_NAK_LEN);
		buf[len++] = pd->reply_id;
		buf[len++] = pd->cmd_data[0];
		ret = OSDP_PD_ERR_NONE;
		break;
#ifdef CONFIG_OSDP_SC_ENABLED
	case REPLY_CCRYPT:
		if (smb == NULL) {
			break;
		}
		ASSERT_BUF_LEN(REPLY_CCRYPT_LEN);
		osdp_fill_random(pd->sc.pd_random, 8);
		osdp_compute_session_keys(TO_CTX(pd));
		osdp_compute_pd_cryptogram(pd);
		buf[len++] = pd->reply_id;
		for (i = 0; i < 8; i++) {
			buf[len++] = pd->sc.pd_client_uid[i];
		}
		for (i = 0; i < 8; i++) {
			buf[len++] = pd->sc.pd_random[i];
		}
		for (i = 0; i < 16; i++) {
			buf[len++] = pd->sc.pd_cryptogram[i];
		}
		smb[0] = 3;      /* length */
		smb[1] = SCS_12; /* type */
		smb[2] = ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD) ? 0 : 1;
		ret = OSDP_PD_ERR_NONE;
		break;
	case REPLY_RMAC_I:
		if (smb == NULL) {
			break;
		}
		ASSERT_BUF_LEN(REPLY_RMAC_I_LEN);
		osdp_compute_rmac_i(pd);
		buf[len++] = pd->reply_id;
		for (i = 0; i < 16; i++) {
			buf[len++] = pd->sc.r_mac[i];
		}
		smb[0] = 3;       /* length */
		smb[1] = SCS_14;  /* type */
		if (osdp_verify_cp_cryptogram(pd) == 0) {
			smb[2] = 1;  /* CP auth succeeded */
			SET_FLAG(pd, PD_FLAG_SC_ACTIVE);
			if (ISSET_FLAG(pd, PD_FLAG_SC_USE_SCBKD)) {
				LOG_WRN("SC Active with SCBK-D");
			} else {
				LOG_INF("SC Active");
			}
		} else {
			smb[2] = 0;  /* CP auth failed */
			LOG_WRN("failed to verify CP_crypt");
		}
		ret = OSDP_PD_ERR_NONE;
		break;
#endif /* CONFIG_OSDP_SC_ENABLED */
	}

#ifdef CONFIG_OSDP_SC_ENABLED
	if (smb && (smb[1] > SCS_14) && ISSET_FLAG(pd, PD_FLAG_SC_ACTIVE)) {
		smb[0] = 2; /* length */
		smb[1] = (len > 1) ? SCS_18 : SCS_16;
	}
#endif /* CONFIG_OSDP_SC_ENABLED */

	if (ret != 0) {
		/* catch all errors and report it as a RECORD error to CP */
		LOG_ERR("Failed to build REPLY(%02x); Sending NAK instead!",
			pd->reply_id);
		ASSERT_BUF_LEN(REPLY_NAK_LEN);
		buf[0] = REPLY_NAK;
		buf[1] = OSDP_PD_NAK_RECORD;
		len = 2;
	}

	return len;
}

/**
 * pd_send_reply - blocking send; doesn't handle partials
 * Returns:
 *   0 - success
 *  -1 - failure
 */
static int pd_send_reply(struct osdp_pd *pd)
{
	int ret, len;

	/* init packet buf with header */
	len = osdp_phy_packet_init(pd, pd->rx_buf, sizeof(pd->rx_buf));
	if (len < 0) {
		return OSDP_PD_ERR_GENERIC;
	}

	/* fill reply data */
	ret = pd_build_reply(pd, pd->rx_buf, sizeof(pd->rx_buf));
	if (ret <= 0) {
		return OSDP_PD_ERR_GENERIC;
	}
	len += ret;

	/* finalize packet */
	len = osdp_phy_packet_finalize(pd, pd->rx_buf, len, sizeof(pd->rx_buf));
	if (len < 0) {
		return OSDP_PD_ERR_GENERIC;
	}

	ret = pd->channel.send(pd->channel.data, pd->rx_buf, len);
	if (ret != len) {
		LOG_ERR("Channel send for %d bytes failed! ret: %d", len, ret);
		return OSDP_PD_ERR_GENERIC;
	}

	if (IS_ENABLED(CONFIG_OSDP_PACKET_TRACE)) {
		if (pd->cmd_id != CMD_POLL) {
			osdp_dump("PD sent", pd->rx_buf, len);
		}
	}

	return OSDP_PD_ERR_NONE;
}

static int pd_receve_packet(struct osdp_pd *pd)
{
	uint8_t *buf;
	int rec_bytes, ret, was_empty, max_len;

	was_empty = pd->rx_buf_len == 0;
	buf = pd->rx_buf + pd->rx_buf_len;
	max_len = sizeof(pd->rx_buf) - pd->rx_buf_len;

	rec_bytes = pd->channel.recv(pd->channel.data, buf, max_len);
	if (rec_bytes <= 0) {
		return 1;
	}
	if (was_empty && rec_bytes > 0) {
		/* Start of message */
		pd->tstamp = osdp_millis_now();
	}
	pd->rx_buf_len += rec_bytes;

	if (IS_ENABLED(CONFIG_OSDP_PACKET_TRACE)) {
		/**
		 * A crude way of identifying and not printing poll messages
		 * when CONFIG_OSDP_PACKET_TRACE is enabled. This is an early
		 * print to catch errors so keeping it simple.
		 */
		if (pd->rx_buf_len > 8 &&
		    pd->rx_buf[6] != CMD_POLL && pd->rx_buf[8] != CMD_POLL) {
			osdp_dump("PD received", pd->rx_buf, pd->rx_buf_len);
		}
	}

	pd->reply_id = 0;    /* reset past reply ID so phy can send NAK */
	pd->cmd_data[0] = 0; /* reset past NAK reason */
	ret = osdp_phy_decode_packet(pd, pd->rx_buf, pd->rx_buf_len);
	if (ret == OSDP_ERR_PKT_FMT) {
		if (pd->reply_id != 0) {
			return -2; /* Send a NAK */
		}
		return -1; /* fatal errors */
	} else if (ret == OSDP_ERR_PKT_WAIT) {
		/* rx_buf_len != pkt->len; wait for more data */
		return 1;
	} else if (ret == OSDP_ERR_PKT_SKIP) {
		/* soft fail - discard this message */
		pd->rx_buf_len = 0;
		if (pd->channel.flush) {
			pd->channel.flush(pd->channel.data);
		}
		return 1;
	}
	pd->rx_buf_len = ret;
	return 0;
}

void osdp_update(struct osdp *ctx)
{
	int ret;
	struct osdp_pd *pd = TO_PD(ctx, 0);

	switch (pd->state) {
	case OSDP_PD_STATE_IDLE:
		ret = pd_receve_packet(pd);
		if (ret == -1 || ((pd->rx_buf_len > 0 ||
		    ISSET_FLAG(pd, PD_FLAG_SC_ACTIVE)) &&
		    osdp_millis_since(pd->tstamp) > OSDP_RESP_TOUT_MS)) {
			/**
			 * When we receive a command from PD after a timeout,
			 * any established secure channel must be discarded.
			 */
			LOG_ERR("receive errors/timeout");
			pd->state = OSDP_PD_STATE_ERR;
			break;
		}
		if (ret == 1) {
			break;
		}
		if (ret == 0) {
			pd_decode_command(pd, pd->rx_buf, pd->rx_buf_len);
		}
		pd->state = OSDP_PD_STATE_SEND_REPLY;
		__fallthrough;
	case OSDP_PD_STATE_SEND_REPLY:
		if (pd_send_reply(pd) == -1) {
			pd->state = OSDP_PD_STATE_ERR;
			break;
		}
		pd->rx_buf_len = 0;
		pd->state = OSDP_PD_STATE_IDLE;
		break;
	case OSDP_PD_STATE_ERR:
		/**
		 * PD error state is momentary as it doesn't maintain any state
		 * between commands. We just clean up secure channel status and
		 * go back to idle state.
		 */
		CLEAR_FLAG(pd, PD_FLAG_SC_ACTIVE);
		pd->rx_buf_len = 0;
		if (pd->channel.flush) {
			pd->channel.flush(pd->channel.data);
		}
		pd->state = OSDP_PD_STATE_IDLE;
		break;
	}
}

static void osdp_pd_set_attributes(struct osdp_pd *pd, struct osdp_pd_cap *cap,
				   struct osdp_pd_id *id)
{
	int fc;

	while (cap && ((fc = cap->function_code) > 0)) {
		if (fc >= OSDP_PD_CAP_SENTINEL) {
			break;
		}
		pd->cap[fc].function_code = cap->function_code;
		pd->cap[fc].compliance_level = cap->compliance_level;
		pd->cap[fc].num_items = cap->num_items;
		cap++;
	}
	if (id != NULL) {
		memcpy(&pd->id, id, sizeof(struct osdp_pd_id));
	}
}

int osdp_setup(struct osdp *ctx, uint8_t *key)
{
	ARG_UNUSED(key);
	struct osdp_pd *pd;

	if (ctx->cp->num_pd != 1) {
		return -1;
	}
	pd = TO_PD(ctx, 0);
	osdp_pd_set_attributes(pd, osdp_pd_cap, &osdp_pd_id);
	SET_FLAG(pd, PD_FLAG_PD_MODE);
#ifdef CONFIG_OSDP_SC_ENABLED
	if (key == NULL) {
		LOG_WRN("SCBK not provided. PD is in INSTALL_MODE");
		SET_FLAG(pd, PD_FLAG_INSTALL_MODE);
	} else {
		memcpy(pd->sc.scbk, key, 16);
	}
	SET_FLAG(pd, PD_FLAG_SC_CAPABLE);
#endif
	return 0;
}

/* --- Exported Methods --- */

int osdp_pd_get_cmd(struct osdp_cmd *cmd)
{
	struct osdp_cmd *c;
	struct osdp_pd *pd = TO_PD(osdp_get_ctx(), 0);

	if (osdp_cmd_dequeue(pd, &c)) {
		return -1;
	}
	memcpy(cmd, c, sizeof(struct osdp_cmd));
	osdp_cmd_free(pd, c);
	return 0;
}
