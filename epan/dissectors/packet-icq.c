/* packet-icq.c
 * Routines for ICQ packet disassembly
 *
 * $Id$
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * This file: by Kojak <kojak@bigwig.net>
 *
 * Decoding code ripped, reference to the original author at the
 * appropriate place with the code itself.
 */

#include "config.h"

#include <glib.h>

#include <epan/packet.h>
#include <epan/expert.h>

static int proto_icq = -1;
static int hf_icq_version = -1;
static int hf_icq_uin = -1;
static int hf_icq_client_cmd = -1;
static int hf_icq_server_cmd = -1;
static int hf_icq_sessionid = -1;
static int hf_icq_checkcode = -1;
static int hf_icq_type = -1;
static int hf_icq_msg_type = -1;
static int hf_icq_seqnum1 = -1;
static int hf_icq_seqnum2 = -1;
static int hf_icq_checkcode_key = -1;
static int hf_icq_group = -1;
static int hf_icq_ack_random = -1;
static int hf_icq_keep_alive_random = -1;
static int hf_icq_status = -1;
static int hf_icq_meta_user_subcmd = -1;

static gint ett_icq = -1;
static gint ett_icq_header = -1;
static gint ett_icq_body = -1;
static gint ett_icq_body_parts = -1;

static expert_field ei_icq_unknown_meta_subcmd = EI_INIT;
static expert_field ei_icq_unknown_command = EI_INIT;

/* This is not IANA registered */
#define UDP_PORT_ICQ	4000

#define ICQ5_SERVER 0
#define ICQ5_CLIENT 1

const true_false_string tfs_client_server = { "Client", "Server" };

static void
dissect_icqv5Server(tvbuff_t *tvb, int offset, packet_info *pinfo,
					proto_tree *tree, int pktsize);

/* Offsets of fields in the ICQ headers */
/* Can be 0x0002 or 0x0005 */
#define ICQ_VERSION		0x00
/* Is either one (server) or four (client) bytes long */
/* Client header offsets */
#define ICQ5_UNKNOWN		0x02
#define ICQ5_CL_UIN			0x06
#define ICQ5_CL_SESSIONID	0x0a
#define ICQ5_CL_CMD			0x0e
#define ICQ5_CL_SEQNUM1		0x10
#define ICQ5_CL_SEQNUM2		0x12
#define ICQ5_CL_CHECKCODE	0x14
#define ICQ5_CL_PARAM		0x18
#define ICQ5_CL_HDRSIZE		0x18

/* Server header offsets */
#define ICQ5_SRV_SESSIONID	0x03
#define ICQ5_SRV_CMD		0x07
#define ICQ5_SRV_SEQNUM1	0x09
#define ICQ5_SRV_SEQNUM2	0x0b
#define ICQ5_SRV_UIN		0x0d
#define ICQ5_SRV_CHECKCODE	0x11
#define ICQ5_SRV_PARAM		0x15
#define ICQ5_SRV_HDRSIZE	0x15

#define SRV_ACK				0x000a

#define SRV_SILENT_TOO_LONG	0x001e

#define SRV_GO_AWAY			0x0028

#define SRV_NEW_UIN			0x0046

/* LOGIN_REPLY is very scary. It has a lot of fields that are undocumented
 * Only the IP field makes sense */
#define SRV_LOGIN_REPLY		0x005a
#define SRV_LOGIN_REPLY_IP	0x000c

#define SRV_BAD_PASS		0x0064

#define SRV_USER_ONLINE		0x006e
#define SRV_USER_ONL_UIN	0x0000
#define SRV_USER_ONL_IP		0x0004
#define SRV_USER_ONL_PORT	0x0008
#define SRV_USER_ONL_REALIP	0x000c
#define SRV_USER_ONL_X1		0x0010
#define SRV_USER_ONL_STATUS	0x0013
#define SRV_USER_ONL_X2		0x0015

#define SRV_USER_OFFLINE	0x0078
#define SRV_USER_OFFLINE_UIN 0x0000

#define SRV_MULTI			0x0212
#define SRV_MULTI_NUM		0x0000

#define SRV_META_USER		0x03de
#define SRV_META_USER_SUBCMD 0x0000
#define SRV_META_USER_RESULT 0x0002
#define SRV_META_USER_DATA	0x0003

#define SRV_UPDATE_SUCCESS	0x01e0

#define SRV_UPDATE_FAIL		0x01ea

/*
 * ICQv5 SRV_META_USER subcommands
 */
#define META_EX_USER_FOUND	0x0190
#define META_USER_FOUND		0x019a
#define META_ABOUT			0x00e6
#define META_USER_INFO		0x00c8

#define SRV_RECV_MESSAGE	0x00dc
#define SRV_RECV_MSG_UIN	0x0000
#define SRV_RECV_MSG_YEAR	0x0004
#define SRV_RECV_MSG_MONTH	0x0006
#define SRV_RECV_MSG_DAY	0x0007
#define SRV_RECV_MSG_HOUR	0x0008
#define SRV_RECV_MSG_MINUTE	0x0009
#define SRV_RECV_MSG_MSG_TYPE 0x000a

#define SRV_RAND_USER		0x024e
#define SRV_RAND_USER_UIN	0x0000
#define SRV_RAND_USER_IP	0x0004
#define SRV_RAND_USER_PORT	0x0008
#define SRV_RAND_USER_REAL_IP 0x000c
#define SRV_RAND_USER_CLASS	0x0010
#define SRV_RAND_USER_X1	0x0011
#define SRV_RAND_USER_STATUS 0x0015
#define SRV_RAND_USER_TCP_VER 0x0019

/* This message has the same structure as cmd_send_msg */
#define SRV_SYS_DELIVERED_MESS	0x0104

static const value_string serverMetaSubCmdCode[] = {
	{ META_USER_FOUND, "META_USER_FOUND" },
	{ META_EX_USER_FOUND, "META_EX_USER_FOUND"  },
	{ META_ABOUT, "META_ABOUT" },
	{ META_USER_INFO, "META_USER_INFO" },
	{ 0, NULL }
};

static const value_string serverCmdCode[] = {
	{ SRV_ACK, "SRV_ACK" },
	{ SRV_SILENT_TOO_LONG, "SRV_SILENT_TOO_LONG" },
	{ SRV_GO_AWAY, "SRV_GO_AWAY" },
	{ SRV_NEW_UIN, "SRV_NEW_UIN" },
	{ SRV_LOGIN_REPLY, "SRV_LOGIN_REPLY" },
	{ SRV_BAD_PASS, "SRV_BAD_PASS" },
	{ SRV_USER_ONLINE, "SRV_USER_ONLINE" },
	{ SRV_USER_OFFLINE, "SRV_USER_OFFLINE" },
	{ 130, "SRV_QUERY" },
	{ 140, "SRV_USER_FOUND" },
	{ 160, "SRV_END_OF_SEARCH" },
	{ 180, "SRV_NEW_USER" },
	{ 200, "SRV_UPDATE_EXT" },
	{ SRV_RECV_MESSAGE, "SRV_RECV_MESSAGE" },
	{ 230, "SRV_END_OFFLINE_MESSAGES" },
	{ 240, "SRV_NOT_CONNECTED" },
	{ 250, "SRV_TRY_AGAIN" },
	{ SRV_SYS_DELIVERED_MESS, "SRV_SYS_DELIVERED_MESS" },
	{ 280, "SRV_INFO_REPLY" },
	{ 290, "SRV_EXT_INFO_REPLY" },
	{ 420, "SRV_STATUS_UPDATE" },
	{ 450, "SRV_SYSTEM_MESSAGE" },
	{ SRV_UPDATE_SUCCESS, "SRV_UPDATE_SUCCESS" },
	{ SRV_UPDATE_FAIL, "SRV_UPDATE_FAIL" },
	{ 500, "SRV_AUTH_UPDATE" },
	{ SRV_MULTI, "SRV_MULTI_PACKET" },
	{ 540, "SRV_END_CONTACTLIST_STATUS" },
	{ SRV_RAND_USER, "SRV_RAND_USER" },
	{ SRV_META_USER, "SRV_META_USER" },
	{ 0, NULL }
};

