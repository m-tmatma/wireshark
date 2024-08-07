/* packet-ipoib.c
 * Routines for decoding IP over InfiniBand (IPoIB) packet disassembly
 * See: https://tools.ietf.org/html/rfc4391#section-6
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/etypes.h>
#include <wiretap/wtap.h>

void proto_register_ipoib(void);
void proto_reg_handoff_ipoib(void);

static dissector_handle_t ipoib_handle;

static int proto_ipoib;
static int hf_dgid;
static int hf_daddr;
static int hf_daddr_qpn;
static int hf_grh;
static int hf_grh_ip_version;
static int hf_grh_traffic_class;
static int hf_grh_flow_label;
static int hf_grh_sqpn;
static int hf_grh_sgid;
static int hf_type;
static int hf_reserved;

static int ett_raw;
static int ett_hdr;

static dissector_handle_t arp_handle;
static dissector_handle_t ip_handle;
static dissector_handle_t ipv6_handle;

static int
dissect_ipoib(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
  proto_tree *fh_tree;
  proto_tree *fh_subtree;
  proto_item *ti;
  tvbuff_t   *next_tvb;
  uint16_t    type;
  int         grh_size = 0;

  if (pinfo->rec->rec_header.packet_header.pkt_encap == WTAP_ENCAP_IP_OVER_IB_PCAP)
    grh_size = 40;

  /* load the top pane info. This should be overwritten by
     the next protocol in the stack */
  col_set_str(pinfo->cinfo, COL_PROTOCOL, "IPoIB");
  col_set_str(pinfo->cinfo, COL_INFO, "IP over Infiniband");

  /* populate a tree in the second pane with the IPoIB header data */
  if (tree) {
    ti = proto_tree_add_item (tree, proto_ipoib, tvb, 0, grh_size + 4, ENC_NA);
    fh_tree = proto_item_add_subtree(ti, ett_raw);

    /* for PCAP data populate subtree with GRH pseudo header data */
    if (pinfo->rec->rec_header.packet_header.pkt_encap == WTAP_ENCAP_IP_OVER_IB_PCAP) {

      /* Zero means GRH is not valid (unicast). Only destination
         address is set. */
      if (tvb_get_ntohs(tvb, 0) == 0) {
        ti = proto_tree_add_item (fh_tree, hf_daddr, tvb, 20, 20, ENC_NA);
        fh_subtree = proto_item_add_subtree(ti, ett_hdr);

        proto_tree_add_item(fh_subtree, hf_daddr_qpn, tvb, 21, 3, ENC_BIG_ENDIAN);
        proto_tree_add_item(fh_subtree, hf_dgid, tvb, 24, 16, ENC_NA);
      } else {
        ti = proto_tree_add_item (fh_tree, hf_grh, tvb, 0, 40, ENC_NA);
        fh_subtree = proto_item_add_subtree(ti, ett_hdr);

        proto_tree_add_item(fh_subtree, hf_grh_ip_version, tvb, 0, 1, ENC_BIG_ENDIAN);
        proto_tree_add_item(fh_subtree, hf_grh_traffic_class, tvb, 0, 2, ENC_BIG_ENDIAN);
        proto_tree_add_item(fh_subtree, hf_grh_flow_label,tvb, 0, 4, ENC_BIG_ENDIAN);
        proto_tree_add_item(fh_subtree, hf_grh_sqpn, tvb, 5, 3, ENC_BIG_ENDIAN);
        proto_tree_add_item(fh_subtree, hf_grh_sgid, tvb, 8, 16, ENC_NA);
        proto_tree_add_item(fh_subtree, hf_dgid, tvb, 24, 16, ENC_NA);
      }
    }

    proto_tree_add_item(fh_tree, hf_type, tvb, grh_size + 0, 2, ENC_BIG_ENDIAN);
    proto_tree_add_item(fh_tree, hf_reserved, tvb, grh_size + 2, 2, ENC_BIG_ENDIAN);
  }

  next_tvb = tvb_new_subset_remaining(tvb, grh_size + 4);

  type = tvb_get_ntohs(tvb, grh_size + 0);
  switch (type) {
  case ETHERTYPE_IP:
    call_dissector(ip_handle, next_tvb, pinfo, tree);
    break;
  case ETHERTYPE_IPv6:
    call_dissector(ipv6_handle, next_tvb, pinfo, tree);
    break;
  case ETHERTYPE_ARP:
  case ETHERTYPE_REVARP:
    call_dissector(arp_handle, next_tvb, pinfo, tree);
    break;
  default:
    break;
  }
  return tvb_captured_length(tvb);
}

