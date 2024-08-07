/* packet-dcerpc-conv.c
 * Routines for dcerpc conv dissection
 * Copyright 2001, Todd Sabin <tas@webspan.net>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"


#include <epan/packet.h>
#include "packet-dcerpc.h"
#include "packet-dcerpc-dce122.h"

void proto_register_conv (void);
void proto_reg_handoff_conv (void);

static int proto_conv;
static int hf_conv_opnum;
static int hf_conv_rc;
static int hf_conv_who_are_you_rqst_actuid;
static int hf_conv_who_are_you_rqst_boot_time;
static int hf_conv_who_are_you2_rqst_actuid;
static int hf_conv_who_are_you2_rqst_boot_time;
static int hf_conv_who_are_you_resp_seq;
static int hf_conv_who_are_you2_resp_seq;
static int hf_conv_who_are_you2_resp_casuuid;

static int ett_conv;


static e_guid_t uuid_conv = { 0x333a2276, 0x0000, 0x0000, { 0x0d, 0x00, 0x00, 0x80, 0x9c, 0x00, 0x00, 0x00 } };
static uint16_t ver_conv = 3;


static int
conv_dissect_who_are_you_rqst (tvbuff_t *tvb, int offset,
			       packet_info *pinfo, proto_tree *tree,
			       dcerpc_info *di, uint8_t *drep)
{
	/*
	 *         [in]    uuid_t          *actuid,
	 *         [in]    unsigned32      boot_time,
	 */
	e_guid_t actuid;

	offset = dissect_ndr_uuid_t(tvb, offset, pinfo, tree, di, drep, hf_conv_who_are_you_rqst_actuid, &actuid);
	offset = dissect_ndr_time_t(tvb, offset, pinfo, tree, di, drep, hf_conv_who_are_you_rqst_boot_time, NULL);

	col_add_fstr(pinfo->cinfo, COL_INFO,
			     "conv_who_are_you request actuid: %08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			     actuid.data1, actuid.data2, actuid.data3,
			     actuid.data4[0], actuid.data4[1], actuid.data4[2], actuid.data4[3],
			     actuid.data4[4], actuid.data4[5], actuid.data4[6], actuid.data4[7]);

	return offset;
}

static int
conv_dissect_who_are_you_resp (tvbuff_t *tvb, int offset,
			       packet_info *pinfo, proto_tree *tree,
			       dcerpc_info *di, uint8_t *drep)
{
	/*
	 *         [out]   unsigned32      *seq,
	 *         [out]   unsigned32      *st
	 */
	uint32_t seq, st;

	offset = dissect_ndr_uint32 (tvb, offset, pinfo, tree, di, drep, hf_conv_who_are_you_resp_seq, &seq);
	offset = dissect_ndr_uint32 (tvb, offset, pinfo, tree, di, drep, hf_conv_rc, &st);


	col_add_fstr(pinfo->cinfo, COL_INFO, "conv_who_are_you response seq:%u st:%s",
			     seq, val_to_str_ext(st, &dce_error_vals_ext, "%u"));

	return offset;
}



static int
conv_dissect_who_are_you2_rqst (tvbuff_t *tvb, int offset,
				packet_info *pinfo, proto_tree *tree,
				dcerpc_info *di, uint8_t *drep)
{
	/*
	 *         [in]    uuid_t          *actuid,
	 *         [in]    unsigned32      boot_time,
	 */
	e_guid_t actuid;

	offset = dissect_ndr_uuid_t(tvb, offset, pinfo, tree, di, drep, hf_conv_who_are_you2_rqst_actuid, &actuid);
	offset = dissect_ndr_time_t(tvb, offset, pinfo, tree, di, drep, hf_conv_who_are_you2_rqst_boot_time, NULL);

		col_add_fstr(pinfo->cinfo, COL_INFO,
			     "conv_who_are_you2 request actuid: %08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			     actuid.data1, actuid.data2, actuid.data3,
			     actuid.data4[0], actuid.data4[1], actuid.data4[2], actuid.data4[3],
			     actuid.data4[4], actuid.data4[5], actuid.data4[6], actuid.data4[7]);

	return offset;
}
static int
conv_dissect_who_are_you2_resp (tvbuff_t *tvb, int offset,
				packet_info *pinfo, proto_tree *tree,
				dcerpc_info *di, uint8_t *drep)
{
	/*
	 *         [out]   unsigned32      *seq,
	 *         [out]   uuid_t          *cas_uuid,
	 *
	 *         [out]   unsigned32      *st
	 */
	uint32_t seq, st;
	e_guid_t cas_uuid;

	offset = dissect_ndr_uint32 (tvb, offset, pinfo, tree, di, drep, hf_conv_who_are_you2_resp_seq, &seq);
	offset = dissect_ndr_uuid_t (tvb, offset, pinfo, tree, di, drep, hf_conv_who_are_you2_resp_casuuid, &cas_uuid);
	offset = dissect_ndr_uint32 (tvb, offset, pinfo, tree, di, drep, hf_conv_rc, &st);

	col_add_fstr(pinfo->cinfo, COL_INFO,
			     "conv_who_are_you2 response seq:%u st:%s cas:%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			     seq, val_to_str_ext(st, &dce_error_vals_ext, "%u"),
			     cas_uuid.data1, cas_uuid.data2, cas_uuid.data3,
			     cas_uuid.data4[0], cas_uuid.data4[1], cas_uuid.data4[2], cas_uuid.data4[3],
			     cas_uuid.data4[4], cas_uuid.data4[5], cas_uuid.data4[6], cas_uuid.data4[7]);

	return offset;
}