#define MSG_TEXT			0x0001
#define MSG_URL				0x0004
#define MSG_AUTH_REQ		0x0006
#define MSG_AUTH			0x0008
#define MSG_USER_ADDED		0x000c
#define MSG_EMAIL			0x000e
#define MSG_CONTACTS		0x0013

#define STATUS_ONLINE		0x00000000
#define STATUS_AWAY			0x00000001
#define STATUS_DND			0x00000013
#define STATUS_INVISIBLE	0x00000100
#define STATUS_OCCUPIED		0x00000010
#define STATUS_NA			0x00000004
#define STATUS_CHAT			0x00000020

/* Offsets for all packets measured from the start of the payload; i.e.
 * with the ICQ header removed
 */
#define CMD_ACK				0x000a
#define CMD_ACK_RANDOM		0x0000

#define CMD_SEND_MSG			0x010E
#define CMD_SEND_MSG_RECV_UIN	0x0000
#define CMD_SEND_MSG_MSG_TYPE	0x0004
#define CMD_SEND_MSG_MSG_LEN	0x0006
#define CMD_SEND_MSG_MSG_TEXT	0x0008
/* The rest of the packet should be a null-term string */

#define CMD_LOGIN			0x03E8
#define CMD_LOGIN_TIME		0x0000
#define CMD_LOGIN_PORT		0x0004
#define CMD_LOGIN_PASSLEN	0x0008
#define CMD_LOGIN_PASSWD	0x000A
/* The password is variable length; so when we've decoded the passwd,
 * the structure starts counting at 0 again.
 */
#define CMD_LOGIN_IP		0x0004
#define CMD_LOGIN_STATUS	0x0009

#define CMD_CONTACT_LIST	0x0406
#define CMD_CONTACT_LIST_NUM 0x0000

#define CMD_USER_META		0x064a

#define CMD_REG_NEW_USER	0x03fc

#define CMD_ACK_MESSAGES		0x0442
#define CMD_ACK_MESSAGES_RANDOM	0x0000

#define CMD_KEEP_ALIVE			0x042e
#define CMD_KEEP_ALIVE_RANDOM	0x0000

#define CMD_SEND_TEXT_CODE		0x0438
#define CMD_SEND_TEXT_CODE_LEN	0x0000
#define CMD_SEND_TEXT_CODE_TEXT	0x0002

#define CMD_MSG_TO_NEW_USER     0x0456

#define CMD_QUERY_SERVERS		0x04ba

#define CMD_QUERY_ADDONS		0x04c4

#define CMD_STATUS_CHANGE		0x04d8
#define CMD_STATUS_CHANGE_STATUS 0x0000

#define CMD_ADD_TO_LIST		0x053c
#define CMD_ADD_TO_LIST_UIN	0x0000

#define CMD_RAND_SEARCH			0x056e
#define CMD_RAND_SEARCH_GROUP	0x0000

#define CMD_META_USER		0x064a

static const value_string msgTypeCode[] = {
	{ MSG_TEXT, "MSG_TEXT" },
	{ MSG_URL, "MSG_URL" },
	{ MSG_AUTH_REQ, "MSG_AUTH_REQ" },
	{ MSG_AUTH, "MSG_AUTH" },
	{ MSG_USER_ADDED, "MSG_USER_ADDED" },
	{ MSG_EMAIL, "MSG_EMAIL" },
	{ MSG_CONTACTS, "MSG_CONTACTS" },
	{ 0, NULL }
};

static const value_string statusCode[] = {
	{ STATUS_ONLINE, "ONLINE" },
	{ STATUS_AWAY, "AWAY" },
	{ STATUS_DND, "DND" },
	{ STATUS_INVISIBLE, "INVISIBLE" },
	{ STATUS_OCCUPIED, "OCCUPIED" },
	{ STATUS_NA, "NA" },
	{ STATUS_CHAT, "Free for Chat" },
	{ 0, NULL }
};

static const value_string clientCmdCode[] = {
	{ CMD_ACK, "CMD_ACK" },
	{ CMD_SEND_MSG, "CMD_SEND_MESSAGE" },
	{ CMD_LOGIN, "CMD_LOGIN" },
	{ CMD_REG_NEW_USER, "CMD_REG_NEW_USER" },
	{ 1030, "CMD_CONTACT_LIST" },
	{ 1050, "CMD_SEARCH_UIN" },
	{ 1060, "CMD_SEARCH_USER" },
	{ 1070, "CMD_KEEP_ALIVE" },
	{ CMD_SEND_TEXT_CODE, "CMD_SEND_TEXT_CODE" },
	{ CMD_ACK_MESSAGES, "CMD_ACK_MESSAGES" },
	{ 1100, "CMD_LOGIN_1" },
	{ CMD_MSG_TO_NEW_USER, "CMD_MSG_TO_NEW_USER" },
	{ 1120, "CMD_INFO_REQ" },
	{ 1130, "CMD_EXT_INFO_REQ" },
	{ 1180, "CMD_CHANGE_PW" },
	{ 1190, "CMD_NEW_USER_INFO" },
	{ 1200, "CMD_UPDATE_EXT_INFO" },
	{ CMD_QUERY_SERVERS, "CMD_QUERY_SERVERS" },
	{ CMD_QUERY_ADDONS, "CMD_QUERY_ADDONS" },
	{ CMD_STATUS_CHANGE, "CMD_STATUS_CHANGE" },
	{ 1260, "CMD_NEW_USER_1" },
	{ 1290, "CMD_UPDATE_INFO" },
	{ 1300, "CMD_AUTH_UPDATE" },
	{ 1310, "CMD_KEEP_ALIVE2" },
	{ 1320, "CMD_LOGIN_2" },
	{ CMD_ADD_TO_LIST, "CMD_ADD_TO_LIST" },
	{ 1380, "CMD_RAND_SET" },
	{ CMD_RAND_SEARCH, "CMD_RAND_SEARCH" },
	{ CMD_META_USER, "CMD_META_USER" },
	{ 1700, "CMD_INVIS_LIST" },
	{ 1710, "CMD_VIS_LIST" },
	{ 1720, "CMD_UPDATE_LIST" },
	{ 0, NULL }
};

static const value_string group_vals[] = {
	{ 1, "Name" },
	{ 2, "General" },
	{ 3, "Romance" },
	{ 4, "Games" },
	{ 5, "Students" },
	{ 6, "20 Something" },
	{ 7, "30 Something" },
	{ 8, "40 Something" },
	{ 9, "50 or worse" },
	{ 10, "Man want women" },
	{ 11, "Women want men" },
	{ 0, NULL }
};

/*
 * All ICQv5 decryption code thanx to Sebastien Dault (daus01@gel.usherb.ca)
 */
