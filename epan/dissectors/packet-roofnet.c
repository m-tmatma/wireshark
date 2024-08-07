/* packet-roofnet.c
 * Routines for roofnet dissection
 * Copyright 2006, Sebastien Tandel (sebastien@tandel.be)
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/addr_resolv.h>
#include <epan/expert.h>
#include <epan/ptvcursor.h>


/* roofnet packet type constants */
#define ROOFNET_PT_QUERY 0x01
#define ROOFNET_PT_REPLY 0x02
#define ROOFNET_PT_DATA 0x04
#define ROOFNET_PT_GATEWAY 0x08
static const value_string roofnet_pt_vals[] = {
  { ROOFNET_PT_QUERY, "Query" },
  { ROOFNET_PT_REPLY, "Reply" },
  { ROOFNET_PT_DATA, "Data" },
  { ROOFNET_PT_GATEWAY, "Gateway" },
  { 0, NULL }
};

/* roofnet flag bit masks */
#define ROOFNET_FLAG_ERROR (1<<0)
#define ROOFNET_FLAG_UPDATE (1<<1)
#define ROOFNET_FLAG_LAYER2 (1<<9)
#define ROOFNET_FLAG_RESERVED 0xFDFC
#define ROOFNET_FLAG_MASK (ROOFNET_FLAG_ERROR | ROOFNET_FLAG_UPDATE | ROOFNET_FLAG_LAYER2)

/* header length */
#define ROOFNET_HEADER_LENGTH 160
/* roofnet max length */
/* may change with time */
#define ROOFNET_MAX_LENGTH 400
/* Roofnet Link Description Length
 * which is 6 fields of 4 bytes */
#define ROOFNET_LINK_DESCRIPTION_LENGTH 6*4

/* offset constants */
#define ROOFNET_OFFSET_TYPE 1
#define ROOFNET_OFFSET_NLINKS 2
#define ROOFNET_OFFSET_DATA_LENGTH 10

/* offset relative to a link section of roofnet */
#define ROOFNET_LINK_OFFSET_SRC 0
#define ROOFNET_LINK_OFFSET_DST 20
/* roofnet link fields length */
#define ROOFNET_LINK_LEN 24

/* forward reference */
void proto_register_roofnet(void);
void proto_reg_handoff_roofnet(void);

static dissector_handle_t roofnet_handle;
static dissector_handle_t ip_handle;
static dissector_handle_t eth_withoutfcs_handle;
static int proto_roofnet;

/* hf fields for the header of roofnet */
static int hf_roofnet_version;
static int hf_roofnet_type;
static int hf_roofnet_nlinks;
static int hf_roofnet_next;
static int hf_roofnet_ttl;
static int hf_roofnet_cksum;
static int hf_roofnet_flags;
static int hf_roofnet_flags_error;
static int hf_roofnet_flags_update;
static int hf_roofnet_flags_layer2;
static int hf_roofnet_flags_reserved;
static int hf_roofnet_data_length;
static int hf_roofnet_query_dst;
static int hf_roofnet_seq;
/* static int hf_roofnet_links; */
static int hf_roofnet_link_src;
static int hf_roofnet_link_forward;
static int hf_roofnet_link_rev;
static int hf_roofnet_link_seq;
static int hf_roofnet_link_age;
static int hf_roofnet_link_dst;

static int * const flag_list[] = {
    &hf_roofnet_flags_error,
    &hf_roofnet_flags_update,
    &hf_roofnet_flags_layer2,
    &hf_roofnet_flags_reserved,
    NULL
};


static int ett_roofnet;
static int ett_roofnet_flags;
static int ett_roofnet_link;

static expert_field ei_roofnet_too_many_links;
static expert_field ei_roofnet_too_much_data;

/*
 * dissect the header of roofnet
 */
