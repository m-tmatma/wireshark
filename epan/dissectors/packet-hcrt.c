/* packet-hcrt.c
 *
 * Routines for Hotline Command-Response Transaction (HCrt)
 * Protocol specifications (draft) are available here
 * https://github.com/ShepardSiegel/hotline/tree/master/doc
 *
 * Copyright 2013 Dario Lombardo (lomato@gmail.com)
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"
#include <epan/packet.h>
#include <epan/prefs.h>
#include <epan/expert.h>

static int proto_hcrt;

#define HCRT_UDP_PORTS_DEFAULT "47000"

static unsigned ethertype_pref = 0xf052;

static int hf_hcrt_header;
static int hf_hcrt_message_tag;
static int hf_hcrt_message_type;
static int hf_hcrt_am;
static int hf_hcrt_do;
static int hf_hcrt_1st_dword_enable;
static int hf_hcrt_last_dword_enable;
static int hf_hcrt_resp_code;
static int hf_hcrt_adl;
static int hf_hcrt_last;
static int hf_hcrt_body;
static int hf_hcrt_addr_32;
static int hf_hcrt_addr_64;
static int hf_hcrt_data_32;
static int hf_hcrt_data_64;
static int hf_hcrt_command_nop;

static int ett_hcrt;
static int ett_hcrt_msg;
static int ett_hcrt_hdr;
static int ett_hcrt_body;

static expert_field ei_hcrt_error;

void proto_reg_handoff_hcrt(void);
void proto_register_hcrt(void);

static dissector_handle_t hcrt_handle;

#define HCRT_HDR_LEN 4

#define HCRT_NOP      0x0
#define HCRT_WRITE    0x1
#define HCRT_READ     0x2
#define HCRT_RESPONSE 0x3

#define ADDR_MODE_32 1
#define ADDR_MODE_64 2

/* Message types */
static const value_string hcrt_message_types[] = {
    {0x00, "NOP"},
    {0x01, "Write"},
    {0x02, "Read"},
    {0x03, "Response"},
    {0, NULL}
};

/* Addressing modes */
static const value_string hcrt_ams[] = {
    {0x0, "32 bit"},
    {0x1, "64 bit"},
    {0, NULL}
};

/* Discovery operations */
static const true_false_string hcrt_dos = {
    "DO",
    "not DO",
};

static const value_string dword_enable_vals[] = {
    {0xF, "4B"},
    {0xC, "2B (MS)"},
    {0x3, "2B (LS)"},
    {0x8, "1B (B3)"},
    {0x4, "1B (B2)"},
    {0x2, "1B (B1)"},
    {0x1, "1B (B0)"},
    {0, NULL}
};

static const value_string response_codes[] = {
    {0x0, "OK"},
    {0x1, "Timeout"},
    {0x2, "Error"},
    {0x3, "Reserved"},
    {0x4, "Reserved"},
    {0x5, "Reserved"},
    {0x6, "Reserved"},
    {0x7, "Reserved"},
    {0x8, "Reserved"},
    {0x9, "Reserved"},
    {0xa, "Reserved"},
    {0xb, "Reserved"},
    {0xc, "Reserved"},
    {0xd, "Reserved"},
    {0xe, "Reserved"},
    {0xf, "Reserved"},
    {0, NULL}
};