static const guchar
table_v5 [] = {
 0x59, 0x60, 0x37, 0x6B, 0x65, 0x62, 0x46, 0x48, 0x53, 0x61, 0x4C, 0x59, 0x60, 0x57, 0x5B, 0x3D,
 0x5E, 0x34, 0x6D, 0x36, 0x50, 0x3F, 0x6F, 0x67, 0x53, 0x61, 0x4C, 0x59, 0x40, 0x47, 0x63, 0x39,
 0x50, 0x5F, 0x5F, 0x3F, 0x6F, 0x47, 0x43, 0x69, 0x48, 0x33, 0x31, 0x64, 0x35, 0x5A, 0x4A, 0x42,
 0x56, 0x40, 0x67, 0x53, 0x41, 0x07, 0x6C, 0x49, 0x58, 0x3B, 0x4D, 0x46, 0x68, 0x43, 0x69, 0x48,
 0x33, 0x31, 0x44, 0x65, 0x62, 0x46, 0x48, 0x53, 0x41, 0x07, 0x6C, 0x69, 0x48, 0x33, 0x51, 0x54,
 0x5D, 0x4E, 0x6C, 0x49, 0x38, 0x4B, 0x55, 0x4A, 0x62, 0x46, 0x48, 0x33, 0x51, 0x34, 0x6D, 0x36,
 0x50, 0x5F, 0x5F, 0x5F, 0x3F, 0x6F, 0x47, 0x63, 0x59, 0x40, 0x67, 0x33, 0x31, 0x64, 0x35, 0x5A,
 0x6A, 0x52, 0x6E, 0x3C, 0x51, 0x34, 0x6D, 0x36, 0x50, 0x5F, 0x5F, 0x3F, 0x4F, 0x37, 0x4B, 0x35,
 0x5A, 0x4A, 0x62, 0x66, 0x58, 0x3B, 0x4D, 0x66, 0x58, 0x5B, 0x5D, 0x4E, 0x6C, 0x49, 0x58, 0x3B,
 0x4D, 0x66, 0x58, 0x3B, 0x4D, 0x46, 0x48, 0x53, 0x61, 0x4C, 0x59, 0x40, 0x67, 0x33, 0x31, 0x64,
 0x55, 0x6A, 0x32, 0x3E, 0x44, 0x45, 0x52, 0x6E, 0x3C, 0x31, 0x64, 0x55, 0x6A, 0x52, 0x4E, 0x6C,
 0x69, 0x48, 0x53, 0x61, 0x4C, 0x39, 0x30, 0x6F, 0x47, 0x63, 0x59, 0x60, 0x57, 0x5B, 0x3D, 0x3E,
 0x64, 0x35, 0x3A, 0x3A, 0x5A, 0x6A, 0x52, 0x4E, 0x6C, 0x69, 0x48, 0x53, 0x61, 0x6C, 0x49, 0x58,
 0x3B, 0x4D, 0x46, 0x68, 0x63, 0x39, 0x50, 0x5F, 0x5F, 0x3F, 0x6F, 0x67, 0x53, 0x41, 0x25, 0x41,
 0x3C, 0x51, 0x54, 0x3D, 0x5E, 0x54, 0x5D, 0x4E, 0x4C, 0x39, 0x50, 0x5F, 0x5F, 0x5F, 0x3F, 0x6F,
 0x47, 0x43, 0x69, 0x48, 0x33, 0x51, 0x54, 0x5D, 0x6E, 0x3C, 0x31, 0x64, 0x35, 0x5A, 0x00, 0x00 };


static guint32
get_v5key(guint32 code, int len)
{
	guint32 a1, a2, a3, a4, a5;
	guint32 check, key;

	a1 = code & 0x0001f000;
	a2 = code & 0x07c007c0;
	a3 = code & 0x003e0001;
	a4 = code & 0xf8000000;
	a5 = code & 0x0000083e;

	a1 = a1 >> 0x0c;
	a2 = a2 >> 0x01;
	a3 = a3 << 0x0a;
	a4 = a4 >> 0x10;
	a5 = a5 << 0x0f;

	check = a5 + a1 + a2 + a3 + a4;
	key = len * 0x68656C6C;
	key += check;
	return key;
}

static void
decrypt_v5(guchar *bfr, guint32 size,guint32 key)
{
	guint32 i;
	guint32 k;

	for (i=ICQ5_CL_SESSIONID; i < size; i+=4 ) {
		k = key+table_v5[i&0xff];
		if ( i != 0x16 ) {
			bfr[i] ^= (guchar)(k & 0xff);
			bfr[i+1] ^= (guchar)((k & 0xff00)>>8);
		}
		if ( i != 0x12 ) {
			bfr[i+2] ^= (guchar)((k & 0xff0000)>>16);
			bfr[i+3] ^= (guchar)((k & 0xff000000)>>24);
		}
	}
}

/*
 * The packet has, at offset "offset" a (len, string) pair.
 * Display the length and string in the tree.
 *
 * If anything is wrong, return -1, since -1 is not a valid string
 * length. Else, return the number of chars processed.
 */
static guint16
proto_add_icq_attr(proto_tree* tree, /* The tree to add to */
			tvbuff_t *tvb,    /* Tvbuff with packet */
			const int offset, /* Offset from the start of packet of field */
			const char* descr)	/* The description to use in the tree */
{
	guint16 len;

	len = tvb_get_letohs(tvb, offset);
	if (len > tvb_reported_length_remaining(tvb, offset))
		return -1;	/* length goes past end of packet */
	proto_tree_add_text(tree, tvb, offset, len+2,
			"%s[%u]: %.*s", descr, len, len,
			tvb_get_ephemeral_string(tvb, offset + 2, len));
	return len + 2;
}

