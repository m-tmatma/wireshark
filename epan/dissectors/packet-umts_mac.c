/* packet-umts_mac.c
 * Routines for UMTS MAC (3GPP TS 25.321) disassembly
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/conversation.h>
#include <epan/expert.h>
#include <epan/prefs.h>
#include <epan/proto_data.h>

#include "packet-rrc.h"
#include "packet-umts_fp.h"
#include "packet-umts_mac.h"
#include "packet-nbap.h"

void proto_register_umts_mac(void);
void proto_reg_handoff_umts_mac(void);

int proto_umts_mac;
extern int proto_fp;
extern int proto_umts_rlc;

/* dissector fields */
static int hf_mac_fach_fdd_tctf;
static int hf_mac_rach_fdd_tctf;
static int hf_mac_ct;
static int hf_mac_ueid_type;
static int hf_mac_crnti;
static int hf_mac_urnti;
static int hf_mac_resolved_urnti;
static int hf_mac_crnti_urnti_match_frame;
static int hf_mac_channel;
/* static int hf_mac_channel_str; */

static int hf_mac_lch_id;
static int hf_mac_macdflowd_id;
/* static int hf_mac_channel_hsdsch; */
static int hf_mac_trch_id;

/* static int hf_mac_edch_type2_subframe_header; */
/* static int hf_mac_edch_type2_descriptors; */
/* static int hf_mac_edch_type2_lchid; */
/* static int hf_mac_edch_type2_length; */
/* static int hf_mac_edch_type2_flag; */
static int hf_mac_edch_type2_tsn;
static int hf_mac_edch_type2_ss;
static int hf_mac_edch_type2_ss_interpretation;
static int hf_mac_edch_type2_sdu;
static int hf_mac_edch_type2_sdu_data;
static int hf_mac_is_fraglink;
static int hf_mac_is_reasmin;

/* subtrees */
static int ett_mac;
static int ett_mac_fach;
static int ett_mac_rach;
static int ett_mac_dch;
static int ett_mac_pch;
static int ett_mac_edch;
static int ett_mac_hsdsch;
static int ett_mac_edch_type2;
static int ett_mac_edch_type2_sdu;
static int ett_mac_resolved_urnti;

static expert_field ei_mac_cs_dtch_not_implemented;
static expert_field ei_mac_rach_tctf_unknown;
static expert_field ei_mac_unknown_content;
static expert_field ei_mac_per_frame_info_missing;
static expert_field ei_mac_fach_content_type_unknown;
static expert_field ei_mac_no_logical_channel;
static expert_field ei_mac_faked_logical_channel_id;
static expert_field ei_mac_macis_sdu_reassembled;
static expert_field ei_mac_macis_sdu_first;
static expert_field ei_mac_macis_sdu_middle;
static expert_field ei_mac_macis_sdu_last;
static expert_field ei_mac_macis_sdu_complete;
static expert_field ei_mac_reserved_c_t;

static dissector_handle_t rlc_pcch_handle;
static dissector_handle_t rlc_ccch_handle;
static dissector_handle_t rlc_ctch_handle;
static dissector_handle_t rlc_dcch_handle;
static dissector_handle_t rlc_ps_dtch_handle;
static dissector_handle_t rrc_handle;

/* MAC-is reassembly */
static size_t MAX_TSN = 64;
static uint16_t mac_tsn_size = 6;
static int global_mac_tsn_size = MAC_TSN_6BITS;
int get_mac_tsn_size(void) { return global_mac_tsn_size; }
static const enum_val_t tsn_size_enumvals[] = {
    {"6",  "6 bits",  MAC_TSN_6BITS},
    {"14", "14 bits", MAC_TSN_14BITS},
    {NULL, NULL, -1}};
enum mac_is_fragment_type {
    MAC_IS_HEAD,
    MAC_IS_MIDDLE,
    MAC_IS_TAIL
};
typedef struct _mac_is_fragment {
    uint8_t * data;
    uint32_t length;
    uint32_t frame_num;
    uint16_t tsn;
    uint8_t type;
    struct _mac_is_fragment * next;
} mac_is_fragment;
typedef struct {
    uint32_t frame_num; /* Where reassembly was done (depends on order of arrival). */
    uint16_t tsn; /* TSN for the tail fragment. */
    uint8_t * data;
    uint32_t length;
    mac_is_fragment * fragments;
} mac_is_sdu;
typedef struct {
    mac_is_fragment * head;
    mac_is_fragment * middle;
    mac_is_fragment * tail;
} body_parts;
typedef struct {
    uint8_t lchid; /* Logical Channel Identifier. */
    unsigned ueid; /* User Equipment Identifier. */
} mac_is_channel;
static GHashTable * mac_is_sdus; /* channel -> (frag -> sdu) */
static GHashTable * mac_is_fragments; /* channel -> body_parts[] */
static gboolean mac_is_channel_equal(const void *a, const void *b)
{
    const mac_is_channel *x = (const mac_is_channel *)a, *y = (const mac_is_channel *)b;
    return x->lchid == y->lchid && x->ueid == y->ueid;
}
static unsigned mac_is_channel_hash(const void *key)
{
    const mac_is_channel * ch = (const mac_is_channel *)key;
    return (ch->ueid << 4)  | ch->lchid;
}

static gboolean mac_is_fragment_equal(const void *a, const void *b)
{
    const mac_is_fragment *x = (const mac_is_fragment *)a, *y = (const mac_is_fragment *)b;
    return x->frame_num == y->frame_num && x->tsn == y->tsn && x->type == y->type;
}
static unsigned mac_is_fragment_hash(const void *key)
{
    const mac_is_fragment *frag = (const mac_is_fragment *)key;
    return (frag->frame_num << 2) | frag->type;
}

static const value_string rach_fdd_tctf_vals[] = {
    { TCTF_CCCH_RACH_FDD      , "CCCH over RACH (FDD)" },
    { TCTF_DCCH_DTCH_RACH_FDD , "DCCH/DTCH over RACH (FDD)" },
    { 0, NULL }};

static const value_string fach_fdd_tctf_vals[] = {
    { TCTF_BCCH_FACH_FDD      , "BCCH over FACH (FDD)" },
    { TCTF_DCCH_DTCH_FACH_FDD , "DCCH/DTCH over FACH (FDD)" },
    { TCTF_MTCH_FACH_FDD      , "MTCH over FACH (FDD)" },
    { TCTF_CCCH_FACH_FDD      , "CCCH over FACH (FDD)" },
    { TCTF_MCCH_FACH_FDD      , "MCCH over FACH (FDD)" },
    { TCTF_MSCH_FACH_FDD      , "MSCH over FACH (FDD)" },
    { TCTF_CTCH_FACH_FDD      , "CTCH over FACH (FDD)" },
    { 0, NULL }};

static const value_string ueid_type_vals[] = {
    { MAC_UEID_TYPE_URNTI,  "U-RNTI" },
    { MAC_UEID_TYPE_CRNTI,  "C-RNTI" },
    { 0, NULL }};

static const value_string mac_logical_channel_vals[] = {
    { MAC_PCCH, "PCCH" },
    { MAC_CCCH, "CCCH" },
    { MAC_CTCH, "CTCH" },
    { MAC_DCCH, "DCCH" },
    { MAC_DTCH, "DTCH" },
    { MAC_BCCH, "BCCH" },
    { MAC_MCCH, "MCCH" },
    { MAC_MSCH, "MSCH" },
    { MAC_MTCH, "MTCH" },
    { MAC_N_A, "N/A" },
    { 0, NULL }};

static uint8_t fach_fdd_tctf(uint8_t hdr, uint16_t *bit_offs)
{
    uint8_t tctf;
    /* first, test for valid 2-bit combinations */
    tctf = hdr >> 6;
    switch (tctf) {
        case TCTF_BCCH_FACH_FDD:
        case TCTF_DCCH_DTCH_FACH_FDD:
            *bit_offs = 2;
            return tctf;
    }
    /* 4-bit combinations */
    tctf = hdr >> 4;
    switch (tctf) {
        case TCTF_MTCH_FACH_FDD:
            *bit_offs = 4;
            return tctf;
    }
    /* just return the 8-bit combination */
    *bit_offs = 8;
    tctf = hdr;
    switch (tctf) {
        case TCTF_CCCH_FACH_FDD:
        case TCTF_MCCH_FACH_FDD:
        case TCTF_MSCH_FACH_FDD:
        case TCTF_CTCH_FACH_FDD:
            return tctf;
        default:
            return tctf; /* TODO */
    }
}