static const dcerpc_sub_dissector conv_dissectors[] = {
	{ 0, "who_are_you",
	  conv_dissect_who_are_you_rqst, conv_dissect_who_are_you_resp },
	{ 1, "who_are_you2",
	  conv_dissect_who_are_you2_rqst, conv_dissect_who_are_you2_resp },
	{ 2, "are_you_there",
	  NULL, NULL },
	{ 3, "who_are_you_auth",
	  NULL, NULL },
	{ 4, "who_are_you_auth_more",
	  NULL, NULL },
	{ 0, NULL, NULL, NULL }
};

void
proto_register_conv (void)
{
	static hf_register_info hf[] = {
	{ &hf_conv_opnum,
	    { "Operation", "conv.opnum", FT_UINT16, BASE_DEC, NULL, 0x0, NULL, HFILL }},
	{ &hf_conv_rc,
	    {"Status", "conv.status", FT_UINT32, BASE_DEC|BASE_EXT_STRING, &dce_error_vals_ext, 0x0, NULL, HFILL }},

	{ &hf_conv_who_are_you_rqst_actuid,
	    {"Activity UID", "conv.who_are_you_rqst_actuid", FT_GUID, BASE_NONE, NULL, 0x0, "UUID", HFILL }},
	{ &hf_conv_who_are_you_rqst_boot_time,
	    {"Boot time", "conv.who_are_you_rqst_boot_time", FT_ABSOLUTE_TIME, ABSOLUTE_TIME_LOCAL, NULL, 0x0, NULL, HFILL }},
	{ &hf_conv_who_are_you2_rqst_actuid,
	    {"Activity UID", "conv.who_are_you2_rqst_actuid", FT_GUID, BASE_NONE, NULL, 0x0, "UUID", HFILL }},
	{ &hf_conv_who_are_you2_rqst_boot_time,
	    {"Boot time", "conv.who_are_you2_rqst_boot_time", FT_ABSOLUTE_TIME, ABSOLUTE_TIME_LOCAL, NULL, 0x0, NULL, HFILL }},

	{ &hf_conv_who_are_you_resp_seq,
	    {"Sequence Number", "conv.who_are_you_resp_seq", FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }},
	{ &hf_conv_who_are_you2_resp_seq,
	    {"Sequence Number", "conv.who_are_you2_resp_seq", FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }},
	{ &hf_conv_who_are_you2_resp_casuuid,
	    {"Client's address space UUID", "conv.who_are_you2_resp_casuuid", FT_GUID, BASE_NONE, NULL, 0x0, NULL, HFILL }}
	};

	static int *ett[] = {
		&ett_conv
	};
	proto_conv = proto_register_protocol ("DCE/RPC Conversation Manager", "CONV", "conv");
	proto_register_field_array (proto_conv, hf, array_length (hf));
	proto_register_subtree_array (ett, array_length (ett));
}

void
proto_reg_handoff_conv (void)
{
	/* Register the protocol as dcerpc */
	dcerpc_init_uuid (proto_conv, ett_conv, &uuid_conv, ver_conv, conv_dissectors, hf_conv_opnum);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 8
 * tab-width: 8
 * indent-tabs-mode: t
 * End:
 *
 * vi: set shiftwidth=8 tabstop=8 noexpandtab:
 * :indentSize=8:tabSize=8:noTabs=false:
 */