static void
icqv5_decode_msgType(proto_tree* tree, tvbuff_t *tvb, int offset, int size,
		     packet_info *pinfo)
{
	proto_item *ti, *msg_item;
	proto_tree *subtree;
	int left = size;
	guint16 msgType;
	gint sep_offset;
	int sz;            /* Size of the current element */
	unsigned int n;
	static const char* url_field_descr[] = {
		"Description",
		"URL",
	};
#define N_URL_FIELDS	(sizeof url_field_descr / sizeof url_field_descr[0])
	static const char* email_field_descr[] = {
		"Nickname",
		"First name",
		"Last name",
		"Email address",
		"Unknown",
		"Text"
	};
#define N_EMAIL_FIELDS	(sizeof email_field_descr / sizeof email_field_descr[0])
	static const char* auth_req_field_descr[] = {
		"Nickname",
		"First name",
		"Last name",
		"Email address",
		"Unknown",
		"Reason"
	};
#define N_AUTH_REQ_FIELDS	(sizeof auth_req_field_descr / sizeof auth_req_field_descr[0])
	static const char* user_added_field_descr[] = {
		"Nickname",
		"First name",
		"Last name",
		"Email address",
	};
#define N_USER_ADDED_FIELDS	(sizeof user_added_field_descr / sizeof user_added_field_descr[0])

	msgType = tvb_get_letohs(tvb, offset);
	ti = proto_tree_add_text(tree, tvb, offset, size,
				 "%s Message", val_to_str_const(msgType, msgTypeCode, "Unknown"));
	subtree = proto_item_add_subtree(ti, ett_icq_body_parts);

	msg_item = proto_tree_add_item(subtree, hf_icq_msg_type, tvb, offset, 2, ENC_LITTLE_ENDIAN);
	offset += 2;
	left -= 2;
	if (msgType != MSG_AUTH) {
		/*
		 * XXX - does a MSG_AUTH message really have 3 bytes of information
		 * rather than a length field?
		 */
		proto_tree_add_text(subtree, tvb, offset, 2, "Length: %u",
					tvb_get_letohs(tvb, offset));
		offset += 2;
		left -= 2;
	}

	switch(msgType) {
	case 0xffff:           /* Field unknown */
	break;
	default:
		expert_add_info_format_text(pinfo, msg_item, &ei_icq_unknown_command,
				   "Unknown msgType: %u (0x%x)", msgType, msgType);
		break;
	case MSG_TEXT:
		proto_tree_add_text(subtree, tvb, offset, left, "Msg: %.*s", left-1,
					tvb_get_ephemeral_string(tvb, offset, left));
		break;
	case MSG_URL:
		for (n = 0; n < N_URL_FIELDS; n++) {
			if (n != N_URL_FIELDS - 1) {
				sep_offset = tvb_find_guint8(tvb, offset, left, 0xfe);
				sz = sep_offset - offset + 1;
			} else {
				sz = left;
			}
			if (sz != 0) {
				proto_tree_add_text(subtree, tvb, offset, sz, "%s: %.*s",
							url_field_descr[n],
							sz - 1,
							tvb_get_ephemeral_string(tvb, offset, sz));
			} else {
				proto_tree_add_text(subtree, tvb, offset, 0,
							"%s: %s", url_field_descr[n], "(empty)");
			}
			offset += sz;
			left -= sz;
		}
		break;
	case MSG_EMAIL:
	for (n = 0; n < N_EMAIL_FIELDS; n++) {
		if (n != N_EMAIL_FIELDS - 1) {
			sep_offset = tvb_find_guint8(tvb, offset, left, 0xfe);
			sz = sep_offset - offset + 1;
		} else {
			sz = left;
		}
		if (sz != 0) {
			proto_tree_add_text(subtree, tvb, offset, sz, "%s: %.*s",
						email_field_descr[n],
						sz - 1,
						tvb_get_ephemeral_string(tvb, offset, sz));
		} else {
			proto_tree_add_text(subtree, tvb, offset, 0, "%s: %s",
						email_field_descr[n], "(empty)");
		}
		offset += sz;
		left -= sz;
	}
	break;

	case MSG_AUTH:
	{
		/* Three bytes, first is a char signifying success */
		unsigned char auth_suc;

		auth_suc = tvb_get_guint8(tvb, offset);
		proto_tree_add_text(subtree, tvb, offset, 1,
					"Authorization: (%u) %s",auth_suc,
					(auth_suc==0)?"Denied":"Allowed");
		offset++;
		proto_tree_add_text(subtree, tvb, offset, 2, "x1: 0x%04x",
					tvb_get_letohs(tvb, offset));
		break;
	}
	case MSG_AUTH_REQ:
		for (n = 0; n < N_AUTH_REQ_FIELDS; n++) {
			if (n != N_AUTH_REQ_FIELDS - 1) {
				sep_offset = tvb_find_guint8(tvb, offset, left, 0xfe);
				sz = sep_offset - offset + 1;
			} else {
				sz = left;
			}
			if (sz != 0) {
				proto_tree_add_text(subtree, tvb, offset, sz, "%s: %.*s",
							auth_req_field_descr[n], sz - 1,
							tvb_get_ephemeral_string(tvb, offset, sz));
			} else {
				proto_tree_add_text(subtree, tvb, offset, 0, "%s: %s",
							auth_req_field_descr[n], "(empty)");
			}
			offset += sz;
			left -= sz;
		}
		break;
	case MSG_USER_ADDED:
	for (n = 0; n < N_USER_ADDED_FIELDS; n++) {
		if (n != N_USER_ADDED_FIELDS - 1) {
			sep_offset = tvb_find_guint8(tvb, offset, left, 0xfe);
			sz = sep_offset - offset + 1;
		} else {
			sz = left;
		}
		if (sz != 0) {
			proto_tree_add_text(subtree, tvb, offset, sz, "%s: %.*s",
						user_added_field_descr[n], sz - 1,
						tvb_get_ephemeral_string(tvb, offset, sz));
		} else {
			proto_tree_add_text(subtree, tvb, offset, 0, "%s: %s",
						user_added_field_descr[n], "(empty)");
		}
		offset += sz;
		left -= sz;
	}
	break;
	case MSG_CONTACTS:
	{
		gint sep_offset_prev;
		int sz_local = 0;            /* Size of the current element */
		int n_local = 0;             /* The nth element */
		gboolean last = FALSE;

		while (!last) {
			sep_offset = tvb_find_guint8(tvb, offset, left, 0xfe);
			if (sep_offset != -1) {
				sz_local = sep_offset - offset + 1;
			}
			else {
				sz_local = left;
				last = TRUE;
			}

			if (n_local == 0) {
				/* The first element is the number of Nick/UIN pairs follow */
				proto_tree_add_text(subtree, tvb, offset, sz_local,
							"Number of pairs: %.*s", sz_local - 1,
							tvb_get_ephemeral_string(tvb, offset, sz_local));
				n_local++;
			} else if (!last) {
				int svsz = sz_local;

				left -= sz_local;
				sep_offset_prev = sep_offset;
				sep_offset = tvb_find_guint8(tvb, sep_offset_prev, left, 0xfe);
				if (sep_offset != -1)
					sz_local = sep_offset - offset + 1;
				else {
					sz_local = left;
					last = TRUE;
				}
				proto_tree_add_text(subtree, tvb, offset, sz_local + svsz,
							"%.*s: %.*s", svsz - 1,
							tvb_get_ephemeral_string(tvb, offset, svsz), sz_local - 1,
							tvb_get_ephemeral_string(tvb, sep_offset_prev + 1, sz_local));
				n_local += 2;
			}

			left -= (sz_local+1);
			offset = sep_offset + 1;
		}
		break;
	}
	}
}

/*********************************
 *
 * Client commands
 *
 *********************************/
static void
icqv5_cmd_send_text_code(proto_tree* tree, /* Tree to put the data in */
			 tvbuff_t *tvb,    /* Decrypted packet content */
			 int offset)       /* Offset from the start of the packet to the content */
{
	proto_tree* subtree = tree;
	guint16 len;
	guint16 x1;

	len = tvb_get_letohs(tvb, offset+CMD_SEND_TEXT_CODE_LEN);
	proto_tree_add_text(subtree, tvb, offset + CMD_SEND_TEXT_CODE_LEN,
				2, "Length: %d", len);

	if (len>0) {
		proto_tree_add_text(subtree, tvb, offset + CMD_SEND_TEXT_CODE_TEXT,
				len, "Text: %.*s", len,
				tvb_get_ephemeral_string(tvb, offset + CMD_SEND_TEXT_CODE_TEXT,
						len));
	}

	x1 = tvb_get_letohs(tvb, offset + CMD_SEND_TEXT_CODE_TEXT + len);
	proto_tree_add_text(subtree, tvb,
				offset + CMD_SEND_TEXT_CODE_TEXT + len,
				2, "X1: 0x%04x", x1);
}

static void
icqv5_cmd_send_msg(proto_tree* tree, tvbuff_t *tvb, int offset, int size,
		   packet_info *pinfo)
{
	proto_tree_add_text(tree, tvb, offset + CMD_SEND_MSG_RECV_UIN, 4,
				"Receiver UIN: %u",
				tvb_get_letohl(tvb, offset + CMD_SEND_MSG_RECV_UIN));
	size -= 4;

	icqv5_decode_msgType(tree, tvb, offset + CMD_SEND_MSG_MSG_TYPE,
				 size, pinfo);
}