static uint16_t tree_add_common_dcch_dtch_fields(tvbuff_t *tvb, packet_info *pinfo _U_,
    proto_tree *tree, uint16_t bitoffs, fp_info *fpinf, umts_mac_info *macinf, rlc_info  *rlcinf)
{
    uint8_t ueid_type;
    conversation_t   *p_conv;
    umts_fp_conversation_info_t *umts_fp_conversation_info = NULL;
    fp_rach_channel_info_t *fp_rach_channel_info = NULL;
    fp_fach_channel_info_t *fp_fach_channel_info = NULL;
    wmem_tree_t* channel_rnti_map = NULL;
    uint16_t c_rnti;
    fp_crnti_allocation_info_t *fp_crnti_allocation_info = NULL;

    ueid_type = tvb_get_bits8(tvb, bitoffs, 2);
    proto_tree_add_bits_item(tree, hf_mac_ueid_type, tvb, bitoffs, 2, ENC_BIG_ENDIAN);
    bitoffs += 2;
    if (ueid_type == MAC_UEID_TYPE_URNTI) {
        proto_tree_add_bits_item(tree, hf_mac_urnti, tvb, bitoffs, 32, ENC_BIG_ENDIAN);
        rlcinf->ueid[fpinf->cur_tb] = tvb_get_bits32(tvb, bitoffs, 32,ENC_BIG_ENDIAN);
        bitoffs += 32;
    } else if (ueid_type == MAC_UEID_TYPE_CRNTI) {
        proto_tree_add_bits_item(tree, hf_mac_crnti, tvb, 4, 16, ENC_BIG_ENDIAN);
        c_rnti = tvb_get_bits16(tvb, bitoffs, 16,ENC_BIG_ENDIAN);
        p_conv = (conversation_t *)find_conversation(pinfo->num, &pinfo->net_dst, &pinfo->net_src,
                        conversation_pt_to_conversation_type(pinfo->ptype),
                        pinfo->destport, pinfo->srcport, NO_ADDR_B);
        if (p_conv != NULL) {
            umts_fp_conversation_info = (umts_fp_conversation_info_t *)conversation_get_proto_data(p_conv, proto_fp);
        }
        /* Trying to resolve the U-RNTI for this C-RNTI based on the channel type*/
        switch(fpinf->channel){
            case CHANNEL_RACH_FDD:
                /* In RACH: Get the channel's RNTIs map */
                if (umts_fp_conversation_info) {
                    fp_rach_channel_info = (fp_rach_channel_info_t *)umts_fp_conversation_info->channel_specific_info;
                    if(fp_rach_channel_info) {
                        channel_rnti_map = fp_rach_channel_info->crnti_to_urnti_map;
                    }
                }
                break;
            case CHANNEL_FACH_FDD:
                /* In FACH: Get the channel's RNTIs map */
                if (umts_fp_conversation_info) {
                    fp_fach_channel_info = (fp_fach_channel_info_t *)umts_fp_conversation_info->channel_specific_info;
                    if(fp_fach_channel_info) {
                        channel_rnti_map = fp_fach_channel_info->crnti_to_urnti_map;
                    }
                }
                break;
        }
        if(channel_rnti_map) {
            fp_crnti_allocation_info = (fp_crnti_allocation_info_t *)wmem_tree_lookup32(channel_rnti_map, c_rnti);
        }
        /* If not found in the RACH/FACH channel's map, Look in the global RNTIs map */
        if(fp_crnti_allocation_info == NULL) {
            fp_crnti_allocation_info = (fp_crnti_allocation_info_t *)wmem_tree_lookup32(rrc_global_urnti_crnti_map, c_rnti);
            if(fp_crnti_allocation_info != NULL) {
                /* If found in the global map, check how many times it was retrieved (including this one) */
                fp_crnti_allocation_info->global_retrieval_count++;
                /* If seen 2 times (RACH + fast FACH) remove from global map */
                if(fp_crnti_allocation_info->global_retrieval_count == 2) {
                    wmem_tree_remove32(rrc_global_urnti_crnti_map, c_rnti);
                }
                /* Also add to this channel's map for later retrieval */
                if(channel_rnti_map) {
                    wmem_tree_insert32(channel_rnti_map, c_rnti, (void *)fp_crnti_allocation_info);
                }
            }
        }
        /* Choosing between resolved U-RNTI (if found) or the C-RNTI as UE-ID for RLC */
        if(fp_crnti_allocation_info != NULL) {
            /* Using U-RNTI */
            rlcinf->ueid[fpinf->cur_tb] = fp_crnti_allocation_info->urnti;
            /* Adding 'Resolved U-RNTI' related tree items*/
            proto_item *temp;
            proto_tree *resolved_urnti_tree;
            temp = proto_tree_add_uint(tree, hf_mac_resolved_urnti, tvb, 0, 0, fp_crnti_allocation_info->urnti);
            proto_item_set_generated(temp);
            resolved_urnti_tree = proto_item_add_subtree(temp, ett_mac_resolved_urnti);
            temp = proto_tree_add_uint(resolved_urnti_tree , hf_mac_crnti_urnti_match_frame, tvb, 0, 0, fp_crnti_allocation_info->alloc_frame_number);
            proto_item_set_generated(temp);
        }
        else {
            /* Using C-RNTI */
            rlcinf->ueid[fpinf->cur_tb] = c_rnti;
        }
        bitoffs += 16;
    }

    if (macinf->ctmux[fpinf->cur_tb]) {
        proto_item * temp;
        if(rlcinf){
            rlcinf->rbid[fpinf->cur_tb] = tvb_get_bits8(tvb, bitoffs, 4)+1;
        }
        proto_tree_add_bits_item(tree, hf_mac_ct, tvb, bitoffs, 4, ENC_BIG_ENDIAN);
        bitoffs += 4;
        if(rlcinf){
            temp = proto_tree_add_uint(tree, hf_mac_lch_id, tvb, 0, 0, rlcinf->rbid[fpinf->cur_tb]);
            proto_item_set_generated(temp);
        }
    }
    return bitoffs;
}

static int dissect_mac_fdd_pch(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    proto_tree *pch_tree = NULL;
    proto_item *channel_type;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MAC");
    col_set_str(pinfo->cinfo, COL_INFO, "PCCH");

    if (tree) {
        proto_item *ti;
        ti = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
        pch_tree = proto_item_add_subtree(ti, ett_mac_pch);
        proto_item_append_text(ti, " (PCCH)");
        channel_type = proto_tree_add_uint(pch_tree, hf_mac_channel, tvb, 0, 0, MAC_PCCH);
        proto_item_set_generated(channel_type);
    }
    call_dissector_with_data(rlc_pcch_handle, tvb, pinfo, tree, data);
    return tvb_captured_length(tvb);
}

static int dissect_mac_fdd_rach(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    uint8_t        tctf;
    uint8_t        chan;
    uint16_t       bitoffs   = 0;
    tvbuff_t      *next_tvb;
    proto_tree    *rach_tree = NULL;
    proto_item    *channel_type;
    umts_mac_info *macinf;
    fp_info       *fpinf;
    rlc_info      *rlcinf;
    proto_item    *ti        = NULL;
    uint8_t        c_t;
    /* RACH TCTF is always 2 bit */
    tctf = tvb_get_bits8(tvb, 0, 2);
    bitoffs += 2;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MAC");

    col_set_str(pinfo->cinfo, COL_INFO,
        val_to_str_const(tctf, rach_fdd_tctf_vals, "Unknown TCTF"));

    ti = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
    rach_tree = proto_item_add_subtree(ti, ett_mac_rach);

    macinf = (umts_mac_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_mac, 0);
    fpinf  = (fp_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_fp, 0);
    rlcinf = (rlc_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_rlc, 0);
    if (!macinf || !fpinf) {
        proto_tree_add_expert(rach_tree, pinfo, &ei_mac_per_frame_info_missing, tvb, 0, -1);
        return 1;
    }

    proto_tree_add_bits_item(rach_tree, hf_mac_rach_fdd_tctf, tvb, 0, 2, ENC_BIG_ENDIAN);
    if (tctf == TCTF_DCCH_DTCH_RACH_FDD) {
        macinf->ctmux[fpinf->cur_tb] = 1; /* DCCH/DTCH on RACH *always* has a C/T */
        bitoffs = tree_add_common_dcch_dtch_fields(tvb, pinfo, rach_tree, bitoffs, fpinf, macinf, rlcinf);
    }

    chan = fpinf->cur_chan;
    /* handoff to next dissector */
    switch (tctf) {
        case TCTF_CCCH_RACH_FDD:
            proto_item_append_text(ti, " (CCCH)");
            channel_type = proto_tree_add_uint(rach_tree, hf_mac_channel, tvb, 0, 0, MAC_CCCH);
            proto_item_set_generated(channel_type);
            next_tvb = tvb_new_octet_aligned(tvb, bitoffs, fpinf->chan_tf_size[chan] - bitoffs);
            add_new_data_source(pinfo, next_tvb, "Octet-Aligned CCCH Data");
            call_dissector_with_data(rlc_ccch_handle, next_tvb, pinfo, tree, data);
            break;
        case TCTF_DCCH_DTCH_RACH_FDD:
            /*Set RLC Mode/MAC content based on the L-CHID derived from the C/T flag*/
            c_t = tvb_get_bits8(tvb,bitoffs-4,4);
            if (c_t == 15) {
                /* reserved value, discard PDU */
                expert_add_info(pinfo, NULL, &ei_mac_reserved_c_t);
                break;
            }
            rlcinf->mode[chan] = lchId_rlc_map[c_t+1];
            macinf->content[chan] = lchId_type_table[c_t+1];
            rlcinf->rbid[chan] = c_t+1;
            switch (macinf->content[chan]) {
                case MAC_CONTENT_DCCH:
                    proto_item_append_text(ti, " (DCCH)");
                    channel_type = proto_tree_add_uint(rach_tree, hf_mac_channel, tvb, 0, 0, MAC_DCCH);
                    proto_item_set_generated(channel_type);
                    next_tvb = tvb_new_octet_aligned(tvb, bitoffs, fpinf->chan_tf_size[chan] - bitoffs);
                    add_new_data_source(pinfo, next_tvb, "Octet-Aligned DCCH Data");
                    call_dissector_with_data(rlc_dcch_handle, next_tvb, pinfo, tree, data);
                    break;
                case MAC_CONTENT_PS_DTCH:
                    proto_item_append_text(ti, " (PS DTCH)");
                    channel_type = proto_tree_add_uint(rach_tree, hf_mac_channel, tvb, 0, 0, MAC_DTCH);
                    proto_item_set_generated(channel_type);
                    next_tvb = tvb_new_octet_aligned(tvb, bitoffs, fpinf->chan_tf_size[chan] - bitoffs);
                    add_new_data_source(pinfo, next_tvb, "Octet-Aligned DTCH Data");
                    call_dissector_with_data(rlc_ps_dtch_handle, next_tvb, pinfo, tree, data);
                    break;
                case MAC_CONTENT_CS_DTCH:
                    proto_item_append_text(ti, " (CS DTCH)");
                    /* TODO */
                    break;
                default:
                    proto_item_append_text(ti, " (Unknown RACH DCCH/DTCH Content)");
                    expert_add_info_format(pinfo, NULL, &ei_mac_unknown_content, "Unknown RACH DCCH/DTCH Content");
            }
            break;
        default:
            proto_item_append_text(ti, " (Unknown RACH TCTF)");
            expert_add_info_format(pinfo, NULL, &ei_mac_rach_tctf_unknown, "Unknown RACH TCTF");
    }
    return tvb_captured_length(tvb);
}

