/* packet-rfid-mifare.c
 * Dissector for the NXP MiFare Protocol
 *
 * References:
 * http://code.google.com/p/nfc-tools/source/browse/trunk/libfreefare/libfreefare/mifare_classic.c
 * http://www.nxp.com/documents/data_sheet/MF1S703x.pdf
 * http://www.nxp.com/documents/application_note/AN1304.pdf
 *
 * Copyright 2011, Tyson Key <tyson.key@gmail.com>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "config.h"

#include <epan/packet.h>

void proto_register_mifare(void);

static int proto_mifare;

static int hf_mifare_command;
static int hf_mifare_block_address;
static int hf_mifare_key_a;
static int hf_mifare_key_b;
static int hf_mifare_uid;
static int hf_mifare_operand;
static int hf_mifare_payload;

#define AUTH_A    0x60
#define AUTH_B    0x61
#define READ      0x30
#define WRITE     0xA0
#define TRANSFER  0xB0
#define DECREMENT 0xC0
#define INCREMENT 0xC1
#define RESTORE   0xC2

static const value_string hf_mifare_commands[] = {
    {AUTH_A,    "AUTH_A"},
    {AUTH_B,    "AUTH_B"},
    {READ,      "READ"},
    {WRITE,     "WRITE"},
    {TRANSFER,  "TRANSFER"},
    {DECREMENT, "DECREMENT"},
    {INCREMENT, "INCREMENT"},
    {RESTORE,   "RESTORE"},

    /* End of commands */
    {0x00, NULL}
};

/* Subtree handles: set by register_subtree_array */
static int ett_mifare;

static int
dissect_mifare(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    proto_item *item;
    proto_tree *mifare_tree;
    uint8_t     cmd;


    col_set_str(pinfo->cinfo, COL_PROTOCOL, "MiFare");
    col_clear(pinfo->cinfo, COL_INFO);

    /* Start with a top-level item to add everything else to */

    item = proto_tree_add_item(tree, proto_mifare, tvb, 0, -1, ENC_NA);
    mifare_tree = proto_item_add_subtree(item, ett_mifare);

    proto_tree_add_item(mifare_tree, hf_mifare_command, tvb, 0, 1, ENC_BIG_ENDIAN);
    cmd = tvb_get_uint8(tvb, 0);


    switch (cmd) {

        case AUTH_A:
            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(mifare_tree, hf_mifare_key_a, tvb, 2, 6, ENC_BIG_ENDIAN);
            proto_tree_add_item(mifare_tree, hf_mifare_uid, tvb, 8, 4, ENC_BIG_ENDIAN);
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Authenticate with Key A");
            break;

        case AUTH_B:
            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(mifare_tree, hf_mifare_key_b, tvb, 2, 6, ENC_BIG_ENDIAN);
            proto_tree_add_item(mifare_tree, hf_mifare_uid, tvb, 8, 4, ENC_BIG_ENDIAN);
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Authenticate with Key B");
            break;

        case READ:
            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Read");
            break;

        case WRITE:
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Write");

            /* LibNFC and the TouchATag-branded reader don't expose the 2-byte CRC
               or 4-bit NAK, as per MF1S703x, so we pretend that they don't exist.

               I've never seen traces with those data structures before, either... */

            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);

            proto_tree_add_item(mifare_tree, hf_mifare_payload, tvb, 2, -1, ENC_NA);
            break;

        case TRANSFER:
            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Transfer");
            break;

        case DECREMENT:
            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(mifare_tree, hf_mifare_operand, tvb, 2, 4, ENC_BIG_ENDIAN);
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Decrement");
            break;

        case INCREMENT:
            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(mifare_tree, hf_mifare_operand, tvb, 2, 4, ENC_BIG_ENDIAN);
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Increment");
            break;

        case RESTORE:
            proto_tree_add_item(mifare_tree, hf_mifare_block_address, tvb, 1, 1, ENC_BIG_ENDIAN);
            proto_tree_add_item(mifare_tree, hf_mifare_operand, tvb, 2, 4, ENC_BIG_ENDIAN);
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Restore");
            break;

        default:
            col_append_sep_str(pinfo->cinfo, COL_INFO, NULL, "Unknown");
            break;
    }
    return tvb_captured_length(tvb);
}

void
proto_register_mifare(void)
{
    static hf_register_info hf[] = {

        {&hf_mifare_command,
         { "Command", "mifare.cmd", FT_UINT8, BASE_HEX,
           VALS(hf_mifare_commands), 0x0, NULL, HFILL }},
        {&hf_mifare_block_address,
         { "Block Address", "mifare.block.addr", FT_UINT8, BASE_DEC,
           NULL, 0x0, NULL, HFILL }},
        {&hf_mifare_key_a,
         { "Key A", "mifare.key.a", FT_UINT64, BASE_HEX,
           NULL, 0x0, NULL, HFILL }},
        {&hf_mifare_key_b,
         { "Key B", "mifare.key.b", FT_UINT64, BASE_HEX,
           NULL, 0x0, NULL, HFILL }},
        {&hf_mifare_uid,
         { "UID", "mifare.uid", FT_UINT32, BASE_HEX,
           NULL, 0x0, NULL, HFILL }},
        {&hf_mifare_operand,
         { "Operand", "mifare.operand", FT_INT32, BASE_DEC,
           NULL, 0x0, NULL, HFILL }},
        {&hf_mifare_payload,
         { "Payload", "mifare.payload", FT_BYTES, BASE_NONE,
           NULL, 0x0, NULL, HFILL }}
    };

    static int *ett[] = {
        &ett_mifare
    };

    proto_mifare = proto_register_protocol("NXP MiFare", "MiFare", "mifare");
    proto_register_field_array(proto_mifare, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

    register_dissector("mifare", dissect_mifare, proto_mifare);
}

/*
 * Editor modelines - https://www.wireshark.org/tools/modelines.html
 *
 * Local variables:
 * c-basic-offset: 4
 * tab-width: 8
 * indent-tabs-mode: nil
 * End:
 *
 * ex: set shiftwidth=4 tabstop=8 expandtab:
 * :indentSize=4:tabSize=8:noTabs=true:
 */
