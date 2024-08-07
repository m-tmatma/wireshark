/* packet-lpd.c
 * Routines for LPR and LPRng packet disassembly
 * Gilbert Ramirez <gram@alumni.rice.edu>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <epan/packet.h>

void proto_register_lpd(void);
void proto_reg_handoff_lpd(void);

static dissector_handle_t lpd_handle;

#define TCP_PORT_PRINTER		515

static int proto_lpd;
static int hf_lpd_response;
static int hf_lpd_request;
static int hf_lpd_client_code;
static int hf_lpd_printer_option;
static int hf_lpd_response_code;

static int ett_lpd;

enum lpr_type { request, response, unknown };

static int find_printer_string(tvbuff_t *tvb, int offset);

/* This information comes from the LPRng HOWTO, which also describes
	RFC 1179. http://www.astart.com/lprng/LPRng-HOWTO.html */
static const value_string lpd_client_code[] = {
	{ 1, "LPC: start print / jobcmd: abort" },
	{ 2, "LPR: transfer a printer job / jobcmd: receive control file" },
	{ 3, "LPQ: print short form of queue status / jobcmd: receive data file" },
	{ 4, "LPQ: print long form of queue status" },
	{ 5, "LPRM: remove jobs" },
	{ 6, "LPRng lpc: do control operation" },
	{ 7, "LPRng lpr: transfer a block format print job" },
	{ 8, "LPRng lpc: secure command transfer" },
	{ 9, "LPRng lpq: verbose status information" },
	{ 0, NULL }
};
static const value_string lpd_server_code[] = {
	{ 0, "Success: accepted, proceed" },
	{ 1, "Queue not accepting jobs" },
	{ 2, "Queue temporarily full, retry later" },
	{ 3, "Bad job format, do not retry" },
	{ 0, NULL }
};

static int
dissect_lpd(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
	proto_tree	*lpd_tree;
	proto_item	*ti, *hidden_item;
	enum lpr_type	lpr_packet_type;
	uint8_t		code;
	int		printer_len;

	col_set_str(pinfo->cinfo, COL_PROTOCOL, "LPD");
	col_clear(pinfo->cinfo, COL_INFO);

	/* rfc1179 states that all responses are 1 byte long */
	code = tvb_get_uint8(tvb, 0);
	if (tvb_reported_length(tvb) == 1) {
		lpr_packet_type = response;
	}
	else if (code <= 9) {
		lpr_packet_type = request;
	}
	else {
		lpr_packet_type = unknown;
	}

	if (lpr_packet_type == request && code !=0) {
		col_add_str(pinfo->cinfo, COL_INFO, val_to_str(code, lpd_client_code, "Unknown client code: %u"));
	}
	else if (lpr_packet_type == response) {
		col_set_str(pinfo->cinfo, COL_INFO, "LPD response");
	}
	else {
		col_set_str(pinfo->cinfo, COL_INFO, "LPD continuation");
	}

		ti = proto_tree_add_item(tree, proto_lpd, tvb, 0, -1, ENC_NA);
		lpd_tree = proto_item_add_subtree(ti, ett_lpd);

		if (lpr_packet_type == response) {
		  hidden_item = proto_tree_add_boolean(lpd_tree, hf_lpd_response,
						tvb, 0, 0, true);
		} else {
		  hidden_item = proto_tree_add_boolean(lpd_tree, hf_lpd_request,
						tvb, 0, 0, true);
		}
		proto_item_set_hidden(hidden_item);

		if (lpr_packet_type == request) {
			printer_len = find_printer_string(tvb, 1);

			if (code <= 9 && printer_len != -1) {
				proto_tree_add_uint_format(lpd_tree, hf_lpd_client_code, tvb, 0, 1, code,
					"%s", val_to_str(code, lpd_client_code, "Unknown client code: %u"));
				proto_tree_add_item(lpd_tree, hf_lpd_printer_option, tvb, 1, printer_len, ENC_ASCII);
			}
			else {
				call_data_dissector(tvb, pinfo, lpd_tree);
			}
		}
		else if (lpr_packet_type == response) {
			if (code <= 3) {
				proto_tree_add_item(lpd_tree, hf_lpd_response_code, tvb, 0, 1, ENC_BIG_ENDIAN);
			}
			else {
				call_data_dissector(tvb, pinfo, lpd_tree);
			}
		}
		else {
			call_data_dissector(tvb, pinfo, lpd_tree);
		}

	return tvb_captured_length(tvb);
}


static int
find_printer_string(tvbuff_t *tvb, int offset)
{
	int	i;

	/* try to find end of string, either '\n' or '\0' */
	i = tvb_find_uint8(tvb, offset, -1, '\0');
	if (i == -1)
		i = tvb_find_uint8(tvb, offset, -1, '\n');
	if (i == -1)
		return -1;
	return i - offset;	/* length of string */
}


void
proto_register_lpd(void)
{
	static hf_register_info hf[] = {
		{ &hf_lpd_response,
		  { "Response",           "lpd.response",
		    FT_BOOLEAN, BASE_NONE, NULL, 0x0,
		    "true if LPD response", HFILL }},

		{ &hf_lpd_request,
		  { "Request",            "lpd.request",
		    FT_BOOLEAN, BASE_NONE, NULL, 0x0,
		    "true if LPD request", HFILL }},

		{ &hf_lpd_client_code,
		  { "Client code",            "lpd.client_code",
		    FT_UINT8, BASE_DEC, VALS(lpd_client_code), 0x0,
		    NULL, HFILL }},

		{ &hf_lpd_printer_option,
		  { "Printer/options",            "lpd.printer_option",
		    FT_STRING, BASE_NONE, NULL, 0x0,
		    NULL, HFILL }},

		{ &hf_lpd_response_code,
		  { "Response",            "lpd.response_code",
		    FT_UINT8, BASE_DEC, VALS(lpd_server_code), 0x0,
		    NULL, HFILL }},
	};
	static int *ett[] = {
		&ett_lpd,
	};

	proto_lpd = proto_register_protocol("Line Printer Daemon Protocol", "LPD", "lpd");
	lpd_handle = register_dissector("lpd", dissect_lpd, proto_lpd);
	proto_register_field_array(proto_lpd, hf, array_length(hf));
	proto_register_subtree_array(ett, array_length(ett));
}

void
proto_reg_handoff_lpd(void)
{
	dissector_add_uint_with_preference("tcp.port", TCP_PORT_PRINTER, lpd_handle);
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