static void
icqv5_cmd_login(proto_tree* tree, tvbuff_t *tvb, int offset)
{
	proto_tree* subtree = tree;
	time_t theTime;
	char *aTime;
	guint32 port;
	guint32 passwdLen;

	if (tree) {
		theTime = tvb_get_letohl(tvb, offset + CMD_LOGIN_TIME);
		aTime = abs_time_secs_to_str(theTime, ABSOLUTE_TIME_LOCAL, TRUE);
		proto_tree_add_text(subtree, tvb, offset + CMD_LOGIN_TIME, 4,
					"Time: %ld = %s", (long)theTime, aTime);
		port = tvb_get_letohl(tvb, offset + CMD_LOGIN_PORT);
		proto_tree_add_text(subtree, tvb, offset + CMD_LOGIN_PORT, 4,
					"Port: %u", port);
		passwdLen = tvb_get_letohs(tvb, offset + CMD_LOGIN_PASSLEN);
		proto_tree_add_text(subtree, tvb, offset + CMD_LOGIN_PASSLEN,
					2 + passwdLen, "Passwd: %.*s", (int)passwdLen,
					tvb_get_ephemeral_string(tvb, offset + CMD_LOGIN_PASSWD,
						passwdLen));
		proto_tree_add_text(subtree, tvb,
					offset + CMD_LOGIN_PASSWD + passwdLen + CMD_LOGIN_IP,
					4, "IP: %s", tvb_ip_to_str(tvb, offset + CMD_LOGIN_PASSWD + passwdLen + CMD_LOGIN_IP));
		proto_tree_add_item(subtree, hf_icq_status, tvb, offset + CMD_LOGIN_PASSWD + passwdLen + CMD_LOGIN_STATUS, 4, ENC_LITTLE_ENDIAN);
	}
}

static void
icqv5_cmd_contact_list(proto_tree* tree, tvbuff_t *tvb, int offset)
{
	unsigned char num;
	int i;
	guint32 uin;

	if (tree) {
		num = tvb_get_guint8(tvb, offset + CMD_CONTACT_LIST_NUM);
		proto_tree_add_text(tree, tvb, offset + CMD_CONTACT_LIST,
					1, "Number of uins: %u", num);
		/*
		 * A sequence of num times UIN follows
		 */
		offset += (CMD_CONTACT_LIST_NUM + 1);
		for (i = 0; i < num; i++) {
			uin = tvb_get_letohl(tvb, offset);
			proto_tree_add_text(tree, tvb, offset, 4,
					"UIN[%d]: %u", i ,uin);
			offset += 4;
		}
	}
}

/**********************
 *
 * Server commands
 *
 **********************
 */
static void
icqv5_srv_user_online(proto_tree* tree,/* Tree to put the data in */
				tvbuff_t *tvb,   /* Tvbuff with packet */
				int offset)      /* Offset from the start of the packet to the content */
{
	proto_tree* subtree = tree;

	if (tree) {
		proto_tree_add_text(subtree, tvb, offset + SRV_USER_ONL_UIN, 4,
					"UIN: %u",
					tvb_get_letohl(tvb, offset + SRV_USER_ONL_UIN));
		proto_tree_add_text(subtree, tvb, offset + SRV_USER_ONL_IP, 4,
					"IP: %s", tvb_ip_to_str(tvb, offset + SRV_USER_ONL_IP));
		proto_tree_add_text(subtree, tvb, offset + SRV_USER_ONL_PORT, 4,
					"Port: %u",
					tvb_get_letohl(tvb, offset + SRV_USER_ONL_PORT));
		proto_tree_add_text(subtree, tvb, offset + SRV_USER_ONL_REALIP, 4,
					"RealIP: %s", tvb_ip_to_str(tvb, offset + SRV_USER_ONL_REALIP));
		proto_tree_add_item(subtree, hf_icq_status, tvb, offset + SRV_USER_ONL_STATUS, 2, ENC_LITTLE_ENDIAN);
		/*
		 * Kojak: Hypothesis is that this field might be an encoding for the
		 * version used by the UIN that changed. To test this, I included
		 * this line to the code.
		 */
		proto_tree_add_text(subtree, tvb, offset + SRV_USER_ONL_X2, 4,
					"Version: %08x",
					tvb_get_letohl(tvb, offset + SRV_USER_ONL_X2));
	}
}

static void
icqv5_srv_multi(proto_tree* tree, /* Tree to put the data in */
		tvbuff_t *tvb,    /* Packet content */
		int offset,       /* Offset from the start of the packet to the content */
		packet_info* pinfo)
{
	guint8 num;
	guint16 pktSz;
	int i;

	num = tvb_get_guint8(tvb, offset + SRV_MULTI_NUM);
	proto_tree_add_text(tree, tvb, offset + SRV_MULTI_NUM, 1,
				"Number of pkts: %u", num);
	/*
	 * A sequence of num times ( pktsize, packetData) follows
	 */
	offset += (SRV_MULTI_NUM + 1);
	for (i = 0; i < num; i++) {
		pktSz = tvb_get_letohs(tvb, offset);
		offset += 2;
		dissect_icqv5Server(tvb, offset, pinfo, tree, pktSz);
		offset += pktSz;
	}
}

