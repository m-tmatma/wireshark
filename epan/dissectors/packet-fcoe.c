/*
 * packet-fcoe.c
 * Routines for FCoE dissection - Fibre Channel over Ethernet
 * Copyright (c) 2006 Nuova Systems, Inc. (jre@nuovasystems.com)
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * Based on packet-fcip.c, Copyright 2001, Dinesh G Dutt (ddutt@cisco.com)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*
 * For FCoE protocol details, see http://fcoe.com.
 */

#include "config.h"

#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/crc32-tvb.h>
#include <epan/expert.h>
#include "packet-fc.h"

void proto_register_fcoe(void);
void proto_reg_handoff_fcoe(void);

#define FCOE_HEADER_LEN   14        /* header: version, SOF, and padding */
#define FCOE_TRAILER_LEN   8        /* trailer: CRC, EOF, and padding */

typedef enum {
    FCOE_EOFn    = 0x41,
    FCOE_EOFt    = 0x42,
    FCOE_EOFrt   = 0x44,
    FCOE_EOFdt   = 0x46,
    FCOE_EOFni   = 0x49,
    FCOE_EOFdti  = 0x4E,
    FCOE_EOFrti  = 0x4F,
    FCOE_EOFa    = 0x50
} fcoe_eof_t;

typedef enum {
    FCOE_SOFf    = 0x28,
    FCOE_SOFi4   = 0x29,
    FCOE_SOFi2   = 0x2D,
    FCOE_SOFi3   = 0x2E,
    FCOE_SOFn4   = 0x31,
    FCOE_SOFn2   = 0x35,
    FCOE_SOFn3   = 0x36,
    FCOE_SOFc4   = 0x39
} fcoe_sof_t;

static const value_string fcoe_eof_vals[] = {
    {FCOE_EOFn,   "EOFn" },
    {FCOE_EOFt,   "EOFt" },
    {FCOE_EOFrt,  "EOFrt" },
    {FCOE_EOFdt,  "EOFdt" },
    {FCOE_EOFni,  "EOFni" },
    {FCOE_EOFdti, "EOFdti" },
    {FCOE_EOFrti, "EOFrti" },
    {FCOE_EOFa,   "EOFa" },
    {0, NULL}
};

static const value_string fcoe_sof_vals[] = {
    {FCOE_SOFf, "SOFf" },
    {FCOE_SOFi4, "SOFi4" },
    {FCOE_SOFi2, "SOFi2" },
    {FCOE_SOFi3, "SOFi3" },
    {FCOE_SOFn4, "SOFn4" },
    {FCOE_SOFn2, "SOFn2" },
    {FCOE_SOFn3, "SOFn3" },
    {FCOE_SOFc4, "SOFc4" },
    {0, NULL}
};

static int proto_fcoe;
static int hf_fcoe_ver;
static int hf_fcoe_len;
static int hf_fcoe_sof;
static int hf_fcoe_eof;
static int hf_fcoe_crc;
static int hf_fcoe_crc_status;

static int ett_fcoe;

static expert_field ei_fcoe_crc;

static dissector_handle_t fc_handle;
static dissector_handle_t fcoe_handle;


/* Looks for the EOF at a given offset. Returns NULL if the EOF is not
 * present, is not one of the known values, or if the next three bytes, if
 * present, are not padding. Otherwise returns the entry from the EOF
 * value_string. Intended for use with the newer T11 version, where the frame
 * length is not explicitly set (and padding is used). */
static const char *
fcoe_get_eof(tvbuff_t *tvb, int eof_offset)
{
    uint8_t     eof          = 0;
    const char *eof_str;
    int         padding_remaining;

    if (!tvb_bytes_exist(tvb, eof_offset, 1)) {
        return NULL;
    }

    padding_remaining = MIN(tvb_captured_length_remaining(tvb, eof_offset+1),3);
    if (tvb_memeql(tvb, eof_offset+1, (const uint8_t*)"\x00\x00\x00", padding_remaining)) {
        return NULL;
    }

    eof = tvb_get_uint8(tvb, eof_offset);
    eof_str = try_val_to_str(eof, fcoe_eof_vals);
    return eof_str;
}

