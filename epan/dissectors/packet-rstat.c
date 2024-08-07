/* packet-rstat.c
 * Stubs for Sun's remote statistics RPC service
 *
 * Guy Harris <guy@alum.mit.edu>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include "packet-rpc.h"

void proto_register_rstat(void);
void proto_reg_handoff_rstat(void);

static int proto_rstat;
static int hf_rstat_procedure_v1;
static int hf_rstat_procedure_v2;
static int hf_rstat_procedure_v3;
static int hf_rstat_procedure_v4;

static int ett_rstat;

#define RSTAT_PROGRAM	100001

#define RSTATPROC_NULL		0
#define RSTATPROC_STATS		1
#define RSTATPROC_HAVEDISK	2

/* proc number, "proc name", dissect_request, dissect_reply */
static const vsff rstat1_proc[] = {
	{ RSTATPROC_NULL,	"NULL",
		dissect_rpc_void,	dissect_rpc_void },
	{ RSTATPROC_STATS,	"STATS",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ 0,	NULL,	NULL,	NULL }
};
static const value_string rstat1_proc_vals[] = {
	{ RSTATPROC_NULL,	"NULL" },
	{ RSTATPROC_STATS,	"STATS" },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK" },
	{ 0,	NULL }
};

static const vsff rstat2_proc[] = {
	{ RSTATPROC_NULL,	"NULL",
		dissect_rpc_void,	dissect_rpc_void },
	{ RSTATPROC_STATS,	"STATS",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ 0,	NULL,	NULL,	NULL }
};
static const value_string rstat2_proc_vals[] = {
	{ RSTATPROC_NULL,	"NULL" },
	{ RSTATPROC_STATS,	"STATS" },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK" },
	{ 0,	NULL }
};

static const vsff rstat3_proc[] = {
	{ RSTATPROC_NULL,	"NULL",
		dissect_rpc_void,	dissect_rpc_void },
	{ RSTATPROC_STATS,	"STATS",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ 0,	NULL,	NULL,	NULL }
};
static const value_string rstat3_proc_vals[] = {
	{ RSTATPROC_NULL,	"NULL" },
	{ RSTATPROC_STATS,	"STATS" },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK" },
	{ 0,	NULL }
};

static const vsff rstat4_proc[] = {
	{ RSTATPROC_NULL,	"NULL",
		dissect_rpc_void,	dissect_rpc_void },
	{ RSTATPROC_STATS,	"STATS",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK",
		dissect_rpc_unknown,	dissect_rpc_unknown },
	{ 0,	NULL,	NULL,	NULL }
};
static const value_string rstat4_proc_vals[] = {
	{ RSTATPROC_NULL,	"NULL" },
	{ RSTATPROC_STATS,	"STATS" },
	{ RSTATPROC_HAVEDISK,	"HAVEDISK" },
	{ 0,	NULL }
};

static const rpc_prog_vers_info rstat_vers_info[] = {
	{ 1, rstat1_proc, &hf_rstat_procedure_v1 },
	{ 2, rstat2_proc, &hf_rstat_procedure_v2 },
	{ 3, rstat3_proc, &hf_rstat_procedure_v3 },
	{ 4, rstat4_proc, &hf_rstat_procedure_v4 },
};

void
proto_register_rstat(void)
{
	static hf_register_info hf[] = {
		{ &hf_rstat_procedure_v1, {
			"V1 Procedure", "rstat.procedure_v1", FT_UINT32, BASE_DEC,
			VALS(rstat1_proc_vals), 0, NULL, HFILL }},
		{ &hf_rstat_procedure_v2, {
			"V2 Procedure", "rstat.procedure_v2", FT_UINT32, BASE_DEC,
			VALS(rstat2_proc_vals), 0, NULL, HFILL }},
		{ &hf_rstat_procedure_v3, {
			"V3 Procedure", "rstat.procedure_v3", FT_UINT32, BASE_DEC,
			VALS(rstat3_proc_vals), 0, NULL, HFILL }},
		{ &hf_rstat_procedure_v4, {
			"V4 Procedure", "rstat.procedure_v4", FT_UINT32, BASE_DEC,
			VALS(rstat4_proc_vals), 0, NULL, HFILL }}
	};

	static int *ett[] = {
		&ett_rstat,
	};

	proto_rstat = proto_register_protocol("RSTAT", "RSTAT", "rstat");
	proto_register_field_array(proto_rstat, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));
}

void
proto_reg_handoff_rstat(void)
{
	/* Register the protocol as RPC */
	rpc_init_prog(proto_rstat, RSTAT_PROGRAM, ett_rstat,
	    G_N_ELEMENTS(rstat_vers_info), rstat_vers_info);
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