static void dissect_hcrt_body(tvbuff_t* tvb, proto_tree* tree , unsigned* offset,
    int type, int addr_mode, int adl, int body_len)
{
    proto_item* ti_body;
    proto_tree* hcrt_body_tree;
    int i;

    ti_body = proto_tree_add_item(tree, hf_hcrt_body, tvb, *offset, body_len, ENC_NA);
    hcrt_body_tree = proto_item_add_subtree(ti_body, ett_hcrt_body);

    switch (type) {
        case HCRT_NOP:
            proto_tree_add_item(hcrt_body_tree, hf_hcrt_command_nop, tvb, *offset,
                body_len, ENC_NA);
            break;
        case HCRT_WRITE:
            if (addr_mode == ADDR_MODE_32) {
                /* Address (32) */
                proto_tree_add_item(hcrt_body_tree, hf_hcrt_addr_32, tvb, *offset,
                    4, ENC_LITTLE_ENDIAN);

                /* Data */
                for (i = 1; i <= adl; i++) {
                    proto_tree_add_item(hcrt_body_tree, hf_hcrt_data_32, tvb,
                        *offset + i * 4, 4, ENC_LITTLE_ENDIAN);
                }
            } else {
                /* Address (64) */
                proto_tree_add_item(hcrt_body_tree, hf_hcrt_addr_64, tvb, *offset,
                    8, ENC_LITTLE_ENDIAN);

                /* Data */
                for (i = 1; i <= adl; i++)
                    proto_tree_add_item(hcrt_body_tree, hf_hcrt_data_64, tvb,
                        *offset + i * 8, 8, ENC_LITTLE_ENDIAN);
            }
            break;
        case HCRT_READ:
            if (addr_mode == ADDR_MODE_32) {
                /* Address (32) */
                proto_tree_add_item(hcrt_body_tree, hf_hcrt_addr_32, tvb, *offset, 4,
                    ENC_LITTLE_ENDIAN);
            } else {
                /* Address (64) */
                proto_tree_add_item(hcrt_body_tree, hf_hcrt_addr_64, tvb, *offset, 8,
                    ENC_LITTLE_ENDIAN);
            }
            break;
        case HCRT_RESPONSE:
            if (body_len > 0) {
                proto_tree_add_item(hcrt_body_tree, hf_hcrt_command_nop, tvb, *offset,
                    body_len, ENC_NA);
            }
            break;
        default:
            DISSECTOR_ASSERT_NOT_REACHED();
            break;
    }

    (*offset) += body_len;
}

/* Returns true if this is the last message */
static bool dissect_hcrt_header(tvbuff_t* tvb, packet_info* pinfo, proto_tree* tree,
    unsigned* offset, uint8_t b0_first, uint8_t b0_current)
{
    proto_item* ti_hdr;
    proto_tree* hcrt_hdr_tree;
    bool last;
    uint8_t type;

    ti_hdr = proto_tree_add_item(tree, hf_hcrt_header, tvb, *offset, 4, ENC_NA);
    hcrt_hdr_tree = proto_item_add_subtree(ti_hdr, ett_hcrt_hdr);

    if (b0_first != b0_current) {
        expert_add_info_format(pinfo, hcrt_hdr_tree, &ei_hcrt_error,
            "Invalid Byte 0 in Header. Must be equal in all HCrt messages. "
                "Expected: %.2X, got: %.2X", b0_first, b0_current);
    }

    type = (b0_current & 0x30) >> 4;

    /* == Byte 0 == */
    /* TAG */
    proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_message_tag, tvb,
        *offset, 1, ENC_NA);
    /* Message Type */
    proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_message_type, tvb,
        *offset, 1, ENC_NA);
    /* Addressing Mode */
    proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_am, tvb,
        *offset, 1, ENC_NA);
    /* Discovery Operation */
    proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_do, tvb,
        *offset, 1, ENC_NA);
    (*offset)++;

    /* == Byte 1 == */
    if (type != HCRT_RESPONSE) {
        /* 1st DWORD enable */
        proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_1st_dword_enable, tvb,
            *offset, 1, ENC_NA);
    } else {
        /* Response Code */
        proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_resp_code, tvb,
            *offset, 1, ENC_NA);
    }

    if (type != HCRT_RESPONSE) {
        /* Last DWORD enable */
        proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_last_dword_enable, tvb,
            *offset, 1, ENC_NA);
    }
    (*offset)++;

    /* == Byte 2 & 3 == */
    /* ADL */
    proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_adl, tvb, *offset, 2, ENC_LITTLE_ENDIAN);
    /* Last */
    proto_tree_add_item(hcrt_hdr_tree, hf_hcrt_last, tvb, *offset, 2, ENC_LITTLE_ENDIAN);

    /* last */
    last = (tvb_get_letohs(tvb, *offset) & 0x8000) != 0;
    (*offset) += 2;
    return last;
}