static void
icqv5_srv_meta_user(proto_tree* tree, /* Tree to put the data in */
		    tvbuff_t *tvb,    /* Tvbuff with packet */
		    int offset,       /* Offset from the start of the packet to the content */
		    int size _U_,         /* Number of chars left to do */
		    packet_info *pinfo)
{
	proto_tree* sstree;
	proto_item* ti;
	guint16 subcmd;
	unsigned char result;

	subcmd = tvb_get_letohs(tvb, offset + SRV_META_USER_SUBCMD);
	ti = proto_tree_add_item(tree, hf_icq_meta_user_subcmd, tvb, offset + SRV_META_USER_SUBCMD, 2, ENC_LITTLE_ENDIAN);
	sstree = proto_item_add_subtree(ti, ett_icq_body_parts);
	result = tvb_get_guint8(tvb, offset + SRV_META_USER_RESULT);
	proto_tree_add_text(sstree, tvb, offset + SRV_META_USER_RESULT,
				1, "%s", (result==0x0a)?"Success":"Failure");

	/* Skip the META_USER header */
	offset += 3;

	switch(subcmd) {
	case META_EX_USER_FOUND:
	{
		/* This is almost the same as META_USER_FOUND,
		 * however, there's an extra length field
		 */
		guint16 pktLen;

		/* Read the length field */
		pktLen = tvb_get_letohs(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 2,
				"Length: %u", pktLen);

		offset += 2;
	}
	/* FALLTHRU */
	case META_USER_FOUND:
	{
		/* The goto mentioned in this block should be local to this
		 * block if C'd allow it.
		 *
		 * They are used to "implement" a poorman's exception handling
		 */
		int len = 0;
		static const char *descr[] = {
			"Nick",
			"First name",
			"Last name",
			"Email",
			NULL
		};
		const char** d = descr;
		unsigned char auth;
		/*
		 * Read UIN
		 */
		proto_tree_add_text(sstree, tvb, offset, 4,
				"UIN: %u", tvb_get_letohl(tvb, offset));
		offset+=4;

		for ( ; *d!=NULL; d++) {
			len = proto_add_icq_attr(sstree, tvb, offset, *d);
			if (len == -1)
				return;
			offset += len;
		}
		/* Get the authorize setting */
		auth = tvb_get_guint8(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 1,
				"authorization: %s", (auth==0x01)?"Necessary":"Who needs it");
		offset++;
		/* Get x2 */
		proto_tree_add_text(sstree, tvb, offset, 2,
				"x2: 0x%04x", tvb_get_letohs(tvb, offset));
		offset+=2;
		/* Get x3 */
		proto_tree_add_text(sstree, tvb, offset, 4,
				"x3: 0x%08x", tvb_get_letohl(tvb, offset));
		break;
	}
	case META_ABOUT:
	{
		int len;

		/* Get the about information */
		len = tvb_get_letohs(tvb, offset);
		offset+=2;
		proto_tree_add_text(sstree, tvb, offset - 2,
				len+2, "About(%d): %.*s", len,
				len, tvb_get_ephemeral_string(tvb, offset, len));
		break;
	}
	case META_USER_INFO:
	{
		/* The goto mentioned in this block should be local to this
		 * block if C'd allow it.
		 *
		 * They are used to "implement" a poorman's exception handling
		 */
		static const char* descr[] = {
			"Nick",
			"First name",
			"Last name",
			"Primary email",
			"Secondary email",
			"Old email",
			"City",
			"State",
			"Phone",
			"Fax",
			"Street",
			"Cellphone",
			"Zip",
			NULL
		};
		const char** d = descr;
		guint16 country;
		guint8 user_timezone;
		guint8 auth;
		int len = 0;
#if 0
		/* Get the uin */
		uin = tvb_get_letohl(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 4, "UIN: %u", uin);
		offset+=4;
#endif

		/*
		 * Get every field from the description
		 */
		for ( ; *d!=NULL; d++) {
			len = proto_add_icq_attr(sstree, tvb, offset, *d);
			if (len < 0) {
				offset+=2;
				continue;
			}
			offset+=len;
		}
		/* Get country code */
		country = tvb_get_letohs(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 2,
				"Countrycode: %u", country);
		offset+=2;
		/* Get the timezone setting */
		user_timezone = tvb_get_guint8(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 1,
				"Timezone: %u", user_timezone);
		offset++; 
		/* Get the authorize setting */
		auth = tvb_get_guint8(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 1,
				"Authorization: (%u) %s", auth,
				(auth==0)?"No":"Yes");
		offset++;
		/* Get the webaware setting */
		auth = tvb_get_guint8(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 1,
				"Webaware: (%u) %s", auth,
				(auth==0)?"No":"Yes");
		offset++; 
		/* Get the authorize setting */
		auth = tvb_get_guint8(tvb, offset);
		proto_tree_add_text(sstree, tvb, offset, 1,
				"HideIP: (%u) %s", auth, (auth==0)?"No":"Yes");
		break;
	}
	default:
		/* This information is already printed in the tree */
		expert_add_info_format_text(pinfo, ti, &ei_icq_unknown_meta_subcmd,
				   "Unknown Meta subcmd: 0x%x", subcmd);
		break;
	}
}

static void
icqv5_srv_recv_message(proto_tree* tree, /* Tree to put the data in */
						tvbuff_t* tvb,    /* Packet content */
						int offset,       /* Offset from the start of the packet to the content */
						int size,         /* Number of chars left to do */
						packet_info *pinfo)
{
	guint16 year;
	guint8 month;
	guint8 day;
	guint8 hour;
	guint8 minute;

	proto_tree_add_item(tree, hf_icq_uin, tvb, offset + SRV_RECV_MSG_UIN,
				4, ENC_LITTLE_ENDIAN);
	year = tvb_get_letohs(tvb, offset + SRV_RECV_MSG_YEAR);
	month = tvb_get_guint8(tvb, offset + SRV_RECV_MSG_MONTH);
	day = tvb_get_guint8(tvb, offset + SRV_RECV_MSG_DAY);
	hour = tvb_get_guint8(tvb, offset + SRV_RECV_MSG_HOUR);
	minute = tvb_get_guint8(tvb, offset + SRV_RECV_MSG_MINUTE);

	proto_tree_add_text(tree, tvb, offset + SRV_RECV_MSG_YEAR,
				2 + 4,
				"Time: %u-%u-%u %02u:%02u",
				day, month, year, hour, minute);
	icqv5_decode_msgType(tree, tvb, offset + SRV_RECV_MSG_MSG_TYPE,
				 size-10, pinfo);
}

static void
icqv5_srv_rand_user(proto_tree* tree,      /* Tree to put the data in */
		    tvbuff_t *tvb,         /* Tvbuff with packet */
		    int offset)            /* Offset from the start of the packet to the content */
{
	proto_tree* subtree = tree;
	guint32 uin;
	guint32 port;
	guint8 commClass;
	guint16 tcpVer;

	if (tree) {
		/* guint32 UIN */
		uin = tvb_get_letohl(tvb, offset + SRV_RAND_USER_UIN);
		proto_tree_add_text(subtree, tvb, offset + SRV_RAND_USER_UIN,
					4, "UIN: %u", uin);
		/* guint32 IP */
		proto_tree_add_text(subtree, tvb, offset + SRV_RAND_USER_IP,
					4, "IP: %s", tvb_ip_to_str(tvb, offset + SRV_RAND_USER_IP));
		/* guint16 portNum */
		/* XXX - 16 bits, or 32 bits? */
		port = tvb_get_letohs(tvb, offset + SRV_RAND_USER_PORT);
		proto_tree_add_text(subtree, tvb, offset + SRV_RAND_USER_UIN,
					4, "Port: %u", port);
		/* guint32 realIP */
		proto_tree_add_text(subtree, tvb, offset + SRV_RAND_USER_REAL_IP,
					4, "RealIP: %s", tvb_ip_to_str(tvb, offset + SRV_RAND_USER_REAL_IP));
		/* guint8 Communication Class */
		commClass = tvb_get_guint8(tvb, offset + SRV_RAND_USER_CLASS);
		proto_tree_add_text(subtree, tvb, offset + SRV_RAND_USER_CLASS,
					1, "Class: %s",
					(commClass!=4)?"User to User":"Through Server");
		/* guint32 status */
		/* XXX - 16 bits, or 32 bits? */
		proto_tree_add_item(subtree, hf_icq_status, tvb, offset + SRV_RAND_USER_STATUS, 4, ENC_LITTLE_ENDIAN);

		/* guint16 tcpVersion */
		tcpVer = tvb_get_letohs(tvb, offset + SRV_RAND_USER_TCP_VER);
		proto_tree_add_text(subtree, tvb, offset + SRV_RAND_USER_TCP_VER,
					2, "TCPVersion: %u", tcpVer);
	}
}

/*
 * Dissect all the v5 client traffic. This is encrypted, so be careful.
 */