static uint16_t dissect_roofnet_header(proto_tree *tree, packet_info *pinfo, tvbuff_t *tvb, unsigned *offset)
{
  uint16_t flags;
  ptvcursor_t *cursor = ptvcursor_new(pinfo->pool, tree, tvb, *offset);

  ptvcursor_add(cursor, hf_roofnet_version, 1, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_type, 1, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_nlinks, 1, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_next, 1, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_ttl, 2, ENC_BIG_ENDIAN);
  proto_tree_add_checksum(ptvcursor_tree(cursor), ptvcursor_tvbuff(cursor), ptvcursor_current_offset(cursor),
                          hf_roofnet_cksum, -1, NULL, NULL, 0, ENC_BIG_ENDIAN, PROTO_CHECKSUM_NO_FLAGS);
  ptvcursor_advance(cursor, 2);
  flags = tvb_get_ntohs(ptvcursor_tvbuff(cursor), ptvcursor_current_offset(cursor));
  proto_tree_add_bitmask(ptvcursor_tree(cursor), ptvcursor_tvbuff(cursor), ptvcursor_current_offset(cursor),
                          hf_roofnet_flags, ett_roofnet_flags, flag_list, ENC_BIG_ENDIAN);
  ptvcursor_advance(cursor, 2);
  ptvcursor_add(cursor, hf_roofnet_data_length, 2, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_query_dst, 4, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_seq, 4, ENC_BIG_ENDIAN);

  *offset = ptvcursor_current_offset(cursor);
  ptvcursor_free(cursor);

  return flags;
}

/*
 * dissect the description of link in roofnet
 */
static void dissect_roofnet_link(proto_tree *tree, packet_info *pinfo, tvbuff_t *tvb, unsigned *offset, unsigned link)
{
  proto_tree *subtree = NULL;

  ptvcursor_t *cursor = NULL;

  uint32_t addr_src = 0;
  uint32_t addr_dst = 0;

  addr_src = tvb_get_ipv4(tvb, *offset + ROOFNET_LINK_OFFSET_SRC);
  addr_dst = tvb_get_ipv4(tvb, *offset + ROOFNET_LINK_OFFSET_DST);

  subtree = proto_tree_add_subtree_format(tree, tvb, *offset, ROOFNET_LINK_LEN,
                                          ett_roofnet_link, NULL, "link: %u, src: %s, dst: %s",
                                          link,
                                          get_hostname(addr_src),
                                          get_hostname(addr_dst));

  proto_tree_add_ipv4(subtree, hf_roofnet_link_src, tvb, *offset, 4, addr_src);
  *offset += 4;

  cursor = ptvcursor_new(pinfo->pool, subtree, tvb, *offset);

  ptvcursor_add(cursor, hf_roofnet_link_forward, 4, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_link_rev, 4, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_link_seq, 4, ENC_BIG_ENDIAN);
  ptvcursor_add(cursor, hf_roofnet_link_age, 4, ENC_BIG_ENDIAN);

  *offset = ptvcursor_current_offset(cursor);
  ptvcursor_free(cursor);

  proto_tree_add_ipv4(subtree, hf_roofnet_link_dst, tvb, *offset, 4, addr_dst);
  /* don't increment offset here because the dst of this link is the src of the next one */
}

/*
 * dissect the data in roofnet
 */
static void dissect_roofnet_data(proto_tree *tree, tvbuff_t *tvb, packet_info * pinfo, int offset, uint16_t flags)
{
  uint16_t roofnet_datalen = 0;
  uint16_t remaining_datalen = 0;

  roofnet_datalen = tvb_get_ntohs(tvb, ROOFNET_OFFSET_DATA_LENGTH);
  remaining_datalen = tvb_reported_length_remaining(tvb, offset);


  /* dissect on remaining_datalen */
   if (roofnet_datalen < remaining_datalen)
     proto_tree_add_expert_format(tree, pinfo, &ei_roofnet_too_much_data, tvb, offset, roofnet_datalen,
                                  "[More payload data (%u) than told by Roofnet (%u)]",
                                  remaining_datalen, roofnet_datalen);

  if (roofnet_datalen == 0)
    return;

  /* dissect payload */
  if (flags & ROOFNET_FLAG_LAYER2) {
    /* ethernet frame is padded with 2 bytes at the start */
    call_dissector(eth_withoutfcs_handle, tvb_new_subset_remaining(tvb, offset+2), pinfo, tree);
  } else {
    call_dissector(ip_handle, tvb_new_subset_remaining(tvb, offset), pinfo, tree);
  }

}

