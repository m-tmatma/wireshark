/* paket-ethercat-frame.h
 *
 * Copyright (c) 2007 by Beckhoff Automation GmbH
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef _PACKET_ETHERCAT_FRAME_H
#define _PACKET_ETHERCAT_FRAME_H

#include <ws_diag_control.h>

/* structure for decoding the header -----------------------------------------*/
DIAG_OFF_PEDANTIC
typedef union _EtherCATFrameParser
{
   struct
   {
      uint16_t length   : 11;
      uint16_t reserved : 1;
      uint16_t protocol : 4;
   } v;
   uint16_t hdr;
} EtherCATFrameParserHDR;
DIAG_ON_PEDANTIC
typedef EtherCATFrameParserHDR *PEtherCATFrameParserHDR;

#define EtherCATFrameParserHDR_Len (int)sizeof(EtherCATFrameParserHDR)

#endif /* _PACKET_ETHERCAT_FRAME_H */