static void
dissect_icqv5Client(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	proto_tree *icq_header_tree, *icq_body_tree;
	proto_item *ti, *cmd_item;

	int pktsize;		/* The actual size of the ICQ content */
	int capturedsize;		/* The captured size of the ICQ content */
	guint32 rounded_size;
	guint32 code, key;
	guint16 cmd;
	guint8 *decr_pd;		/* Decrypted content */
	tvbuff_t *decr_tvb;

	pktsize = tvb_reported_length(tvb);
	capturedsize = tvb_length(tvb);

	/* Get the encryption key */
	code = tvb_get_letohl(tvb, ICQ5_CL_CHECKCODE);
	key = get_v5key(code, pktsize);

	/*
	 * Make a copy of the packet data, and decrypt it.
	 * The decryption processes 4 bytes at a time, and starts at
	 * an offset of ICQ5_CL_SESSIONID (which isn't a multiple of 4),
	 * so we make sure that there are
	 *
	 *	(ICQ5_CL_SESSIONID + a multiple of 4)
	 *
	 * bytes in the buffer.
	 */
	rounded_size = ((((capturedsize - ICQ5_CL_SESSIONID) + 3)/4)*4) + ICQ5_CL_SESSIONID;
	/* rounded_size might exceed the tvb bounds so we can't just use tvb_memdup here. */
	decr_pd = (guint8 *)g_malloc(rounded_size);
	tvb_memcpy(tvb, decr_pd, 0, capturedsize);
	decrypt_v5(decr_pd, rounded_size, key);

	/* Allocate a new tvbuff, referring to the decrypted data. */
	decr_tvb = tvb_new_child_real_data(tvb, decr_pd, capturedsize, pktsize);

	/* Arrange that the allocated packet data copy be freed when the
	   tvbuff is freed. */
	tvb_set_free_cb(decr_tvb, g_free);

	/* Add the decrypted data to the data source list. */
	add_new_data_source(pinfo, decr_tvb, "Decrypted");

	cmd = tvb_get_letohs(decr_tvb, ICQ5_CL_CMD);

	col_add_fstr(pinfo->cinfo, COL_INFO, "ICQv5 %s", val_to_str_const(cmd, clientCmdCode, "Unknown"));

	ti = proto_tree_add_text(tree, tvb, 0, ICQ5_CL_HDRSIZE, "Header");
	icq_header_tree = proto_item_add_subtree(ti, ett_icq_header);

	ti = proto_tree_add_boolean(icq_header_tree, hf_icq_type, tvb, 0, 0, ICQ5_CLIENT);
	PROTO_ITEM_SET_GENERATED(ti);

	proto_tree_add_item(icq_header_tree, hf_icq_version, tvb, ICQ_VERSION, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_uin, tvb, ICQ5_CL_UIN, 4, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_sessionid, decr_tvb, ICQ5_CL_SESSIONID, 4, ENC_LITTLE_ENDIAN);
	cmd_item = proto_tree_add_item(icq_header_tree, hf_icq_client_cmd, decr_tvb, ICQ5_CL_CMD, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_seqnum1, decr_tvb, ICQ5_CL_SEQNUM1, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_seqnum2, decr_tvb, ICQ5_CL_SEQNUM2, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_checkcode, tvb, ICQ5_CL_CHECKCODE, 4, ENC_LITTLE_ENDIAN);
	ti = proto_tree_add_uint(icq_header_tree, hf_icq_checkcode_key, tvb, ICQ5_CL_CHECKCODE, 4, key);
	PROTO_ITEM_SET_GENERATED(ti);

	ti = proto_tree_add_text(tree, decr_tvb, ICQ5_CL_HDRSIZE, pktsize - ICQ5_CL_HDRSIZE, "Body");
	icq_body_tree = proto_item_add_subtree(ti, ett_icq_body);

	switch(cmd) {
	case CMD_ACK:
		proto_tree_add_item(icq_body_tree, hf_icq_ack_random, decr_tvb, ICQ5_CL_HDRSIZE + CMD_ACK_RANDOM, 4, ENC_LITTLE_ENDIAN);
		break;
	case CMD_SEND_MSG:
	case CMD_MSG_TO_NEW_USER:
		icqv5_cmd_send_msg(icq_body_tree, decr_tvb, ICQ5_CL_HDRSIZE,
				   pktsize - ICQ5_CL_HDRSIZE, pinfo);
		break;
	case CMD_RAND_SEARCH:
		proto_tree_add_item(icq_body_tree, hf_icq_group, decr_tvb, ICQ5_CL_HDRSIZE + CMD_RAND_SEARCH_GROUP, 4, ENC_LITTLE_ENDIAN);
		break;
	case CMD_LOGIN:
		icqv5_cmd_login(icq_body_tree, decr_tvb, ICQ5_CL_HDRSIZE);
		break;
	case CMD_SEND_TEXT_CODE:
		icqv5_cmd_send_text_code(icq_body_tree, decr_tvb, ICQ5_CL_HDRSIZE);
		break;
	case CMD_STATUS_CHANGE:
		proto_tree_add_item(icq_body_tree, hf_icq_status, decr_tvb, ICQ5_CL_HDRSIZE + CMD_STATUS_CHANGE_STATUS, 4, ENC_LITTLE_ENDIAN);
		break;
	case CMD_ACK_MESSAGES:
		proto_tree_add_item(icq_body_tree, hf_icq_ack_random, decr_tvb, ICQ5_CL_HDRSIZE + CMD_ACK_MESSAGES_RANDOM, 4, ENC_LITTLE_ENDIAN);
		break;
	case CMD_KEEP_ALIVE:
		proto_tree_add_item(icq_body_tree, hf_icq_keep_alive_random, decr_tvb, ICQ5_CL_HDRSIZE + CMD_KEEP_ALIVE_RANDOM, 4, ENC_LITTLE_ENDIAN);
		break;
	case CMD_ADD_TO_LIST:
		proto_tree_add_item(icq_body_tree, hf_icq_uin, decr_tvb, ICQ5_CL_HDRSIZE + CMD_ADD_TO_LIST_UIN, 4, ENC_LITTLE_ENDIAN);
		break;
	case CMD_CONTACT_LIST:
		icqv5_cmd_contact_list(icq_body_tree, decr_tvb, ICQ5_CL_HDRSIZE);
		break;
	case CMD_META_USER:
	case CMD_REG_NEW_USER:
	case CMD_QUERY_SERVERS:
	case CMD_QUERY_ADDONS:
		proto_tree_add_text(icq_body_tree, tvb, ICQ5_CL_HDRSIZE, 0, "No parameters");
		break;
	default:
		expert_add_info(pinfo, cmd_item, &ei_icq_unknown_command);
		break;
	}
}