static int dissect_mac_fdd_fach(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    uint8_t        hdr, tctf;
    uint16_t       bitoffs   = 0;
    uint16_t       tctf_len, chan;
    proto_tree    *fach_tree = NULL;
    proto_item    *channel_type;
    tvbuff_t *next_tvb;
    umts_mac_info *macinf;
    fp_info       *fpinf;
    rlc_info      *rlcinf;
    struct rrc_info *rrcinf;
    proto_item    *ti        = NULL;
    int c_t;
    hdr = tvb_get_uint8(tvb, 0);

    /* get target channel type field */
    tctf = fach_fdd_tctf(hdr, &bitoffs);
    tctf_len = bitoffs;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MAC");

    col_set_str(pinfo->cinfo, COL_INFO,
        val_to_str_const(tctf, fach_fdd_tctf_vals, "Unknown TCTF"));

    ti = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
    fach_tree = proto_item_add_subtree(ti, ett_mac_fach);

    macinf = (umts_mac_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_mac, 0);
    fpinf  = (fp_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_fp, 0);
    rlcinf = (rlc_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_rlc, 0);

    if (!macinf || !fpinf) {
        proto_tree_add_expert(fach_tree, pinfo, &ei_mac_per_frame_info_missing, tvb, 0, -1);
        return 1;
    }

    proto_tree_add_bits_item(fach_tree, hf_mac_fach_fdd_tctf, tvb, 0, tctf_len, ENC_BIG_ENDIAN);
    if (tctf == TCTF_DCCH_DTCH_FACH_FDD) {
        macinf->ctmux[fpinf->cur_tb] = 1; /* DCCH/DTCH on FACH *always* has a C/T */
        bitoffs = tree_add_common_dcch_dtch_fields(tvb, pinfo, fach_tree, bitoffs, fpinf, macinf, rlcinf);
    }

    chan = fpinf->cur_chan;
    switch (tctf) {
        case TCTF_CCCH_FACH_FDD:
            proto_item_append_text(ti, " (CCCH)");
            channel_type = proto_tree_add_uint(fach_tree, hf_mac_channel, tvb, 0, 0, MAC_CCCH);
            proto_item_set_generated(channel_type);
            /* CCCH over FACH is always octet aligned */
            next_tvb = tvb_new_subset_remaining(tvb, 1);
            call_dissector_with_data(rlc_ccch_handle, next_tvb, pinfo, tree, data);
            break;
        case TCTF_DCCH_DTCH_FACH_FDD:

            /*Set RLC Mode based on the L-CHID derived from the C/T flag*/
            c_t = tvb_get_bits8(tvb,bitoffs-4,4);
            if (c_t == 15) {
                /* reserved value, discard PDU */
                expert_add_info(pinfo, NULL, &ei_mac_reserved_c_t);
                break;
            }
            rlcinf->mode[fpinf->cur_tb] = lchId_rlc_map[c_t+1];
            macinf->content[fpinf->cur_tb] = lchId_type_table[c_t+1];
            switch (macinf->content[fpinf->cur_tb]) {

                case MAC_CONTENT_DCCH:
                    proto_item_append_text(ti, " (DCCH)");
                    channel_type = proto_tree_add_uint(fach_tree, hf_mac_channel, tvb, 0, 0, MAC_DCCH);
                    proto_item_set_generated(channel_type);
                    next_tvb = tvb_new_octet_aligned(tvb, bitoffs, fpinf->chan_tf_size[chan] - bitoffs);
                    add_new_data_source(pinfo, next_tvb, "Octet-Aligned DCCH Data");
                    call_dissector_with_data(rlc_dcch_handle, next_tvb, pinfo, tree, data);
                    break;
                case MAC_CONTENT_PS_DTCH:
                    proto_item_append_text(ti, " (PS DTCH)");
                    channel_type = proto_tree_add_uint(fach_tree, hf_mac_channel, tvb, 0, 0, MAC_DTCH);
                    proto_item_set_generated(channel_type);
                    next_tvb = tvb_new_octet_aligned(tvb, bitoffs, fpinf->chan_tf_size[chan] - bitoffs);
                    add_new_data_source(pinfo, next_tvb, "Octet-Aligned DCCH Data");
                    call_dissector_with_data(rlc_ps_dtch_handle, next_tvb, pinfo, tree, data);
                    break;
                case MAC_CONTENT_CS_DTCH:
                    proto_item_append_text(ti, " (CS DTCH)");
                    expert_add_info(pinfo, NULL, &ei_mac_cs_dtch_not_implemented);
                    /* TODO */
                    break;
                default:
                    proto_item_append_text(ti, " (Unknown FACH Content)");
                    expert_add_info_format(pinfo, NULL, &ei_mac_unknown_content, "Unknown FACH Content for this transportblock");
            }
            break;
        case TCTF_CTCH_FACH_FDD:
            proto_item_append_text(ti, " (CTCH)");
            channel_type = proto_tree_add_uint(fach_tree, hf_mac_channel, tvb, 0, 0, MAC_CTCH);
            proto_item_set_generated(channel_type);
            /* CTCH over FACH is always octet aligned */
            next_tvb = tvb_new_subset_remaining(tvb, 1);
            call_dissector_with_data(rlc_ctch_handle, next_tvb, pinfo, tree, data);
            break;
        /* july 5: Added support for BCCH*/
        case TCTF_BCCH_FACH_FDD:
            proto_item_append_text(ti, " (BCCH)");
            channel_type = proto_tree_add_uint(fach_tree, hf_mac_channel, tvb, 0, 0, MAC_BCCH);
            proto_item_set_generated(channel_type);

            /*We need to skip the first two bits (the TCTF bits), and since there is no MAC header, send rest to RRC*/
            next_tvb= tvb_new_octet_aligned(tvb, 2, (tvb_reported_length(tvb)*8)-2);
            add_new_data_source(pinfo, next_tvb, "Octet-Aligned BCCH Data");

            /* In this case skip RLC and call RRC immediately subdissector */
            rrcinf = (rrc_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_rrc, 0);
            if (!rrcinf) {
                rrcinf = wmem_new0(wmem_file_scope(), struct rrc_info);
                p_add_proto_data(wmem_file_scope(), pinfo, proto_rrc, 0, rrcinf);
            }
            rrcinf->msgtype[fpinf->cur_tb] = RRC_MESSAGE_TYPE_BCCH_FACH;

            call_dissector_with_data(rrc_handle, next_tvb, pinfo, tree, data);

            break;
        case TCTF_MSCH_FACH_FDD:
        case TCTF_MCCH_FACH_FDD:
        case TCTF_MTCH_FACH_FDD:
            expert_add_info(pinfo, NULL, &ei_mac_fach_content_type_unknown);
            break;
        default:
            proto_item_append_text(ti, " (Unknown FACH Content)");
            expert_add_info_format(pinfo, NULL, &ei_mac_unknown_content, " Unknown FACH Content");
            break;
    }
    return tvb_captured_length(tvb);
}

