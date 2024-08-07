/* packet-crmf.c
 * Routines for RFC2511 Certificate Request Message Format packet dissection
 *   Ronnie Sahlberg 2004
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/oids.h>
#include <epan/asn1.h>

#include "packet-ber.h"
#include "packet-crmf.h"
#include "packet-cms.h"
#include "packet-pkix1explicit.h"
#include "packet-pkix1implicit.h"

#define PNAME  "Certificate Request Message Format"
#define PSNAME "CRMF"
#define PFNAME "crmf"

void proto_register_crmf(void);
void proto_reg_handoff_crmf(void);

/* Initialize the protocol and registered fields */
static int proto_crmf;
static int hf_crmf_type_oid;
#include "packet-crmf-hf.c"

/* Initialize the subtree pointers */
#include "packet-crmf-ett.c"
#include "packet-crmf-fn.c"


/*--- proto_register_crmf ----------------------------------------------*/
void proto_register_crmf(void) {

  /* List of fields */
  static hf_register_info hf[] = {
    { &hf_crmf_type_oid,
      { "Type", "crmf.type.oid",
        FT_STRING, BASE_NONE, NULL, 0,
        "Type of AttributeTypeAndValue", HFILL }},
#include "packet-crmf-hfarr.c"
  };

  /* List of subtrees */
  static int *ett[] = {
#include "packet-crmf-ettarr.c"
  };

  /* Register protocol */
  proto_crmf = proto_register_protocol(PNAME, PSNAME, PFNAME);

  /* Register fields and subtrees */
  proto_register_field_array(proto_crmf, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));

}


/*--- proto_reg_handoff_crmf -------------------------------------------*/
void proto_reg_handoff_crmf(void) {
	oid_add_from_string("id-pkip","1.3.6.1.5.5.7.5");
	oid_add_from_string("id-regCtrl","1.3.6.1.5.5.7.5.1");
	oid_add_from_string("id-regInfo","1.3.6.1.5.5.7.5.2");
#include "packet-crmf-dis-tab.c"
}

