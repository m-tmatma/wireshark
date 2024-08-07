/* packet-lapbether.c
 * Routines for lapbether frame disassembly
 * Richard Sharpe <rsharpe@ns.aus.com> based on the lapb module by
 * Olivier Abad <oabad@noos.fr>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/etypes.h>

void proto_register_lapbether(void);
void proto_reg_handoff_lapbether(void);

static int proto_lapbether;

static int hf_lapbether_length;

static int ett_lapbether;

static dissector_handle_t lapbether_handle;
static dissector_handle_t lapb_handle;

static int
dissect_lapbether(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    proto_tree *lapbether_tree, *ti;
    int         len;
    tvbuff_t   *next_tvb;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "LAPBETHER");
    col_clear(pinfo->cinfo, COL_INFO);

    len = tvb_get_uint8(tvb, 0) + tvb_get_uint8(tvb, 1) * 256;

    if (tree) {

      ti = proto_tree_add_protocol_format(tree, proto_lapbether, tvb, 0, 2,
                                          "LAPBETHER");

      lapbether_tree = proto_item_add_subtree(ti, ett_lapbether);
      proto_tree_add_uint_format(lapbether_tree, hf_lapbether_length, tvb, 0, 2,
                                 len, "Length: %u", len);

    }

    next_tvb = tvb_new_subset_length(tvb, 2, len);
    call_dissector(lapb_handle, next_tvb, pinfo, tree);

    return tvb_captured_length(tvb);
}

void
proto_register_lapbether(void)
{
    static hf_register_info hf[] = {
      { &hf_lapbether_length,
        { "Length Field", "lapbether.length", FT_UINT16, BASE_DEC, NULL, 0x0,
          "LAPBEther Length Field", HFILL }},

    };
    static int *ett[] = {
        &ett_lapbether,
    };

    proto_lapbether = proto_register_protocol ("Link Access Procedure Balanced Ethernet (LAPBETHER)",
        "LAPBETHER", "lapbether");
    proto_register_field_array (proto_lapbether, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

  lapbether_handle = register_dissector("lapbether", dissect_lapbether, proto_lapbether);
}

/* The registration hand-off routine */
void
proto_reg_handoff_lapbether(void)
{
  /*
   * Get a handle for the LAPB dissector.
   */
  lapb_handle = find_dissector_add_dependency("lapb", proto_lapbether);

  dissector_add_uint("ethertype", ETHERTYPE_DEC, lapbether_handle);

}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local Variables:
 * c-basic-offset: 2
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=2 tabstop=8 expandtab:
 * :indentSize=2:tabSize=8:noTabs=true:
 */