/* Return true if this is the last message */
static bool dissect_hcrt_message(tvbuff_t* tvb, packet_info* pinfo, proto_tree* tree,
    unsigned* offset, uint8_t b0_first, int i)
{
    bool last;
    unsigned adl;
    unsigned addr_mode;
    unsigned body_len;
    proto_tree* hcrt_msg_tree;
    uint8_t b0_current;
    int type;

    /* Save byte 0 of current packet */
    b0_current = tvb_get_uint8(tvb, *offset);

    /* Get details from header */
    adl = tvb_get_letohs(tvb, *offset + 2) & 0x0FFF;
    addr_mode = (1 + ((b0_current & 0x40) >> 6));
    type = (b0_current & 0x30) >> 4;

    switch (type) {
        case HCRT_NOP:
            body_len = 4 * addr_mode * adl;
            break;
        case HCRT_WRITE:
            body_len = 4 * addr_mode * (adl + 1);
            break;
        case HCRT_READ:
            body_len = 4 * addr_mode;
            break;
        case HCRT_RESPONSE:
            body_len = 4 * addr_mode * adl;
            break;
        default:
            DISSECTOR_ASSERT_NOT_REACHED();
            break;
    }

    hcrt_msg_tree = proto_tree_add_subtree_format(tree, tvb, *offset,
        HCRT_HDR_LEN + body_len, ett_hcrt_msg, NULL, "Message %d", i);

    last = dissect_hcrt_header(tvb, pinfo, hcrt_msg_tree, offset, b0_first, b0_current);
    dissect_hcrt_body(tvb, hcrt_msg_tree, offset, type, addr_mode, adl, body_len);

    return last;
}

static int dissect_hcrt(tvbuff_t* tvb, packet_info* pinfo, proto_tree* tree, void* data _U_)
{
    uint8_t type;
    proto_item* ti;
    proto_tree* hcrt_tree;
    unsigned offset;
    int i = 1;
    uint8_t b0_first;
    uint8_t tag;
    unsigned adl;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "HCrt");
    col_clear(pinfo->cinfo, COL_INFO);

    /* Save byte 0 of first message. Will be checked against byte 0 of other messages */
    b0_first = tvb_get_uint8(tvb, 0);

    tag  = b0_first & 0x0F;
    type = (b0_first & 0x30) >> 4;
    adl  = tvb_get_letohs(tvb, 2) & 0x0FFF;

    col_add_fstr(pinfo->cinfo, COL_INFO, "Type: %s, Tag: 0x%X, ADL: %u",
        val_to_str(type, hcrt_message_types, "Unknown (0x%02x)"), tag, adl);

    if (adl == 1) {
        if (type == HCRT_READ || type == HCRT_WRITE) {
            col_append_fstr(pinfo->cinfo, COL_INFO, ", Address: 0x%.8X", tvb_get_letohl(tvb, 4));
        }
        if (type == HCRT_WRITE) {
            col_append_fstr(pinfo->cinfo, COL_INFO, ", Data: 0x%.8X", tvb_get_letohl(tvb, 8));
        }
    }

    offset = 0;
    ti = proto_tree_add_item(tree, proto_hcrt, tvb, 0, -1, ENC_NA);
    hcrt_tree = proto_item_add_subtree(ti, ett_hcrt);

    while (!dissect_hcrt_message(tvb, pinfo, hcrt_tree, &offset, b0_first, i)) {
        i++;
    }
    return tvb_captured_length(tvb);
}

