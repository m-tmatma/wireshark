/* packet-cosine.c
 * Routines for decoding CoSine IPNOS L2 debug output
 *
 * Motonori Shindo <motonori@shin.do>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * XXX TODO:
 *    o PPoATM and PPoFR encapsulation needs more test.
 *
 */

#include "config.h"

#include <epan/packet.h>
#include <wiretap/wtap.h>

void proto_register_cosine(void);
void proto_reg_handoff_cosine(void);

static int proto_cosine;
static int hf_pro;
static int hf_off;
static int hf_pri;
static int hf_rm;
static int hf_err;
static int hf_sar;
static int hf_channel_id;

static int ett_raw;

static dissector_handle_t cosine_handle;
static dissector_handle_t eth_withoutfcs_handle;
static dissector_handle_t ppp_hdlc_handle;
static dissector_handle_t llc_handle;
static dissector_handle_t chdlc_handle;
static dissector_handle_t fr_handle;

static int
dissect_cosine(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
  proto_tree               *fh_tree;
  proto_item               *ti;
  union wtap_pseudo_header *pseudo_header = pinfo->pseudo_header;

  /* load the top pane info. This should be overwritten by
     the next protocol in the stack */
  col_set_str(pinfo->cinfo, COL_RES_DL_SRC, "N/A");
  col_set_str(pinfo->cinfo, COL_RES_DL_DST, "N/A");
  col_set_str(pinfo->cinfo, COL_PROTOCOL, "N/A");
  col_set_str(pinfo->cinfo, COL_INFO, "CoSine IPNOS L2 debug output");

  /* populate a tree in the second pane with the status of the link
     layer (ie none) */
  if(tree) {
    ti = proto_tree_add_protocol_format(tree, proto_cosine, tvb, 0, 0,
                                        "CoSine IPNOS L2 debug output (%s)",
                                        pseudo_header->cosine.if_name);
    fh_tree = proto_item_add_subtree(ti, ett_raw);
    proto_tree_add_uint(fh_tree, hf_pro, tvb, 0, 0, pseudo_header->cosine.pro);
    proto_tree_add_uint(fh_tree, hf_off, tvb, 0, 0, pseudo_header->cosine.off);
    proto_tree_add_uint(fh_tree, hf_pri, tvb, 0, 0, pseudo_header->cosine.pri);
    proto_tree_add_uint(fh_tree, hf_rm,  tvb, 0, 0, pseudo_header->cosine.rm);
    proto_tree_add_uint(fh_tree, hf_err, tvb, 0, 0, pseudo_header->cosine.err);

    switch (pseudo_header->cosine.encap) {
      case COSINE_ENCAP_ETH:
        break;
      case COSINE_ENCAP_ATM:
      case COSINE_ENCAP_PPoATM:
        proto_tree_add_item(fh_tree, hf_sar, tvb, 0, 16, ENC_NA);
        break;
      case COSINE_ENCAP_PPP:
      case COSINE_ENCAP_FR:
      case COSINE_ENCAP_PPoFR:
        proto_tree_add_item(fh_tree, hf_channel_id, tvb, 0, 4, ENC_NA);
        break;
      case COSINE_ENCAP_HDLC:
        if (pseudo_header->cosine.direction == COSINE_DIR_TX) {
          proto_tree_add_item(fh_tree, hf_channel_id, tvb, 0, 2, ENC_NA);
        } else if (pseudo_header->cosine.direction == COSINE_DIR_RX) {
          proto_tree_add_item(fh_tree, hf_channel_id, tvb, 0, 4, ENC_NA);
        }
        break;
      default:
        break;
    }
  }

  switch (pseudo_header->cosine.encap) {
    case COSINE_ENCAP_ETH:
      call_dissector(eth_withoutfcs_handle, tvb_new_subset_remaining(tvb, 0),
                     pinfo, tree);
      break;
    case COSINE_ENCAP_ATM:
    case COSINE_ENCAP_PPoATM:
      call_dissector(llc_handle, tvb_new_subset_remaining(tvb, 16),
                     pinfo, tree);
      break;
    case COSINE_ENCAP_PPP:
      call_dissector(ppp_hdlc_handle, tvb_new_subset_remaining(tvb, 4),
                     pinfo, tree);
      break;
    case COSINE_ENCAP_HDLC:
      if (pseudo_header->cosine.direction == COSINE_DIR_TX) {
        call_dissector(chdlc_handle, tvb_new_subset_remaining(tvb, 2),
                       pinfo, tree);
      } else if (pseudo_header->cosine.direction == COSINE_DIR_RX) {
        call_dissector(chdlc_handle, tvb_new_subset_remaining(tvb, 4),
                       pinfo, tree);
      }
      break;
    case COSINE_ENCAP_FR:
    case COSINE_ENCAP_PPoFR:
      call_dissector(fr_handle, tvb_new_subset_remaining(tvb, 4),
                     pinfo, tree);
      break;
    case COSINE_ENCAP_TEST:
    case COSINE_ENCAP_UNKNOWN:
      call_data_dissector(tvb, pinfo, tree);
      break;
    default:
      break;
  }
  return tvb_captured_length(tvb);
}

void
proto_register_cosine(void)
{
  static hf_register_info hf[] = {
    { &hf_pro,
      { "Protocol", "cosine.pro", FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_off,
      { "Offset", "cosine.off", FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_pri,
      { "Priority", "cosine.pri", FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_rm,
      { "Rate Marking", "cosine.rm",  FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_err,
      { "Error Code", "cosine.err", FT_UINT8, BASE_DEC, NULL, 0x0, NULL, HFILL}},
    { &hf_sar,
      { "SAR header", "cosine.sar", FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL}},
    { &hf_channel_id,
      { "Channel handle ID", "cosine.channel_id", FT_BYTES, BASE_NONE, NULL, 0x0, NULL, HFILL}},
  };

  static int *ett[] = {
    &ett_raw,
  };

  proto_cosine = proto_register_protocol("CoSine IPNOS L2 debug output",
                                         "CoSine", "cosine");
  proto_register_field_array(proto_cosine, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));

  cosine_handle = register_dissector("cosine", dissect_cosine, proto_cosine);
}

void
proto_reg_handoff_cosine(void)
{

  /*
   * Get handles for dissectors.
   */
  eth_withoutfcs_handle = find_dissector_add_dependency("eth_withoutfcs", proto_cosine);
  ppp_hdlc_handle       = find_dissector_add_dependency("ppp_hdlc", proto_cosine);
  llc_handle            = find_dissector_add_dependency("llc", proto_cosine);
  chdlc_handle          = find_dissector_add_dependency("chdlc", proto_cosine);
  fr_handle             = find_dissector_add_dependency("fr", proto_cosine);

  dissector_add_uint("wtap_encap", WTAP_ENCAP_COSINE, cosine_handle);
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
