/* packet-wap.h
 *
 * Declarations for WAP packet disassembly
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * WAP dissector based on original work by Ben Fowler
 * Updated by Neil Hunter <neil.hunter@energis-squared.com>
 * WTLS support by Alexandre P. Ferreira (Splice IP)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __PACKET_WAP_H__
#define __PACKET_WAP_H__

#include <epan/packet.h>
#include <epan/expert.h>

/* Port Numbers as per IANA */
/* < URL:http://www.iana.org/assignments/port-numbers/ > */
#define UDP_PORT_WSP			9200		/* wap-wsp			*/
#define UDP_PORT_WSP_RANGE		"2948,9200"		/* wap-wsp			*/
#define UDP_PORT_WTP_WSP		9201		/* wap-wsp-wtp		*/
#define UDP_PORT_WTLS_WSP		9202		/* wap-wsp-s		*/
#define UDP_PORT_WTLS_WTP_WSP		9203		/* wap-wsp-wtp-s	*/
#define UDP_PORT_WSP_PUSH		2948		/* wap-wsp		*/
#define UDP_PORT_WTLS_WSP_PUSH		2949		/* wap-wsp-s		*/
#define UDP_PORT_WTLS_RANGE		"2949,9202-9203"	/* wap-wsp			*/

/*
 * Note:
 *   There are four dissectors for the WAP protocol:
 *     WTLS
 *     WTP
 *     WSP
 *     WMLC
 *   Which of these are necessary is determined by the port number above.
 *   I.e. port 9200 (wap-wsp) indicates WSP data and possibly WMLC (depending on
 *   the WSP PDU).
 *   Port 9203 (wap-wsp-wtp-s), on the other hand, has WTLS, WTP, WSP and
 *   possibly WMLC data in that order in the packet.
 *
 *   Therefore the dissectors are chained as follows:
 *
 *   Port        Dissectors
 *   9200                     WSP  ->  WMLC
 *   9201            WTP  ->  WSP  ->  WMLC
 *   9202  WTLS  ->           WSP  ->  WMLC
 *   9203  WTLS  ->  WTP  ->  WSP  ->  WMLC
 *
 *   2948                     WSP  ->  WMLC (Push)
 *   2949  WTLS  ->           WSP  ->  WMLC (Push)
 *
 *   At present, only the unencrypted parts of WTLS can be analysed. Therefore
 *   the WTP and WSP dissectors are not called.
 */

/* Utility function for reading Uintvar encoded values */
unsigned tvb_get_uintvar (tvbuff_t *, unsigned , unsigned *, packet_info *, expert_field *);

/*
 * Misc TODO:
 *
 * WMLC Dissector
 * Check Protocol display
 * Check Protocol information display
 * Check CONNECT/CONNECT REPLY headers
 * Check add_headers code
 * Check Content-Length code
 *
 */

#endif /* packet-wap.h */