static int dissect_mac_fdd_dch(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    uint16_t       pos;
    uint8_t        bitoffs  = 0;
    umts_mac_info *macinf;
    fp_info       *fpinf;
    rlc_info      *rlcinf;
    proto_tree    *dch_tree = NULL;
    proto_item    *channel_type;
    tvbuff_t      *next_tvb;
    proto_item    *ti       = NULL;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MAC");

    ti = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
    dch_tree = proto_item_add_subtree(ti, ett_mac_dch);

    macinf = (umts_mac_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_mac, 0);
    fpinf  = (fp_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_fp, 0);
    rlcinf = (rlc_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_rlc, 0);

    if (!macinf || !fpinf) {
        proto_tree_add_expert(dch_tree, pinfo, &ei_mac_per_frame_info_missing, tvb, 0, -1);
        return 1;
    }
    pos = fpinf->cur_tb;

    if (macinf->ctmux[pos]) {
        if(rlcinf){
            rlcinf->rbid[fpinf->cur_tb] = tvb_get_bits8(tvb, bitoffs, 4)+1;
        }
        /*Add CT flag to GUI*/
        proto_tree_add_bits_item(dch_tree, hf_mac_ct, tvb, 0, 4, ENC_BIG_ENDIAN);
        bitoffs = 4;
    }

    if (bitoffs) {
        next_tvb = tvb_new_octet_aligned(tvb, bitoffs, fpinf->chan_tf_size[pos] - bitoffs);
        add_new_data_source(pinfo, next_tvb, "Octet-Aligned DCCH Data");
    } else
        next_tvb = tvb;
    switch (macinf->content[pos]) {
        case MAC_CONTENT_DCCH:
            proto_item_append_text(ti, " (DCCH)");

            /*Show logical channel id*/
            channel_type = proto_tree_add_uint(dch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);
            if(macinf->lchid[pos]!= 255){

                if(macinf->fake_chid[pos]){
                    expert_add_info(pinfo, channel_type, &ei_mac_faked_logical_channel_id);
                }
            }else{
                expert_add_info(pinfo, channel_type, &ei_mac_no_logical_channel);
            }

            channel_type = proto_tree_add_uint(dch_tree, hf_mac_channel, tvb, 0, 0, MAC_DCCH);
            proto_item_set_generated(channel_type);

            /*Transport channel printout*/
            channel_type = proto_tree_add_uint(dch_tree, hf_mac_trch_id, tvb, 0, 0, macinf->trchid[pos]);
            proto_item_set_generated(channel_type);
            call_dissector_with_data(rlc_dcch_handle, next_tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_PS_DTCH:
            proto_item_append_text(ti, " (PS DTCH)");
             /*Show logical channel id*/
            channel_type = proto_tree_add_uint(dch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);

            if(macinf->lchid[pos]== 255){
                expert_add_info(pinfo, channel_type, &ei_mac_no_logical_channel);
            }

            channel_type = proto_tree_add_uint(dch_tree, hf_mac_channel, tvb, 0, 0, MAC_DTCH);
            proto_item_set_generated(channel_type);
            call_dissector_with_data(rlc_ps_dtch_handle, next_tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_CS_DTCH:
            proto_item_append_text(ti, " (CS DTCH)");
            /*Show logical channel id*/
            channel_type = proto_tree_add_uint(dch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);
            if(macinf->lchid[pos]!= 255){
                if(macinf->fake_chid[pos]){
                    expert_add_info(pinfo, channel_type, &ei_mac_faked_logical_channel_id);
                }
            }else{
                expert_add_info(pinfo, channel_type, &ei_mac_no_logical_channel);
            }

            channel_type = proto_tree_add_uint(dch_tree, hf_mac_channel, tvb, 0, 0, MAC_DTCH);
            proto_item_set_generated(channel_type);

            /*Transport channel printout*/
            channel_type = proto_tree_add_uint(dch_tree, hf_mac_trch_id, tvb, 0, 0, macinf->trchid[pos]);
            proto_item_set_generated(channel_type);

            break;
        default:
            proto_item_append_text(ti, " (Unknown DCH Content)");
            expert_add_info_format(pinfo, NULL, &ei_mac_unknown_content, "Unknown DCH Content");
    }
    return tvb_captured_length(tvb);
}

static void init_frag(tvbuff_t * tvb, body_parts * bp, unsigned length, unsigned offset, uint32_t frame_num, uint16_t tsn, uint8_t type)
{
    mac_is_fragment * frag = wmem_new(wmem_file_scope(), mac_is_fragment);
    frag->type = type;
    frag->length = length;
    frag->data = (uint8_t *)wmem_alloc(wmem_file_scope(), length);
    frag->frame_num = frame_num;
    frag->tsn = tsn;
    frag->next = NULL;
    switch (type) {
        case MAC_IS_HEAD:
            DISSECTOR_ASSERT(bp->head == NULL);
            bp->head = frag;
            break;
        case MAC_IS_MIDDLE:
            DISSECTOR_ASSERT(bp->middle == NULL);
            bp->middle = frag;
            break;
        case MAC_IS_TAIL:
            DISSECTOR_ASSERT(bp->tail == NULL);
            bp->tail = frag;
            break;
    }
    tvb_memcpy(tvb, frag->data, offset, length);
}

static void mac_is_copy(mac_is_sdu * sdu, mac_is_fragment * frag, unsigned total_length, bool reverse)
{
    DISSECTOR_ASSERT(sdu->length+frag->length <= total_length);
    if (reverse) {
        memcpy(sdu->data+total_length-frag->length-sdu->length, frag->data, frag->length);
    } else {
        memcpy(sdu->data+sdu->length, frag->data, frag->length);
    }
    sdu->length += frag->length;
    wmem_free(wmem_file_scope(), frag->data);
}

/*
 * @param length Length of whole SDU, it will be verified.
 */
static tvbuff_t * reassemble(tvbuff_t * tvb, body_parts ** body_parts_array, uint16_t head_tsn, unsigned length, mac_is_channel * ch, unsigned frame_num)
{
    mac_is_sdu * sdu;
    mac_is_fragment * f;
    uint16_t i;
    GHashTable * sdus;

    /* Find frag->sdu hash table for this channel. */
    sdus = (GHashTable *)g_hash_table_lookup(mac_is_sdus, ch);
    /* If this is the first time we see this channel. */
    if (sdus == NULL) {
        mac_is_channel * channel;
        sdus = g_hash_table_new(mac_is_fragment_hash, mac_is_fragment_equal);
        channel = wmem_new(wmem_file_scope(), mac_is_channel);
        *channel = *ch;
        g_hash_table_insert(mac_is_sdus, channel, sdus);
    }

    sdu = wmem_new(wmem_file_scope(), mac_is_sdu);
    sdu->length = 0;
    sdu->data = (uint8_t *)wmem_alloc(wmem_file_scope(), length);

    f = body_parts_array[head_tsn]->head; /* Start from head. */
    g_hash_table_insert(sdus, f, sdu); /* Insert head->sdu mapping. */
    body_parts_array[head_tsn]->head = NULL; /* Reset head. */
    mac_is_copy(sdu, f, length, false); /* Copy head data into SDU. */
    sdu->fragments = f; /* Set up fragments list to point at head. */
    sdu->frame_num = frame_num; /* Frame number where reassembly is being done. */

    for (i = (head_tsn+1)%MAX_TSN; body_parts_array[i]->middle != NULL; i = (i+1)%MAX_TSN)
    {
        f = f->next = body_parts_array[i]->middle; /* Iterate through. */
        g_hash_table_insert(sdus, f, sdu); /* Insert middle->sdu mapping. */
        body_parts_array[i]->middle = NULL; /* Reset. */
        mac_is_copy(sdu, f, length, false); /* Copy middle data into SDU. */
    }
    DISSECTOR_ASSERT(body_parts_array[i]->tail != NULL);

    f->next = body_parts_array[i]->tail;
    g_hash_table_insert(sdus, f->next, sdu); /* Insert tail->sdu mapping. */
    body_parts_array[i]->tail = NULL; /* Reset tail. */
    sdu->tsn = i; /* Use TSN of tail as key for the SDU. */
    mac_is_copy(sdu, f->next, length, false); /* Copy tail data into SDU. */

    return tvb_new_child_real_data(tvb, sdu->data, sdu->length, sdu->length);
}

static mac_is_sdu * get_sdu(unsigned frame_num, uint16_t tsn, uint8_t type, mac_is_channel * ch)
{
    mac_is_sdu * sdu = NULL;
    GHashTable * sdus = NULL;
    mac_is_fragment frag_lookup_key;

    sdus = (GHashTable *)g_hash_table_lookup(mac_is_sdus, ch);
    if (sdus) {
        frag_lookup_key.frame_num = frame_num;
        frag_lookup_key.tsn = tsn;
        frag_lookup_key.type = type;
        sdu = (mac_is_sdu *)g_hash_table_lookup(sdus, &frag_lookup_key);
        return sdu;
    }
    return NULL;
}

static tvbuff_t * add_to_tree(tvbuff_t * tvb, packet_info * pinfo, proto_tree * tree, mac_is_sdu * sdu, unsigned offset, uint16_t maclength, uint8_t type)
{
    tvbuff_t * new_tvb = NULL;

    if (sdu->frame_num == pinfo->num) {
        mac_is_fragment * f = sdu->fragments;
        unsigned counter = 0;
        new_tvb = tvb_new_child_real_data(tvb, sdu->data, sdu->length, sdu->length);
        add_new_data_source(pinfo, new_tvb, "Reassembled MAC-is SDU");
        proto_tree_add_expert(tree, pinfo, &ei_mac_macis_sdu_reassembled, new_tvb, 0, -1);

        while (f) {
            proto_tree_add_uint_format_value(tree, hf_mac_is_fraglink, new_tvb,
                    counter, f->length, f->frame_num,
                    "%u, payload: %u-%u (%u bytes) (TSN: %u)",
                    f->frame_num, counter, counter+f->length-1, f->length,
                    f->tsn);
            counter += f->length;
            f = f->next;
        }
        return new_tvb;
    } else {
        new_tvb = tvb_new_subset_length_caplen(tvb, offset, maclength, -1);
        switch (type) {
            case MAC_IS_HEAD:
                proto_tree_add_expert(tree, pinfo, &ei_mac_macis_sdu_first, new_tvb, 0, -1);
                break;
            case MAC_IS_MIDDLE:
                proto_tree_add_expert(tree, pinfo, &ei_mac_macis_sdu_middle, new_tvb, 0, -1);
                break;
            case MAC_IS_TAIL:
                proto_tree_add_expert(tree, pinfo, &ei_mac_macis_sdu_last, new_tvb, 0, -1);
                break;
        }
        proto_tree_add_uint(tree, hf_mac_is_reasmin, new_tvb, 0, 0, sdu->frame_num);
        return NULL; /* No data here. */
    }
}

/*
 * If return value > 0 then tsn is changed to be tsn of head.
 * @return return length of sequence tsn-1 to head.
 */
static unsigned find_head(body_parts ** body_parts_array, uint16_t * tsn)
{
    unsigned length = 0;
    *tsn = (*tsn==0)? (uint16_t)(MAX_TSN-1) : (*tsn)-1;
    for (; body_parts_array[*tsn]->middle != NULL; *tsn = (*tsn==0)?(uint16_t)(MAX_TSN-1):(*tsn)-1)
        length += body_parts_array[*tsn]->middle->length;
    if (body_parts_array[*tsn]->head != NULL)
        return length+body_parts_array[*tsn]->head->length;
    return 0;
}

/*
 * @return return length of sequence tsn+1 to tail.
 */
static unsigned find_tail(body_parts ** body_parts_array, uint16_t tsn)
{
    unsigned length = 0;
    for (tsn = (tsn+1)%MAX_TSN; body_parts_array[tsn]->middle != NULL; tsn = (tsn+1)%MAX_TSN)
        length += body_parts_array[tsn]->middle->length;
    if (body_parts_array[tsn]->tail != NULL)
        return length+body_parts_array[tsn]->tail->length;
    return 0;
}

/*
 * @param ch Channel for which body parts are to be fetched.
 * @return Array of body_part* for channel 'ch'.
 */
static body_parts ** get_body_parts(mac_is_channel * ch)
{
    body_parts ** bpa = (body_parts **)g_hash_table_lookup(mac_is_fragments, ch);
    /* If there was no body_part* array for this channel, create one. */
    if (bpa == NULL) {
        mac_is_channel * channel;
        bpa = wmem_alloc_array(wmem_file_scope(), body_parts*, MAX_TSN); /* Create new body_parts-pointer array */
        for (size_t i = 0; i < MAX_TSN; i++) {
            bpa[i] = wmem_new0(wmem_file_scope(), body_parts); /* Fill it with body_parts. */
        }
        channel = wmem_new(wmem_file_scope(), mac_is_channel); /* Alloc new channel for use in hash table. */
        *channel = *ch;
        g_hash_table_insert(mac_is_fragments, channel, bpa);
    }
    return bpa;
}

static tvbuff_t * mac_is_add_fragment(tvbuff_t * tvb _U_, packet_info *pinfo, proto_tree * tree _U_, uint8_t lchid, unsigned ueid, int offset, uint8_t ss, uint16_t tsn, int sdu_no, uint8_t no_sdus, uint16_t maclength)
{
    mac_is_channel ch; /* Channel for looking up in hash tables. */
    ch.lchid = lchid;
    ch.ueid = ueid;

    /* If in first scan-through. */
    if (!PINFO_FD_VISITED(pinfo)) {
        /* Get body parts array for this channel. */
        body_parts ** body_parts_array = get_body_parts(&ch);
        /* Middle segment */
        if (no_sdus == 1 && ss == 3) {
            unsigned head_length, tail_length;
            init_frag(tvb, body_parts_array[tsn], maclength, offset, pinfo->num, tsn, MAC_IS_MIDDLE);
            tail_length = find_tail(body_parts_array, tsn);
            if (tail_length > 0) {
                head_length = find_head(body_parts_array, &tsn);
                if (head_length > 0) {
                    /* tsn is now TSN of head */
                    return reassemble(tvb, body_parts_array, tsn, tail_length+head_length+maclength, &ch, pinfo->num);
                }
            }
            /* XXX: haven't confirmed if case when middle segment comes last
             * actually works or not. */
        }
        /* If first SDU is last segment of previous. A tail. */
        else if (sdu_no == 0 && (ss & 1) == 1) {
            unsigned length = maclength;
            init_frag(tvb, body_parts_array[tsn], maclength, offset, pinfo->num, tsn, MAC_IS_TAIL);
            length += find_head(body_parts_array, &tsn);
            if (length > maclength) {
                /* tsn is now TSN of head */
                return reassemble(tvb, body_parts_array, tsn, length, &ch, pinfo->num);
            }
        }
        /* If last SDU is first segment of next. A head. */
        else if (sdu_no == no_sdus-1 && (ss & 2) == 2) {
            unsigned length = maclength;
            init_frag(tvb, body_parts_array[tsn], maclength, offset, pinfo->num, tsn, MAC_IS_HEAD);
            length += find_tail(body_parts_array, tsn);
            if (length > maclength) {
                return reassemble(tvb, body_parts_array, tsn, length, &ch, pinfo->num);
            }
        /* If our SDU is not fragmented. */
        } else {
            DISSECTOR_ASSERT((sdu_no == 0) ? (ss&1) == 0 : ((sdu_no == no_sdus-1) ? (ss&2) == 0 : true));
            return tvb_new_subset_length_caplen(tvb, offset, maclength, -1);
        }
    /* If clicking on a packet. */
    } else {
        tvbuff_t * new_tvb = NULL;
        /* Middle segment */
        if (no_sdus == 1 && ss == 3) {
            mac_is_sdu * sdu = get_sdu(pinfo->num, tsn, MAC_IS_MIDDLE, &ch);
            if (sdu) {
                return add_to_tree(tvb, pinfo, tree, sdu, offset, maclength, MAC_IS_MIDDLE);
            }
        }
        /* If first SDU is last segment of previous. A tail. */
        else if (sdu_no == 0 && (ss & 1) == 1) {
            mac_is_sdu * sdu = get_sdu(pinfo->num, tsn, MAC_IS_TAIL, &ch);
            if (sdu) {
                return add_to_tree(tvb, pinfo, tree, sdu, offset, maclength, MAC_IS_TAIL);
            }
        }
        /* If last SDU is first segment of next. A head. */
        else if (sdu_no == no_sdus-1 && (ss & 2) == 2) {
            mac_is_sdu * sdu = get_sdu(pinfo->num, tsn, MAC_IS_HEAD, &ch);
            if (sdu) {
                return add_to_tree(tvb, pinfo, tree, sdu, offset, maclength, MAC_IS_HEAD);
            }
        } else {
            new_tvb = tvb_new_subset_length_caplen(tvb, offset, maclength, -1);
            proto_tree_add_expert(tree, pinfo, &ei_mac_macis_sdu_complete, new_tvb, 0, -1);
            proto_tree_add_item(tree, hf_mac_edch_type2_sdu_data, new_tvb, 0, -1, ENC_NA);
            return new_tvb;
        }
    }
    return NULL;
}

static void ss_interpretation(tvbuff_t * tvb, proto_tree * tree, uint8_t ss, unsigned number_of_mac_is_sdus, unsigned offset)
{
    switch (ss) {
        case 0:
            if (number_of_mac_is_sdus > 1) {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The first MAC-is SDU of the MAC-is PDU is a complete MAC-d PDU or MAC-c PDU. The last MAC-is SDU of the MAC-is PDU is a complete MAC-d PDU or MAC-c PDU.");
            } else {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The MAC-is SDU of the MAC-is PDU is a complete MAC-d PDU or MAC-c PDU.");
            }
            break;
        case 1:
            if (number_of_mac_is_sdus > 1) {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The last MAC-is SDU of the MAC-is PDU is a complete MAC-d PDU or MAC-c PDU. The first MAC-is SDU of the MAC-is PDU is the last segment of a MAC-d PDU or MAC-c PDU.");
            } else {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The MAC-is SDU of the MAC-is PDU is the last segment of a MAC-d PDU or MAC-c PDU.");
            }
            break;
        case 2:
            if (number_of_mac_is_sdus > 1) {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The first MAC-is SDU of the MAC-is PDU is a complete MAC-d PDU or MAC-c PDU. The last MAC-is SDU of the MAC-is PDU is the first segment of a MAC-d PDU or MAC-c PDU.");
            } else {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The MAC-is SDU of the MAC-is PDU is the first segment of a MAC-d PDU or MAC-c PDU.");
            }
            break;
        case 3:
            if (number_of_mac_is_sdus > 1) {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The first MAC-is SDU of the MAC-is PDU is the last segment of a MAC-d PDU or MAC-c PDU and the last MAC-is SDU of MAC-is PDU is the first segment of a MAC-d PDU or MAC-c PDU.");
            } else {
                proto_tree_add_uint_format_value(tree, hf_mac_edch_type2_ss_interpretation, tvb, offset, 1, ss,
                    "The MAC-is SDU is a middle segment of a MAC-d PDU or MAC-c PDU.");
            }
            break;
    }
}

static void call_rlc(tvbuff_t * tvb, packet_info * pinfo, proto_tree * tree, proto_item * ti, uint8_t lchid, void *data)
{
    switch (lchId_type_table[lchid]) {
        case MAC_CONTENT_DCCH:
            proto_item_append_text(ti, " (DCCH)");
            call_dissector_with_data(rlc_dcch_handle, tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_PS_DTCH:
            proto_item_append_text(ti, " (PS DTCH)");
            call_dissector_with_data(rlc_ps_dtch_handle, tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_CS_DTCH:
            proto_item_append_text(ti, " (CS DTCH)");
            /* TODO */
            break;
        default:
            proto_item_append_text(ti, " (Unknown EDCH Content)");
            expert_add_info_format(pinfo, ti, &ei_mac_unknown_content, "Unknown EDCH Content");
            break;
    }
}

/*
 * Dissect a MAC-is PDU.
 */
static int dissect_mac_fdd_edch_type2(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    unsigned sdu_no, subframe_bytes = 0, offset = 0;
    uint8_t ss;
    uint16_t tsn;
    proto_item *pi, *temp;
    proto_tree *macis_pdu_tree, *macis_sdu_tree;
    umts_mac_is_info * mac_is_info = (umts_mac_is_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_mac, 0);
    rlc_info * rlcinf = (rlc_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_rlc, 0);
    struct fp_info *p_fp_info = (struct fp_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_fp, 0);

    DISSECTOR_ASSERT(mac_is_info != NULL && rlcinf != NULL && p_fp_info != NULL);

    pi = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
    macis_pdu_tree = proto_item_add_subtree(pi, ett_mac_edch_type2);

    /* SS */
    ss = (tvb_get_uint8(tvb, offset) & 0xc0) >> 6;
    proto_tree_add_item(macis_pdu_tree, hf_mac_edch_type2_ss, tvb, offset, 1, ENC_BIG_ENDIAN);

    ss_interpretation(tvb, macis_pdu_tree, ss, mac_is_info->number_of_mac_is_sdus, offset);

    /* TSN */
    tsn = tvb_get_bits8(tvb, offset*8+2, mac_tsn_size);
    proto_tree_add_bits_item(macis_pdu_tree, hf_mac_edch_type2_tsn, tvb, offset*8+2, mac_tsn_size, ENC_BIG_ENDIAN);

    offset += (2+mac_tsn_size)/8;

    /* MAC-is SDUs (i.e. MACd PDUs) */
    for (sdu_no=0; sdu_no < mac_is_info->number_of_mac_is_sdus; sdu_no++) {
        proto_item *ti;
        tvbuff_t * asm_tvb;
        uint8_t lchid = mac_is_info->lchid[sdu_no]+1;
        unsigned sdulength = mac_is_info->sdulength[sdu_no];

        ti = proto_tree_add_item(tree, hf_mac_edch_type2_sdu, tvb, offset, sdulength, ENC_NA);
        macis_sdu_tree = proto_item_add_subtree(ti, ett_mac_edch_type2_sdu);
        proto_item_append_text(ti, " (Logical channel=%u, Len=%u)", lchid, sdulength);
        temp = proto_tree_add_uint(ti, hf_mac_lch_id, tvb, 0, 0, lchid);
        proto_item_set_generated(temp);
        /*Set up information needed for MAC and lower layers*/
        rlcinf->mode[sdu_no] = lchId_rlc_map[lchid]; /* Set RLC mode by lchid to RLC_MODE map in nbap.h */
        rlcinf->ueid[sdu_no] = p_fp_info->com_context_id;
        rlcinf->rbid[sdu_no] = lchid;
        rlcinf->li_size[sdu_no] = RLC_LI_7BITS;
        rlcinf->ciphered[sdu_no] = false;
        rlcinf->deciphered[sdu_no] = false;

        asm_tvb = mac_is_add_fragment(tvb, pinfo, macis_sdu_tree, lchid, p_fp_info->com_context_id, offset, ss, tsn, sdu_no, mac_is_info->number_of_mac_is_sdus, sdulength);
        if (asm_tvb != NULL) {
            call_rlc(asm_tvb, pinfo, tree, ti, lchid, data);
        }

        offset += sdulength;
        subframe_bytes += sdulength;
    }

    proto_item_append_text(pi, "-is PDU (SS=%u, TSN=%u, %u bytes in %u SDU fragments)",
                           ss, tsn, subframe_bytes, mac_is_info->number_of_mac_is_sdus);

    proto_item_set_len(pi, 1+subframe_bytes);
    /*total_bytes += subframe_bytes;*/
    return tvb_captured_length(tvb);
}

static int dissect_mac_fdd_edch(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    proto_tree    *edch_tree = NULL;
    proto_item    *channel_type;
    umts_mac_info *macinf;
    fp_info       *fpinf;
    uint16_t       pos;
    proto_item    *ti        = NULL;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MAC");

    ti = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
    edch_tree = proto_item_add_subtree(ti, ett_mac_edch);

    fpinf  = (fp_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_fp, 0);

    macinf = (umts_mac_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_mac, 0);
    if (!macinf|| !fpinf) {
        proto_tree_add_expert(edch_tree, pinfo, &ei_mac_per_frame_info_missing, tvb, 0, -1);
        return 1;
    }

    pos = fpinf->cur_tb;

    switch (macinf->content[pos]) {
        case MAC_CONTENT_DCCH:
            proto_item_append_text(ti, " (DCCH)");

            /*Show the logical channel id*/
            channel_type = proto_tree_add_uint(edch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);

            channel_type = proto_tree_add_uint(edch_tree, hf_mac_channel, tvb, 0, 0, MAC_DCCH);
            proto_item_set_generated(channel_type);


            call_dissector_with_data(rlc_dcch_handle, tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_PS_DTCH:
            proto_item_append_text(ti, " (PS DTCH)");

            /*Show the logical channel id*/
            channel_type = proto_tree_add_uint(edch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);

            channel_type = proto_tree_add_uint(edch_tree, hf_mac_channel, tvb, 0, 0, MAC_DTCH);
            proto_item_set_generated(channel_type);

            call_dissector_with_data(rlc_ps_dtch_handle, tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_CS_DTCH:
            proto_item_append_text(ti, " (CS DTCH)");
            /* TODO */
            break;
        default:
            proto_item_append_text(ti, " (Unknown EDCH Content)");
            expert_add_info_format(pinfo, ti, &ei_mac_unknown_content, "Unknown EDCH Content");
            break;
    }
    return tvb_captured_length(tvb);
}
/**
* Dissect hsdsch_common channel.
*
* This will dissect hsdsch common channels, we handle this separately
* since we might have to deal with MAC-ehs and or MAC-c headers
* (in the MAC PDU).
*
* @param tvb
* @param pinfo
* @param tree
* @return Void.
*/
#if 0
static void dissect_mac_fdd_hsdsch_common(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree)
{
    proto_tree    *hsdsch_tree = NULL;
    /*proto_item    *channel_type;
    */
    fp_info       *fpinf;
    umts_mac_info *macinf;
    uint16_t       pos;
  /*  uint8_t        bitoffs=0;
    tvbuff_t      *next_tvb;
    */
    proto_item    *ti  = NULL;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MAC");

    ti = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
    hsdsch_tree = proto_item_add_subtree(ti, ett_mac_hsdsch);

    fpinf  = (fp_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_fp, 0);
    macinf = (umts_mac_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_mac);

    if (!macinf) {
        proto_tree_add_expert(hsdsch_tree, pinfo, &ei_mac_per_frame_info_missing, tvb, 0, -1);
        return;
    }
    pos = fpinf->cur_tb;
    switch(macinf->content[pos]){
        /*In this case we don't have a MAC-c header 9.2.1.4*/

#if 0
        case MAC_CONTENT_CCCH:

            break;
        case MAC_CONTENT_PCCH:

            break;

        case MAC_CONTENT_BCCH:

            break;
#endif
        default:

            proto_item_append_text(ti, " (Unknown HSDSCH-Common Content)");
            expert_add_info_format(pinfo, NULL, &ei_mac_unknown_content, "Unknown HSDSCH-Common Content");
            break;
    }

}
#endif
/* to avoid unnecessary re-alignment, the 4 bit padding prepended to the HSDSCH in FP type 1
 * are handled in the MAC layer
 * If the C/T field is present, 'bitoffs' will be 8 (4 bit padding and 4 bit C/T) and
 * no re-alignment is necessary
 * If no C/T is present, the whole payload will be left-shifted by 4 bit
 */
static int dissect_mac_fdd_hsdsch(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void *data)
{
    proto_tree    *hsdsch_tree = NULL;
    proto_item    *channel_type;
    fp_info       *fpinf;
    umts_mac_info *macinf;
    uint16_t       pos;
    uint8_t        bitoffs=0;
    tvbuff_t      *next_tvb;
    proto_item    *ti          = NULL;
    rlc_info * rlcinf;

    /*struct rrc_info  *rrcinf = NULL;
    */
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MAC");

    ti = proto_tree_add_item(tree, proto_umts_mac, tvb, 0, -1, ENC_NA);
    hsdsch_tree = proto_item_add_subtree(ti, ett_mac_hsdsch);

    fpinf  = (fp_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_fp, 0);
    macinf = (umts_mac_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_mac, 0);

    pos = fpinf->cur_tb;
    bitoffs = fpinf->hsdsch_entity == ehs ? 0 : 4;   /*No MAC-d header for type 2*/

    if (!macinf) {
        proto_tree_add_expert(hsdsch_tree, pinfo, &ei_mac_per_frame_info_missing, tvb, 0, -1);
        return 1;
    }
    if (macinf->ctmux[pos]) {   /*The 4'st bits are padding*/
        proto_tree_add_bits_item(hsdsch_tree, hf_mac_ct, tvb, bitoffs, 4, ENC_BIG_ENDIAN);

        /*Sets the proper lchid, for later layers.*/
        macinf->lchid[pos] = tvb_get_bits8(tvb,bitoffs,4)+1;
        macinf->fake_chid[pos] = false;
        macinf->content[pos] = lchId_type_table[macinf->lchid[pos]];    /*Lookup MAC content*/

        rlcinf = (rlc_info *)p_get_proto_data(wmem_file_scope(), pinfo, proto_umts_rlc, 0);
        rlcinf->rbid[pos] = macinf->lchid[pos];
        rlcinf->mode[pos] =  lchId_rlc_map[macinf->lchid[pos]]; /*Look up RLC mode*/
        bitoffs += 4;
    }

    if ((bitoffs % 8) == 0) {
        next_tvb = tvb_new_subset_remaining(tvb, bitoffs/8);
    } else {
        next_tvb = tvb_new_octet_aligned(tvb, bitoffs, macinf->pdu_len);    /*Get rid of possible padding in at the end?*/
        add_new_data_source(pinfo, next_tvb, "Octet-Aligned HSDSCH Data");
    }

    switch (macinf->content[pos]) {
        case MAC_CONTENT_CCCH:
            proto_item_append_text(ti, " (CCCH)");
            /*Set the logical channel id if it exists */
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);
            if(macinf->lchid[pos] != 255){
                if(macinf->fake_chid[pos]){
                    expert_add_info(pinfo, channel_type, &ei_mac_faked_logical_channel_id);
                }
            }else{
                expert_add_info(pinfo, channel_type, &ei_mac_no_logical_channel);
            }
            /*Set the type of channel*/
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_channel, tvb, 0, 0, MAC_DCCH);
            proto_item_set_generated(channel_type);

            /*Set the MACd-Flow ID*/
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_macdflowd_id, tvb, 0, 0, macinf->macdflow_id[pos]);
            proto_item_set_generated(channel_type);
            call_dissector_with_data(rlc_ccch_handle, next_tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_DCCH:
            proto_item_append_text(ti, " (DCCH)");
          /*  channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_channel_hsdsch, tvb, 0, 0, MAC_DCCH);
            proto_item_set_generated(channel_type)*/
            /*Set the logical channel id if it exists */
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);
            if(macinf->lchid[pos] != 255){
                if(macinf->fake_chid[pos]){
                    expert_add_info(pinfo, channel_type, &ei_mac_faked_logical_channel_id);
                }
            }else{
                expert_add_info(pinfo, channel_type, &ei_mac_no_logical_channel);
            }

            /*Set the type of channel*/
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_channel, tvb, 0, 0, MAC_DCCH);

            proto_item_set_generated(channel_type);

            /*Set the MACd-Flow ID*/
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_macdflowd_id, tvb, 0, 0, macinf->macdflow_id[pos]);
            proto_item_set_generated(channel_type);
            call_dissector_with_data(rlc_dcch_handle, next_tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_PS_DTCH:
            proto_item_append_text(ti, " (PS DTCH)");

            /*Set the logical channel id if it exists */
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_lch_id, tvb, 0, 0, macinf->lchid[pos]);
            proto_item_set_generated(channel_type);
            if(macinf->lchid[pos] != 255){
                if(macinf->fake_chid[pos]){
                    expert_add_info(pinfo, channel_type, &ei_mac_faked_logical_channel_id);
                }
            }else{
                expert_add_info(pinfo, channel_type, &ei_mac_no_logical_channel);
            }

            /*Sets the channel type*/
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_channel, tvb, 0, 0, MAC_DTCH);

            proto_item_set_generated(channel_type);

            /*Set the MACd-Flow ID*/
            channel_type = proto_tree_add_uint(hsdsch_tree, hf_mac_macdflowd_id, tvb, 0, 0, macinf->macdflow_id[pos]);
            proto_item_set_generated(channel_type);

            call_dissector_with_data(rlc_ps_dtch_handle, next_tvb, pinfo, tree, data);
            break;
        case MAC_CONTENT_CS_DTCH:
            proto_item_append_text(ti, " (CS DTCH)");
            break;
        default:
            proto_item_append_text(ti, " (Unknown HSDSCH Content)");
           expert_add_info_format(pinfo, NULL, &ei_mac_unknown_content, "Unknown HSDSCH Content");
    }
    return tvb_captured_length(tvb);
}