void proto_register_hcrt(void)
{
    expert_module_t* expert_hcrt;
    module_t* hcrt_module;

    static hf_register_info hf[] = {
        { &hf_hcrt_header,
            { "Header", "hcrt.hdr",
            FT_NONE, BASE_NONE,
            NULL, 0x00,
            NULL, HFILL }
        },
        { &hf_hcrt_message_tag,
            { "Tag", "hcrt.tag",
            FT_UINT8, BASE_HEX,
            NULL, 0x0F,
            NULL, HFILL }
        },
        { &hf_hcrt_message_type,
            { "Type", "hcrt.type",
            FT_UINT8, BASE_DEC,
            VALS(hcrt_message_types), 0x30,
            NULL, HFILL }
        },
        { &hf_hcrt_am,
            { "Addressing Mode", "hcrt.am",
            FT_UINT8, BASE_DEC,
            VALS(hcrt_ams), 0x40,
            NULL, HFILL }
        },
        { &hf_hcrt_do,
            { "Discovery Operation", "hcrt.do",
            FT_BOOLEAN, 8,
            TFS(&hcrt_dos), 0x80,
            NULL, HFILL }
        },
        { &hf_hcrt_1st_dword_enable,
            { "1st DWORD enable", "hcrt.first_dword_enable",
            FT_UINT8, BASE_HEX,
            VALS(dword_enable_vals), 0xF0,
            NULL, HFILL }
        },
        { &hf_hcrt_last_dword_enable,
            { "Last DWORD enable", "hcrt.last_dword_enable",
            FT_UINT8, BASE_HEX,
            VALS(dword_enable_vals), 0x0F,
            NULL, HFILL }
        },
        { &hf_hcrt_resp_code,
            { "Response code", "hcrt.response_code",
            FT_UINT8, BASE_HEX,
            VALS(response_codes), 0xF0,
            NULL, HFILL }
        },
        { &hf_hcrt_adl,
            { "ADL", "hcrt.adl",
            FT_UINT16, BASE_DEC,
            NULL, 0x0FFF,
            NULL, HFILL }
        },
        { &hf_hcrt_last,
            { "Last message", "hcrt.last",
            FT_BOOLEAN, 16,
            NULL, 0x8000,
            NULL, HFILL }
        },
        { &hf_hcrt_body,
            { "Body", "hcrt.body",
            FT_NONE, BASE_NONE,
            NULL, 0x00,
            NULL, HFILL }
        },
        { &hf_hcrt_addr_32,
            { "Address", "hcrt.address32",
            FT_UINT32, BASE_HEX,
            NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_hcrt_addr_64,
            { "Address", "hcrt.address64",
            FT_UINT64, BASE_HEX,
            NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_hcrt_data_32,
            { "Data", "hcrt.data32",
            FT_UINT32, BASE_HEX,
            NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_hcrt_data_64,
            { "Data", "hcrt.data64",
            FT_UINT64, BASE_HEX,
            NULL, 0x0,
            NULL, HFILL }
        },
        { &hf_hcrt_command_nop,
            { "Command", "hcrt.command_nop",
            FT_BYTES, BASE_NONE,
            NULL, 0x0,
            NULL, HFILL }
        }
    };

    static ei_register_info ei[] = {
        { &ei_hcrt_error, { "hcrt.error", PI_MALFORMED, PI_ERROR, "Unusual error code", EXPFILL }}
    };

    /* Setup protocol subtree array */
    static int* ett[] = {
        &ett_hcrt,
        &ett_hcrt_msg,
        &ett_hcrt_hdr,
        &ett_hcrt_body,
    };

    proto_hcrt = proto_register_protocol ("Hotline Command-Response Transaction protocol", "HCrt", "hcrt");

    proto_register_field_array(proto_hcrt, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));
    expert_hcrt = expert_register_protocol(proto_hcrt);
    expert_register_field_array(expert_hcrt, ei, array_length(ei));

    hcrt_module = prefs_register_protocol(proto_hcrt, proto_reg_handoff_hcrt);
    prefs_register_uint_preference(hcrt_module,
        "dissector_ethertype",
        "Ethernet type",
        "The ethernet type used for L2 communications",
        10, &ethertype_pref);

    hcrt_handle = register_dissector("hcrt", dissect_hcrt, proto_hcrt);
}

void proto_reg_handoff_hcrt(void)
{
    static bool hcrt_prefs_initialized = false;
    static int hcrt_ethertype;

    if (!hcrt_prefs_initialized) {
        /* Also register as a dissector that can be selected by a TCP port number via
        "decode as" */
        dissector_add_for_decode_as_with_preference("tcp.port", hcrt_handle);
        dissector_add_uint_range_with_preference("udp.port", HCRT_UDP_PORTS_DEFAULT, hcrt_handle);
        hcrt_prefs_initialized = true;
    } else {
        dissector_delete_uint("ethertype", hcrt_ethertype, hcrt_handle);
    }

    hcrt_ethertype = ethertype_pref;

    dissector_add_uint("ethertype", hcrt_ethertype, hcrt_handle);
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