static int
dissect_fcoe(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    int         crc_offset;
    int         eof_offset;
    int         frame_len    = 0;
    int         header_len   = FCOE_HEADER_LEN;
    unsigned    version;
    const char *ver;
    uint16_t    len_sof;
    int         bytes_remaining;
    uint8_t     sof          = 0;
    uint8_t     eof          = 0;
    const char *eof_str;
    const char *crc_msg;
    const char *len_msg;
    proto_item *ti;
    proto_tree *fcoe_tree;
    tvbuff_t   *next_tvb;
    bool        crc_exists;
    uint32_t    crc_computed = 0;
    uint32_t    crc          = 0;
    fc_data_t fc_data;

    /*
     * For now, handle both the version defined before and after August 2007.
     * In the newer version, byte 1 is reserved and always zero.  In the old
     * version, it'll never be zero.
     */
    if (tvb_get_uint8(tvb, 1)) {
        header_len = 2;
        len_sof = tvb_get_ntohs(tvb, 0);
        frame_len = ((len_sof & 0x3ff0) >> 2) - 4;
        sof = len_sof & 0xf;
        sof |= (sof < 8) ? 0x30 : 0x20;
        version = len_sof >> 14;
        ver = "pre-T11 ";
        if (version != 0)
            ver = wmem_strdup_printf(pinfo->pool, ver, "pre-T11 ver %d ", version);
        eof_offset = header_len + frame_len + 4;
        eof_str = "none";
        if (tvb_bytes_exist(tvb, eof_offset, 1)) {
            eof = tvb_get_uint8(tvb, eof_offset);
            eof_str = val_to_str(eof, fcoe_eof_vals, "0x%x");
        }
        /* Old format has a length field, so we can help the Ethernet dissector
         * guess about the FCS; note this format does not pad after the EOF */
        set_actual_length(tvb, eof_offset+1);
    } else {
        frame_len = tvb_reported_length_remaining(tvb, 0) -
          FCOE_HEADER_LEN - FCOE_TRAILER_LEN;
        sof = tvb_get_uint8(tvb, FCOE_HEADER_LEN - 1);

        /*
         * Only version 0 is defined at this point.
         * Don't print the version in the short summary if it is zero.
         */
        ver = "";
        version = tvb_get_uint8(tvb, 0) >> 4;
        if (version != 0)
            ver = wmem_strdup_printf(pinfo->pool, ver, "ver %d ", version);

        eof_offset = header_len + frame_len + 4;
        if (NULL == (eof_str = fcoe_get_eof(tvb, eof_offset))) {
            /* We didn't find the EOF, look 4 bytes earlier */
            if (NULL != (eof_str = fcoe_get_eof(tvb, eof_offset-4))) {
                /* Found it, so it seems there's an Ethernet FCS. */
                frame_len -= 4;
                set_actual_length(tvb, eof_offset);
                eof_offset -= 4;
            } else {
                if (tvb_bytes_exist(tvb, eof_offset, 1)) {
                    /* Hmm, we have enough bytes to look for the EOF
                     * but it's an unexpected value. */
                    eof = tvb_get_uint8(tvb, eof_offset);
                    eof_str = wmem_strdup_printf(pinfo->pool, "0x%x", eof);
                } else {
                    /* We just didn't capture enough to get the EOF */
                    eof_str = "none";
                }
            }
        }

    }
    col_set_str(pinfo->cinfo, COL_PROTOCOL, "FCoE");
    crc_offset = header_len + frame_len;

    bytes_remaining = tvb_captured_length_remaining(tvb, header_len);
    if (bytes_remaining > frame_len)
        bytes_remaining = frame_len;        /* backing length */
    next_tvb = tvb_new_subset_length_caplen(tvb, header_len, bytes_remaining, frame_len);

    /*
     * Check the CRC.
     */
    crc_msg = "";
    crc_exists = tvb_bytes_exist(tvb, crc_offset, 4);
    if (crc_exists) {
        crc = tvb_get_ntohl(tvb, crc_offset);
        crc_computed = crc32_802_tvb(next_tvb, frame_len);
        if (crc != crc_computed) {
            crc_msg = " [bad FC CRC]";
        }
    }
    len_msg = "";
    if ((frame_len % 4) != 0 || frame_len < 24) {
        len_msg = " [invalid length]";
    }

    ti = proto_tree_add_protocol_format(tree, proto_fcoe, tvb, 0,
                                        header_len,
                                        "FCoE %s(%s/%s) %d bytes%s%s", ver,
                                        val_to_str(sof, fcoe_sof_vals,
                                                   "0x%x"),
                                        eof_str, frame_len, crc_msg,
                                        len_msg);

    /* Dissect the FCoE header */

    fcoe_tree = proto_item_add_subtree(ti, ett_fcoe);
    proto_tree_add_uint(fcoe_tree, hf_fcoe_ver, tvb, 0, 1, version);
    if (tvb_get_uint8(tvb, 1)) {
        proto_tree_add_uint(fcoe_tree, hf_fcoe_len, tvb, 0, 2, frame_len);
    }
    proto_tree_add_uint(fcoe_tree, hf_fcoe_sof, tvb,
                        header_len - 1, 1, sof);

    /*
     * Create the CRC information.
     */
    if (crc_exists) {
        proto_tree_add_checksum(fcoe_tree, tvb, crc_offset, hf_fcoe_crc, hf_fcoe_crc_status, &ei_fcoe_crc, pinfo, crc_computed, ENC_BIG_ENDIAN, PROTO_CHECKSUM_VERIFY);
        proto_tree_set_appendix(fcoe_tree, tvb, crc_offset,
                                tvb_captured_length_remaining (tvb, crc_offset));
    } else {
        proto_tree_add_checksum(fcoe_tree, tvb, crc_offset, hf_fcoe_crc, hf_fcoe_crc_status, &ei_fcoe_crc, pinfo, 0, ENC_BIG_ENDIAN, PROTO_CHECKSUM_NOT_PRESENT);
    }

    /*
     * Interpret the EOF.
     */
    if (tvb_bytes_exist(tvb, eof_offset, 1)) {
        proto_tree_add_item(fcoe_tree, hf_fcoe_eof, tvb, eof_offset, 1, ENC_BIG_ENDIAN);
    }

    /* Set the SOF/EOF flags in the packet_info header */
    fc_data.sof_eof = 0;
    if (sof == FCOE_SOFi3 || sof == FCOE_SOFi2 || sof == FCOE_SOFi4) {
        fc_data.sof_eof = FC_DATA_SOF_FIRST_FRAME;
    } else if (sof == FCOE_SOFf) {
        fc_data.sof_eof = FC_DATA_SOF_SOFF;
    }

    if (eof != FCOE_EOFn) {
        fc_data.sof_eof |= FC_DATA_EOF_LAST_FRAME;
    } else if (eof != FCOE_EOFt) {
        fc_data.sof_eof |= FC_DATA_EOF_INVALID;
    }

    /* Call the FC Dissector if this is carrying an FC frame */
    fc_data.ethertype = ETHERTYPE_UNK;

    if (fc_handle) {
        call_dissector_with_data(fc_handle, next_tvb, pinfo, tree, &fc_data);
    } else {
        call_data_dissector(next_tvb, pinfo, tree);
    }
    return tvb_captured_length(tvb);
}

