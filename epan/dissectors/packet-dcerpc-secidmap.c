/* packet-dcerpc-secidmap.c
 *
 * Routines for dcerpc  DCE Security ID Mapper
 * Copyright 2002, Jaime Fournier <Jaime.Fournier@hush.com>
 * This information is based off the released idl files from opengroup.
 * ftp://ftp.opengroup.org/pub/dce122/dce/src/security.tar.gz security/idl/rsecidmap.idl
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

void proto_register_secidmap (void);
void proto_reg_handoff_secidmap (void);

static int proto_secidmap;
static int hf_secidmap_opnum;


static int ett_secidmap;

static e_guid_t uuid_secidmap = { 0x0d7c1e50, 0x113a, 0x11ca, { 0xb7, 0x1f, 0x08, 0x00, 0x1e, 0x01, 0xdc, 0x6c } };
static uint16_t ver_secidmap = 1;



static const dcerpc_sub_dissector secidmap_dissectors[] = {
	{ 0, "parse_name",	 NULL, NULL},
	{ 1, "gen_name",	 NULL, NULL},
	{ 2, "avoid_cn_bug",	 NULL, NULL},
	{ 3, "parse_name_cache", NULL, NULL},
	{ 4, "gen_name_cache",	 NULL, NULL},

	{ 0, NULL, NULL, NULL },
};

void
proto_register_secidmap (void)
{
	static hf_register_info hf[] = {
	  { &hf_secidmap_opnum,
	    { "Operation", "secidmap.opnum", FT_UINT16, BASE_DEC,
	      NULL, 0x0, NULL, HFILL }}
	};

	static int *ett[] = {
		&ett_secidmap,
	};
	proto_secidmap = proto_register_protocol ("DCE Security ID Mapper", "SECIDMAP", "secidmap");
	proto_register_field_array (proto_secidmap, hf, array_length (hf));
	proto_register_subtree_array (ett, array_length (ett));
}

void
proto_reg_handoff_secidmap (void)
{
	/* Register the protocol as dcerpc */
	dcerpc_init_uuid (proto_secidmap, ett_secidmap, &uuid_secidmap, ver_secidmap, secidmap_dissectors, hf_secidmap_opnum);
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