/*
 * entry point of the roofnet dissector
 */
static int dissect_roofnet(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
  proto_item * it;
  proto_tree * roofnet_tree;
  unsigned offset = 0;

  uint8_t roofnet_msg_type = 0;
  uint8_t roofnet_nlinks = 0;
  uint8_t nlink = 1;
  uint16_t flags;

  col_set_str(pinfo->cinfo, COL_PROTOCOL, "Roofnet");

  roofnet_msg_type = tvb_get_uint8(tvb, ROOFNET_OFFSET_TYPE);
  /* Clear out stuff in the info column */
  col_add_fstr(pinfo->cinfo, COL_INFO, "Message Type: %s",
               val_to_str(roofnet_msg_type, roofnet_pt_vals, "Unknown (%d)"));

  it = proto_tree_add_item(tree, proto_roofnet, tvb, offset, -1, ENC_NA);
  roofnet_tree = proto_item_add_subtree(it, ett_roofnet);

  flags = dissect_roofnet_header(roofnet_tree, pinfo, tvb, &offset);

  roofnet_nlinks = tvb_get_uint8(tvb, ROOFNET_OFFSET_NLINKS);
  /* Check that we do not have a malformed roofnet packet */
  if ((roofnet_nlinks*6*4)+ROOFNET_HEADER_LENGTH > ROOFNET_MAX_LENGTH) {
    expert_add_info_format(pinfo, it, &ei_roofnet_too_many_links, "Too many links (%u)", roofnet_nlinks);
    return tvb_captured_length(tvb);
  }

  for (; roofnet_nlinks > 0; roofnet_nlinks--) {
    /* Do we have enough buffer to decode the next link ? */
    if (tvb_reported_length_remaining(tvb, offset) < ROOFNET_LINK_DESCRIPTION_LENGTH)
      return offset;
    dissect_roofnet_link(roofnet_tree, pinfo, tvb, &offset, nlink++);
  }

  dissect_roofnet_data(tree, tvb, pinfo, offset+4, flags);
  return tvb_captured_length(tvb);
}