void
proto_register_fcoe(void)
{
    module_t *fcoe_module;

    /* Setup list of header fields  See Section 1.6.1 for details*/
    static hf_register_info hf[] = {
        { &hf_fcoe_sof,
          {"SOF", "fcoe.sof", FT_UINT8, BASE_HEX, VALS(fcoe_sof_vals), 0,
           NULL, HFILL}},
        { &hf_fcoe_eof,
          {"EOF", "fcoe.eof", FT_UINT8, BASE_HEX, VALS(fcoe_eof_vals), 0,
           NULL, HFILL}},
        { &hf_fcoe_ver,
          {"Version", "fcoe.ver", FT_UINT32, BASE_DEC, NULL, 0, NULL, HFILL}},
        { &hf_fcoe_len,
          {"Frame length", "fcoe.len", FT_UINT32,
            BASE_DEC, NULL, 0, NULL, HFILL}},
        { &hf_fcoe_crc,
          {"CRC", "fcoe.crc", FT_UINT32, BASE_HEX, NULL, 0, NULL, HFILL}},
        { &hf_fcoe_crc_status,
          {"CRC Status", "fcoe.crc.status", FT_UINT8, BASE_NONE, VALS(proto_checksum_vals), 0x0,
            NULL, HFILL }}
    };
    static int *ett[] = {
        &ett_fcoe,
    };

    static ei_register_info ei[] = {
        { &ei_fcoe_crc, { "fcoe.crc.bad", PI_CHECKSUM, PI_ERROR, "Bad checksum", EXPFILL }},
    };

    expert_module_t* expert_fcoe;

    /* Register the protocol name and description */
    proto_fcoe = proto_register_protocol("Fibre Channel over Ethernet",
        "FCoE", "fcoe");
    fcoe_handle = register_dissector("fcoe", dissect_fcoe, proto_fcoe);

    /* Required function calls to register the header fields and
     * subtrees used */
    proto_register_field_array(proto_fcoe, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
    expert_fcoe = expert_register_protocol(proto_fcoe);
    expert_register_field_array(expert_fcoe, ei, array_length(ei));

    fcoe_module = prefs_register_protocol_obsolete(proto_fcoe);

    prefs_register_obsolete_preference(fcoe_module, "ethertype");
}

void
proto_reg_handoff_fcoe(void)
{
    dissector_add_uint("ethertype", ETHERTYPE_FCOE, fcoe_handle);
    fc_handle   = find_dissector_add_dependency("fc", proto_fcoe);
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