static void
dissect_icqv5Server(tvbuff_t *tvb, int offset, packet_info *pinfo,
			proto_tree *tree, int pktsize)
{
	/* Server traffic is easy, not encrypted */
	proto_tree *icq_header_tree, *icq_body_tree;
	proto_item *ti, *cmd_item;

	guint16 cmd = tvb_get_letohs(tvb, offset + ICQ5_SRV_CMD);

	if (pktsize == -1) {
		col_add_fstr(pinfo->cinfo, COL_INFO, "ICQv5 %s", val_to_str_const(cmd, serverCmdCode, "Unknown"));
		pktsize = tvb_reported_length(tvb);
	}

	ti = proto_tree_add_text(tree, tvb, offset, ICQ5_SRV_HDRSIZE, "Header");
	icq_header_tree = proto_item_add_subtree(ti, ett_icq_header);

	ti = proto_tree_add_boolean(icq_header_tree, hf_icq_type, tvb, 0, 0, ICQ5_SERVER);
	PROTO_ITEM_SET_GENERATED(ti);

	proto_tree_add_item(icq_header_tree, hf_icq_version, tvb, offset + ICQ_VERSION, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_sessionid, tvb, offset + ICQ5_SRV_SESSIONID, 4, ENC_LITTLE_ENDIAN);
	cmd_item = proto_tree_add_item(icq_header_tree, hf_icq_server_cmd, tvb, offset + ICQ5_SRV_CMD, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_seqnum1, tvb, offset + ICQ5_SRV_SEQNUM1, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_seqnum2, tvb, offset + ICQ5_SRV_SEQNUM2, 2, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_uin, tvb, offset + ICQ5_SRV_UIN, 4, ENC_LITTLE_ENDIAN);
	proto_tree_add_item(icq_header_tree, hf_icq_checkcode, tvb, offset + ICQ5_SRV_CHECKCODE, 4, ENC_LITTLE_ENDIAN);

	ti = proto_tree_add_text(tree, tvb, ICQ5_CL_HDRSIZE, pktsize - ICQ5_SRV_HDRSIZE, "Body");
	icq_body_tree = proto_item_add_subtree(ti, ett_icq_body);

	switch (cmd) {
	case SRV_RAND_USER:
		icqv5_srv_rand_user(icq_body_tree, tvb, offset + ICQ5_SRV_HDRSIZE);
		break;
	case SRV_SYS_DELIVERED_MESS:
		/* The message structures are all the same. Why not run
		 * the same routine? */
		icqv5_cmd_send_msg(icq_body_tree, tvb, offset + ICQ5_SRV_HDRSIZE,
				   pktsize - ICQ5_SRV_HDRSIZE, pinfo);
		break;
	case SRV_USER_ONLINE:
		icqv5_srv_user_online(icq_body_tree, tvb, offset + ICQ5_SRV_HDRSIZE);
		break;
	case SRV_USER_OFFLINE:
		proto_tree_add_item(icq_body_tree, hf_icq_uin, tvb, offset + ICQ5_SRV_HDRSIZE + SRV_USER_OFFLINE_UIN, 4, ENC_LITTLE_ENDIAN);
		break;
	case SRV_LOGIN_REPLY:
		proto_tree_add_text(tree, tvb, offset + ICQ5_SRV_HDRSIZE + SRV_LOGIN_REPLY_IP, 4,
				"IP: %s", tvb_ip_to_str(tvb, offset + ICQ5_SRV_HDRSIZE + SRV_LOGIN_REPLY_IP));
		break;
	case SRV_META_USER:
		icqv5_srv_meta_user(icq_body_tree, tvb, offset + ICQ5_SRV_HDRSIZE,
				pktsize - ICQ5_SRV_HDRSIZE, pinfo);
		break;
	case SRV_RECV_MESSAGE:
		icqv5_srv_recv_message(icq_body_tree, tvb, offset + ICQ5_SRV_HDRSIZE,
				   pktsize - ICQ5_SRV_HDRSIZE, pinfo);
		break;
	case SRV_MULTI:
		icqv5_srv_multi(icq_body_tree, tvb, offset + ICQ5_SRV_HDRSIZE, pinfo);
		break;
	case SRV_ACK:
	case SRV_SILENT_TOO_LONG:
	case SRV_GO_AWAY:
	case SRV_NEW_UIN:
	case SRV_BAD_PASS:
	case SRV_UPDATE_SUCCESS:
		proto_tree_add_text(icq_body_tree, tvb, offset + ICQ5_SRV_HDRSIZE, 0, "No Parameters");
		break;
	default:
		expert_add_info(pinfo, cmd_item, &ei_icq_unknown_command);
		break;
	}
}

static void dissect_icqv5(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
	if (tvb_get_letohl(tvb, ICQ5_UNKNOWN) == 0) {
		dissect_icqv5Client(tvb, pinfo, tree);
	} else {
		dissect_icqv5Server(tvb, 0, pinfo, tree, -1);
	}
}

static int
dissect_icq(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data _U_)
{
	int version;
	proto_item *ti;
	proto_tree *icq_tree;

	version = tvb_get_letohs(tvb, ICQ_VERSION);
	if (version < 2 || version > 5)
		return 0;	/* This is not a (recognized) ICQ packet */

	col_add_fstr(pinfo->cinfo, COL_PROTOCOL, "ICQv%d", version);
	col_add_fstr(pinfo->cinfo, COL_INFO, "ICQ Version %d protocol", version);

	ti = proto_tree_add_protocol_format(tree, proto_icq, tvb, 0, -1, "ICQv%d", version);
	icq_tree = proto_item_add_subtree(ti, ett_icq);

	if (version == 5)
	{
		dissect_icqv5(tvb, pinfo, icq_tree);
	}
	else
	{
		proto_tree_add_item(icq_tree, hf_icq_version, tvb, ICQ_VERSION, 2, ENC_LITTLE_ENDIAN);
	}

	return (tvb_length(tvb));
}

/* registration with the filtering engine */
void
proto_register_icq(void)
{
    static hf_register_info hf[] = {
	{ &hf_icq_version,
	  {"Version", "icq.version", FT_UINT16, BASE_DEC, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_type,
	  {"Client/Server", "icq.client", FT_BOOLEAN, BASE_NONE, TFS(&tfs_client_server), 0x0, NULL, HFILL }},
	{ &hf_icq_msg_type,
	  {"Type", "icq.msg_type", FT_UINT16, BASE_DEC, VALS(msgTypeCode), 0x0, NULL, HFILL }},
	{ &hf_icq_uin,
	  {"UIN", "icq.uin", FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_sessionid,
	  {"Session ID", "icq.sessionid", FT_UINT32, BASE_HEX, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_client_cmd,
	  {"Client command", "icq.client_cmd", FT_UINT16, BASE_DEC, VALS(clientCmdCode), 0x0, NULL, HFILL }},
	{ &hf_icq_server_cmd,
	  {"Server command", "icq.server_cmd", FT_UINT16, BASE_DEC, VALS(serverCmdCode), 0x0, NULL, HFILL }},
	{ &hf_icq_checkcode,
	  {"Checkcode", "icq.checkcode", FT_UINT32, BASE_HEX, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_checkcode_key,
	  {"Key", "icq.checkcode_key", FT_UINT32, BASE_HEX, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_seqnum1,
	  {"Seq Number 1", "icq.seqnum1", FT_UINT16, BASE_HEX, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_seqnum2,
	  {"Seq Number 2", "icq.seqnum2", FT_UINT16, BASE_HEX, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_group,
	  {"Group", "icq.group", FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_ack_random,
	  {"Random", "icq.ack.random", FT_UINT32, BASE_HEX, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_keep_alive_random,
	  {"Random", "icq.keep_alive.random", FT_UINT32, BASE_HEX, NULL, 0x0, NULL, HFILL }},
	{ &hf_icq_status,
	  {"Client command", "icq.status", FT_UINT32, BASE_DEC, VALS(statusCode), 0x0, NULL, HFILL }},
	{ &hf_icq_meta_user_subcmd,
	  {"Subcommand", "icq.meta_user.subcmd", FT_UINT16, BASE_DEC, VALS(serverMetaSubCmdCode), 0x0, NULL, HFILL }},
    };


	static gint *ett[] = {
		&ett_icq,
		&ett_icq_header,
		&ett_icq_body,
		&ett_icq_body_parts,
	};
	static ei_register_info ei[] = {
		{ &ei_icq_unknown_meta_subcmd, { "icq.unknown_meta_subcmd", PI_UNDECODED, PI_WARN, "Unknown meta subcmd", EXPFILL }},
		{ &ei_icq_unknown_command, { "icq.unknown_command", PI_UNDECODED, PI_WARN, "Unknown command", EXPFILL }},
	};

	expert_module_t* expert_icq;

	proto_icq = proto_register_protocol("ICQ Protocol", "ICQ", "icq");
	proto_register_field_array(proto_icq, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));
	expert_icq = expert_register_protocol(proto_icq);
	expert_register_field_array(expert_icq, ei, array_length(ei));
}

void
proto_reg_handoff_icq(void)
{
	dissector_handle_t icq_handle;

	icq_handle = new_create_dissector_handle(dissect_icq, proto_icq);
	dissector_add_uint("udp.port", UDP_PORT_ICQ, icq_handle);
}

/*
 * Editor modelines
 *
 * Local Variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * ex: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