void proto_register_roofnet(void)
{
  static hf_register_info hf[] = {
    /* Roofnet Header */
    { &hf_roofnet_version,
      { "Version", "roofnet.version",
      FT_UINT8, BASE_DEC, NULL, 0x0, "Roofnet Version", HFILL }
    },

    { &hf_roofnet_type,
      { "Type", "roofnet.type",
        FT_UINT8, BASE_DEC, VALS(roofnet_pt_vals), 0x0, "Roofnet Message Type", HFILL }
    },

    { &hf_roofnet_nlinks,
      { "Number of Links", "roofnet.nlinks",
        FT_UINT8, BASE_DEC, NULL, 0x0, "Roofnet Number of Links", HFILL }
    },

    { &hf_roofnet_next,
      { "Next Link", "roofnet.next",
        FT_UINT8, BASE_DEC, NULL, 0x0, "Roofnet Next Link to Use", HFILL }
    },

    { &hf_roofnet_ttl,
      { "Time To Live", "roofnet.ttl",
        FT_UINT16, BASE_DEC, NULL, 0x0, "Roofnet Time to Live", HFILL }
    },

    { &hf_roofnet_cksum,
      { "Checksum", "roofnet.cksum",
        FT_UINT16, BASE_DEC, NULL, 0x0, "Roofnet Header Checksum", HFILL }
    },

    { &hf_roofnet_flags,
      { "Flags", "roofnet.flags",
        FT_UINT16, BASE_HEX, NULL, 0x0, "Roofnet flags", HFILL }
    },

    { &hf_roofnet_flags_error,
      { "Roofnet Error", "roofnet.flags.error",
        FT_BOOLEAN, 16, NULL, ROOFNET_FLAG_ERROR, NULL, HFILL }
    },

    { &hf_roofnet_flags_update,
      { "Roofnet Update", "roofnet.flags.update",
        FT_BOOLEAN, 16, NULL, ROOFNET_FLAG_UPDATE, NULL, HFILL }
    },

    { &hf_roofnet_flags_layer2,
      { "Roofnet Layer 2", "roofnet.flags.layer2",
        FT_BOOLEAN, 16, NULL, ROOFNET_FLAG_LAYER2, NULL, HFILL }
    },

    { &hf_roofnet_flags_reserved,
      { "Roofnet Reserved", "roofnet.flags.reserved",
        FT_BOOLEAN, 16, NULL, ROOFNET_FLAG_RESERVED, NULL, HFILL }
    },

    { &hf_roofnet_data_length,
      { "Data Length", "roofnet.datalength",
        FT_UINT16, BASE_DEC, NULL, 0x0, "Data Payload Length", HFILL }
    },

    { &hf_roofnet_query_dst,
      { "Query Dst", "roofnet.querydst",
        FT_IPv4, BASE_NONE, NULL, 0x0, "Roofnet Query Destination", HFILL }
    },

    { &hf_roofnet_seq,
      { "Seq", "roofnet.seq",
        FT_UINT32, BASE_DEC, NULL, 0x0, "Roofnet Sequential Number", HFILL }
    },

#if 0
    { &hf_roofnet_links,
      { "Links", "roofnet.links",
      FT_NONE, BASE_NONE, NULL, 0x0, NULL, HFILL }
    },
#endif

    { &hf_roofnet_link_src,
      { "Source IP", "roofnet.link.src",
      FT_IPv4, BASE_NONE, NULL, 0x0, "Roofnet Message Source", HFILL }
    },

    { &hf_roofnet_link_forward,
      { "Forward", "roofnet.link.forward",
        FT_UINT32, BASE_DEC, NULL, 0x0, NULL, HFILL }
    },

    { &hf_roofnet_link_rev,
      { "Rev", "roofnet.link.rev",
        FT_UINT32, BASE_DEC, NULL, 0x0, "Revision Number", HFILL }
    },

    { &hf_roofnet_link_seq,
      { "Seq", "roofnet.link.seq",
        FT_UINT32, BASE_DEC, NULL, 0x0, "Link Sequential Number", HFILL }
    },

    { &hf_roofnet_link_age,
      { "Age", "roofnet.link.age",
        FT_UINT32, BASE_DEC, NULL, 0x0, "Information Age", HFILL }
    },

    { &hf_roofnet_link_dst,
      { "Dst IP", "roofnet.link.dst",
        FT_IPv4, BASE_NONE, NULL, 0x0, "Roofnet Message Destination", HFILL }
    }
  };

  /* setup protocol subtree array */
  static int *ett[] = {
    &ett_roofnet,
    &ett_roofnet_flags,
    &ett_roofnet_link
  };

  static ei_register_info ei[] = {
     { &ei_roofnet_too_many_links, { "roofnet.too_many_links", PI_MALFORMED, PI_ERROR, "Too many links", EXPFILL }},
     { &ei_roofnet_too_much_data, { "roofnet.too_much_data", PI_MALFORMED, PI_ERROR, "More payload data than told by Roofnet", EXPFILL }},
  };

  expert_module_t* expert_roofnet;

  proto_roofnet = proto_register_protocol("Roofnet Protocol", "Roofnet", "roofnet");

  proto_register_field_array(proto_roofnet, hf, array_length(hf));
  proto_register_subtree_array(ett, array_length(ett));
  expert_roofnet = expert_register_protocol(proto_roofnet);
  expert_register_field_array(expert_roofnet, ei, array_length(ei));

  roofnet_handle = register_dissector("roofnet", dissect_roofnet, proto_roofnet);
}


void proto_reg_handoff_roofnet(void)
{
  /* Until now there is no other option than having an IPv4 payload (maybe
   * extended one day to IPv6 or other?) */
  ip_handle = find_dissector_add_dependency("ip", proto_roofnet);
  eth_withoutfcs_handle = find_dissector_add_dependency("eth_withoutfcs", proto_roofnet);
  /* I did not put the type numbers in the ethertypes.h as they only are
   * experimental and not official */
  dissector_add_uint("ethertype", 0x0641, roofnet_handle);
  dissector_add_uint("ethertype", 0x0643, roofnet_handle);
  dissector_add_uint("ethertype", 0x0644, roofnet_handle);
  dissector_add_uint("ethertype", 0x0645, roofnet_handle);
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