void
proto_register_ipoib(void)
{
  static hf_register_info hf[] = {
    { &hf_daddr,
      { "Destination address", "ipoib.daddr",
        FT_NONE, BASE_NONE, NULL, 0x0,
        NULL, HFILL}},
    { &hf_daddr_qpn,
      { "Destination QPN", "ipoib.daddr.qpn",
        FT_UINT24, BASE_HEX, NULL, 0x0,
        NULL, HFILL}},
    { &hf_dgid,
      { "Destination GID", "ipoib.dgid",
        FT_IPv6, BASE_NONE, NULL, 0x0,
        NULL, HFILL }},
    { &hf_grh,
      { "Global Route Header", "ipoib.grh",
        FT_NONE, BASE_NONE, NULL, 0x0,
        NULL, HFILL}},
    { &hf_grh_ip_version, {
       "IP Version", "ipoib.grh.ipver",
       FT_UINT8, BASE_DEC, NULL, 0xF0,
       NULL, HFILL}},
    { &hf_grh_traffic_class, {
       "Traffic Class", "ipoib.grh.tclass",
       FT_UINT16, BASE_DEC, NULL, 0x0FF0,
       NULL, HFILL}},
    { &hf_grh_flow_label, {
       "Flow Label", "ipoib.grh.flowlabel",
       FT_UINT32, BASE_DEC, NULL, 0x000FFFFF,
       NULL, HFILL}},
    { &hf_grh_sqpn,
      { "Source QPN", "ipoib.grh.sqpn",
        FT_UINT24, BASE_HEX, NULL, 0x0,
        NULL, HFILL}},
    { &hf_grh_sgid,
      { "Source GID", "ipoib.grh.sgid",
        FT_IPv6, BASE_NONE, NULL, 0x0,
        NULL, HFILL }},
    { &hf_type,
      { "Type", "ipoib.type",
        FT_UINT16, BASE_HEX, VALS(etype_vals), 0x0,
        NULL, HFILL }},
    { &hf_reserved,
      { "Reserved",  "ipoib.reserved",
        FT_UINT16, BASE_HEX, NULL, 0x0,
        NULL, HFILL }}
  };

  static int *ett[] = {
    &ett_raw,
    &ett_hdr
  };

  proto_ipoib = proto_register_protocol("IP over Infiniband", "IPoIB", "ipoib");
  proto_register_field_array(proto_ipoib, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));

  ipoib_handle = register_dissector("ipoib", dissect_ipoib, proto_ipoib);
}

void
proto_reg_handoff_ipoib(void)
{
  /*
   * Get handles for the ARP, IP and IPv6 dissectors.
   */
  arp_handle  = find_dissector_add_dependency("arp", proto_ipoib);
  ip_handle   = find_dissector_add_dependency("ip", proto_ipoib);
  ipv6_handle = find_dissector_add_dependency("ipv6", proto_ipoib);

  dissector_add_uint("wtap_encap", WTAP_ENCAP_IP_OVER_IB_SNOOP, ipoib_handle);
  dissector_add_uint("wtap_encap", WTAP_ENCAP_IP_OVER_IB_PCAP, ipoib_handle);
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