static void mac_is_sdus_hash_destroy(void *data)
{
    g_hash_table_destroy((GHashTable *)data);
}

static void mac_init(void)
{
    mac_is_sdus = g_hash_table_new_full(mac_is_channel_hash, mac_is_channel_equal, NULL, mac_is_sdus_hash_destroy);
    mac_is_fragments = g_hash_table_new_full(mac_is_channel_hash, mac_is_channel_equal, NULL, NULL);
    if (global_mac_tsn_size == MAC_TSN_6BITS) {
        MAX_TSN = 64;
        mac_tsn_size = 6;
    } else {
        MAX_TSN = 16384;
        mac_tsn_size = 14;
    }
}

static void mac_cleanup(void)
{
    g_hash_table_destroy(mac_is_sdus);
    g_hash_table_destroy(mac_is_fragments);
}

void
proto_register_umts_mac(void)
{
    module_t *mac_module;
    static int *ett[] = {
        &ett_mac,
        &ett_mac_fach,
        &ett_mac_rach,
        &ett_mac_dch,
        &ett_mac_pch,
        &ett_mac_edch,
        &ett_mac_hsdsch,
        &ett_mac_edch_type2,
        &ett_mac_edch_type2_sdu,
        &ett_mac_resolved_urnti
    };
    /** XX: Looks like some duplicate filter names ?? **/
    /** XX: May be OK: See doc/README.developer       **/
    static hf_register_info hf[] = {
        { &hf_mac_rach_fdd_tctf,
          { "Target Channel Type Field", "mac.tctf",
            FT_UINT8, BASE_HEX, VALS(rach_fdd_tctf_vals), 0, NULL, HFILL }
        },
        { &hf_mac_fach_fdd_tctf,
          { "Target Channel Type Field", "mac.tctf",
            FT_UINT8, BASE_HEX, VALS(fach_fdd_tctf_vals), 0, NULL, HFILL }
        },
        { &hf_mac_ct,
          { "C/T", "mac.ct",
            FT_UINT8, BASE_HEX, NULL, 0, NULL, HFILL }
        },
        { &hf_mac_ueid_type,
          { "UEID Type", "mac.ueid_type",
            FT_UINT8, BASE_DEC, VALS(ueid_type_vals), 0, NULL, HFILL }
        },
        { &hf_mac_crnti,
          { "C-RNTI (UEID)", "mac.ueid",
            FT_UINT16, BASE_HEX, NULL, 0x0, NULL, HFILL }
        },
        { &hf_mac_urnti,
          { "U-RNTI (UEID)", "mac.ueid",
            FT_UINT32, BASE_HEX, NULL, 0x0, NULL, HFILL }
        },
        { &hf_mac_resolved_urnti,
          { "Resolved U-RNTI", "mac.resolved_urnti",
            FT_UINT32, BASE_HEX, NULL, 0x0,
            "The U-RNTI of the UE which is using the C-RNTI seen in this frame",
            HFILL }
        },
        { &hf_mac_crnti_urnti_match_frame,
          { "C-RNTI Allocation Frame", "mac.crnti_urnti_match_frame",
            FT_FRAMENUM, BASE_NONE, NULL, 0x0,
            "The frame number where the C-RNTI was allocated for the UE",
            HFILL }
        },
        { &hf_mac_channel,
          { "Logical Channel Type", "mac.logical_channel",
            FT_UINT16, BASE_DEC, VALS(mac_logical_channel_vals), 0, NULL, HFILL }
        },

#if 0
         { &hf_mac_channel_str,
          { "Logical Channel", "mac.logical_channel",
            FT_STRING, BASE_NONE, NULL, 0, NULL, HFILL }
        },
#endif
#if 0
        { &hf_mac_channel_hsdsch,
            { "MACd-FlowID", "mac.macd_flowid", FT_UINT16, BASE_DEC, NULL, 0x0,  NULL, HFILL }
        },
#endif
        { &hf_mac_macdflowd_id,
            { "MACd-FlowID", "mac.macd_flowid", FT_UINT16, BASE_DEC, NULL, 0x0,  NULL, HFILL }
        },
         { &hf_mac_lch_id,
            { "Logical Channel ID", "mac.logical_channel_id", FT_UINT16, BASE_DEC, NULL, 0x0,  NULL, HFILL }
        },
        { &hf_mac_trch_id,
            { "Transport Channel ID", "mac.transport_channel_id", FT_UINT16, BASE_DEC, NULL, 0x0,  NULL, HFILL }
        },
#if 0
        { &hf_mac_edch_type2_descriptors,
          { "MAC-is Descriptors",
            "mac.edch.type2.descriptors", FT_STRING, BASE_NONE, NULL, 0x0,
            NULL, HFILL
          }
        },
#endif
#if 0
        { &hf_mac_edch_type2_lchid,
          { "LCH-ID",
            "mac.logical_channel_id", FT_UINT8, BASE_HEX, NULL, 0xf0,
            NULL, HFILL
          }
        },
#endif
#if 0
        { &hf_mac_edch_type2_length,
          { "Length",
            "mac.edch.type2.length", FT_UINT16, BASE_DEC, NULL, 0x0ffe,
            NULL, HFILL
          }
        },
#endif
#if 0
        { &hf_mac_edch_type2_flag,
          { "Flag",
            "mac.edch.type2.lchid", FT_UINT8, BASE_HEX, NULL, 0x01,
            "Indicates if another entry follows", HFILL
          }
        },
#endif
        { &hf_mac_edch_type2_ss,
          { "SS",
            /* TODO: VALS */
            "mac.edch.type2.ss", FT_UINT8, BASE_HEX, NULL, 0xc0,
            "Segmentation Status", HFILL
          }
        },
        { &hf_mac_edch_type2_ss_interpretation,
          { "SS interpretation",
            "mac.edch.type2.ss_interpretation", FT_UINT8, BASE_HEX, NULL, 0x0,
            NULL, HFILL
          }
        },
        { &hf_mac_edch_type2_tsn,
          { "TSN",
            "mac.edch.type2.tsn", FT_UINT16, BASE_DEC, NULL, 0,
            "Transmission Sequence Number", HFILL
          }
        },
        { &hf_mac_edch_type2_sdu,
          { "MAC-is SDU",
            "mac.edch.type2.sdu", FT_NONE, BASE_NONE, NULL, 0x0,
            NULL, HFILL
          }
        },
        { &hf_mac_edch_type2_sdu_data,
          { "Data",
            "mac.edch.type2.sdu.data", FT_BYTES, BASE_NONE, NULL, 0x0,
            NULL, HFILL
          }
        },
#if 0
        { &hf_mac_edch_type2_subframe_header,
          { "Subframe header",
            "mac.edch.type2.subframeheader", FT_STRING, BASE_NONE, NULL, 0x0,
            "EDCH Subframe header", HFILL
          }
        },
#endif
        { &hf_mac_is_reasmin,
          { "Reassembled in frame", "mac.is.reasmin",
            FT_FRAMENUM, BASE_NONE, NULL, 0, NULL, HFILL }
        },
        { &hf_mac_is_fraglink,
          { "Frame", "mac.is.fraglink",
            FT_FRAMENUM, BASE_NONE, NULL, 0, NULL, HFILL }
        }
    };

    static ei_register_info ei[] = {
        { &ei_mac_per_frame_info_missing, { "mac.per_frame_info_missing", PI_MALFORMED, PI_ERROR, "Cannot dissect MAC frame because per-frame info is missing", EXPFILL }},
        { &ei_mac_unknown_content, { "mac.unknown_content", PI_MALFORMED, PI_ERROR, "Unknown RACH DCCH/DTCH Content", EXPFILL }},
        { &ei_mac_rach_tctf_unknown, { "mac.rach_tctf.unknown", PI_MALFORMED, PI_ERROR, "Unknown RACH TCTF", EXPFILL }},
        { &ei_mac_cs_dtch_not_implemented, { "mac.cs_dtch.not_implemented", PI_DEBUG, PI_ERROR, "CS DTCH Is not implemented", EXPFILL }},
        { &ei_mac_fach_content_type_unknown, { "mac.fach_content_type.unknown", PI_UNDECODED, PI_WARN, "Unimplemented FACH Content type!", EXPFILL }},
        { &ei_mac_no_logical_channel, { "mac.no_logical_channel", PI_PROTOCOL, PI_WARN, "Frame is missing logical channel", EXPFILL }},
        { &ei_mac_faked_logical_channel_id, { "mac.faked_logical_channel_id", PI_PROTOCOL, PI_WARN, "This is a faked logical channel id!", EXPFILL }},
        { &ei_mac_macis_sdu_reassembled, { "mac.macis_sdu.reassembled", PI_REASSEMBLE, PI_CHAT, "Reassembled MAC-is SDU", EXPFILL }},
        { &ei_mac_macis_sdu_first, { "mac.macis_sdu.first", PI_REASSEMBLE, PI_CHAT, "This MAC-is SDU is the first segment of a MAC-d PDU or MAC-c PDU", EXPFILL }},
        { &ei_mac_macis_sdu_middle, { "mac.macis_sdu.middle", PI_REASSEMBLE, PI_CHAT, "This MAC-is SDU is a middle segment of a MAC-d PDU or MAC-c PDU", EXPFILL }},
        { &ei_mac_macis_sdu_last, { "mac.macis_sdu.last", PI_REASSEMBLE, PI_CHAT, "This MAC-is SDU is the last segment of a MAC-d PDU or MAC-c PDU", EXPFILL }},
        { &ei_mac_macis_sdu_complete, { "mac.macis_sdu.complete", PI_REASSEMBLE, PI_CHAT, "This MAC-is SDU is a complete MAC-d PDU or MAC-c PDU", EXPFILL }},
        { &ei_mac_reserved_c_t, { "mac.reserved_ct", PI_PROTOCOL, PI_WARN, "C/T has a reserved value, PDU is discarded", EXPFILL }}
    };

    expert_module_t* expert_umts_mac;

    proto_umts_mac = proto_register_protocol("MAC", "MAC", "mac");
    proto_register_field_array(proto_umts_mac, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
    expert_umts_mac = expert_register_protocol(proto_umts_mac);
    expert_register_field_array(expert_umts_mac, ei, array_length(ei));

    register_dissector("mac.fdd.rach", dissect_mac_fdd_rach, proto_umts_mac);
    register_dissector("mac.fdd.fach", dissect_mac_fdd_fach, proto_umts_mac);
    register_dissector("mac.fdd.pch", dissect_mac_fdd_pch, proto_umts_mac);
    register_dissector("mac.fdd.dch", dissect_mac_fdd_dch, proto_umts_mac);
    register_dissector("mac.fdd.edch", dissect_mac_fdd_edch, proto_umts_mac);
    register_dissector("mac.fdd.edch.type2", dissect_mac_fdd_edch_type2, proto_umts_mac);
    register_dissector("mac.fdd.hsdsch", dissect_mac_fdd_hsdsch, proto_umts_mac);

    register_init_routine(mac_init);
    register_cleanup_routine(mac_cleanup);

    /* Preferences */
    mac_module = prefs_register_protocol(proto_umts_mac, NULL);
    prefs_register_enum_preference(mac_module, "tsn_size", "TSN size",
            "TSN size in bits, either 6 or 14 bit",
            &global_mac_tsn_size, tsn_size_enumvals, false);
}

void
proto_reg_handoff_umts_mac(void)
{
    rlc_pcch_handle    = find_dissector_add_dependency("rlc.pcch", proto_umts_mac);
    rlc_ccch_handle    = find_dissector_add_dependency("rlc.ccch", proto_umts_mac);
    rlc_ctch_handle    = find_dissector_add_dependency("rlc.ctch", proto_umts_mac);
    rlc_dcch_handle    = find_dissector_add_dependency("rlc.dcch", proto_umts_mac);
    rlc_ps_dtch_handle = find_dissector_add_dependency("rlc.ps_dtch", proto_umts_mac);

    rrc_handle = find_dissector_add_dependency("rrc", proto_umts_mac);
}

/*
 * Editor modelines  -  https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * vi: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
