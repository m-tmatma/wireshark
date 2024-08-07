/* packet-icap.c
 * Routines for ICAP packet disassembly
 * RFC 3507
 *
 * Srishylam Simharajan simha@netapp.com
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"


#include <epan/packet.h>
#include <epan/strutil.h>
#include "packet-tls.h"

void proto_register_icap(void);
void proto_reg_handoff_icap(void);

typedef enum _icap_type {
    ICAP_OPTIONS,
    ICAP_REQMOD,
    ICAP_RESPMOD,
    ICAP_RESPONSE,
    ICAP_OTHER
} icap_type_t;

static int proto_icap;
static int hf_icap_response;
static int hf_icap_reqmod;
static int hf_icap_respmod;
static int hf_icap_options;
/* static int hf_icap_other; */

static int ett_icap;

static dissector_handle_t http_handle;

#define TCP_PORT_ICAP           1344
static int is_icap_message(const unsigned char *data, int linelen, icap_type_t *type);
static int
dissect_icap(tvbuff_t *tvb, packet_info *pinfo, proto_tree *tree, void* data _U_)
{
    proto_tree   *icap_tree = NULL;
    proto_item   *ti        = NULL;
    proto_item   *hidden_item;
    tvbuff_t     *new_tvb;
    int           offset    = 0;
    const unsigned char *line;
    int           next_offset;
    const unsigned char *linep, *lineend;
    int           linelen;
    unsigned char c;
    icap_type_t   icap_type;
    int           datalen;

    col_set_str(pinfo->cinfo, COL_PROTOCOL, "ICAP");

    /*
     * Put the first line from the buffer into the summary
     * if it's an ICAP header (but leave out the
     * line terminator).
     * Otherwise, just call it a continuation.
     *
     * Note that "tvb_find_line_end()" will return a value that
     * is not longer than what's in the buffer, so the
     * "tvb_get_ptr()" call won't throw an exception.
     */
    linelen = tvb_find_line_end(tvb, offset, -1, &next_offset, false);
    line = tvb_get_ptr(tvb, offset, linelen);
    icap_type = ICAP_OTHER; /* type not known yet */
    if (is_icap_message(line, linelen, &icap_type))
        col_add_str(pinfo->cinfo, COL_INFO,
            format_text(pinfo->pool, line, linelen));
    else
        col_set_str(pinfo->cinfo, COL_INFO, "Continuation");

    if (tree) {
        ti = proto_tree_add_item(tree, proto_icap, tvb, offset, -1,
            ENC_NA);
        icap_tree = proto_item_add_subtree(ti, ett_icap);
    }

    /*
     * Process the packet data, a line at a time.
     */
    icap_type = ICAP_OTHER; /* type not known yet */
    while (tvb_offset_exists(tvb, offset)) {
        bool is_icap = false;
        bool loop_done = false;
        /*
         * Find the end of the line.
         */
        linelen = tvb_find_line_end(tvb, offset, -1, &next_offset,
            false);

        /*
         * Get a buffer that refers to the line.
         */
        line = tvb_get_ptr(tvb, offset, linelen);
        lineend = line + linelen;

        /*
         * find header format
         */
        if (is_icap_message(line, linelen, &icap_type)) {
            goto is_icap_header;
        }

        /*
         * if it looks like a blank line, end of header perhaps?
         */
        if (linelen == 0) {
            goto is_icap_header;
        }

        /*
         * No.  Does it look like a header?
         */
        linep = line;
        loop_done = false;
        while (linep < lineend && (!loop_done)) {
            c = *linep++;

            /*
             * This must be a CHAR, and must not be a CTL, to be part
             * of a token; that means it must be printable ASCII.
             *
             * XXX - what about leading LWS on continuation
             * lines of a header?
             */
            if (!g_ascii_isprint(c)) {
                is_icap = false;
                break;
            }

            switch (c) {

            case '(':
            case ')':
            case '<':
            case '>':
            case '@':
            case ',':
            case ';':
            case '\\':
            case '"':
            case '/':
            case '[':
            case ']':
            case '?':
            case '=':
            case '{':
            case '}':
                /*
                 * It's a separator, so it's not part of a
                 * token, so it's not a field name for the
                 * beginning of a header.
                 *
                 * (We don't have to check for HT; that's
                 * already been ruled out by "iscntrl()".)
                 *
                 * XXX - what about ' '?  HTTP's checks
                 * check for that.
                 */
                is_icap = false;
                loop_done = true;
                break;

            case ':':
                /*
                 * This ends the token; we consider this
                 * to be a header.
                 */
                goto is_icap_header;
            }
        }

        /*
         * We don't consider this part of an ICAP message,
         * so we don't display it.
         * (Yeah, that means we don't display, say, a text/icap
         * page, but you can get that from the data pane.)
         */
        if (!is_icap)
            break;
is_icap_header:
        proto_tree_add_format_text(icap_tree, tvb, offset, next_offset - offset);
        offset = next_offset;
    }

    if (tree) {
        switch (icap_type) {

        case ICAP_OPTIONS:
            hidden_item = proto_tree_add_boolean(icap_tree,
                        hf_icap_options, tvb, 0, 0, 1);
                        proto_item_set_hidden(hidden_item);
            break;

        case ICAP_REQMOD:
            hidden_item = proto_tree_add_boolean(icap_tree,
                        hf_icap_reqmod, tvb, 0, 0, 1);
                        proto_item_set_hidden(hidden_item);
            break;

        case ICAP_RESPMOD:
            hidden_item = proto_tree_add_boolean(icap_tree,
                        hf_icap_respmod, tvb, 0, 0, 1);
                        proto_item_set_hidden(hidden_item);
            break;

        case ICAP_RESPONSE:
            hidden_item = proto_tree_add_boolean(icap_tree,
                        hf_icap_response, tvb, 0, 0, 1);
                        proto_item_set_hidden(hidden_item);
            break;

        case ICAP_OTHER:
        default:
            break;
        }
    }

    datalen = tvb_reported_length_remaining(tvb, offset);
    if (datalen > 0) {
        if(http_handle){
            new_tvb = tvb_new_subset_remaining(tvb, offset);
            call_dissector(http_handle, new_tvb, pinfo, icap_tree);
        }
    }

	return tvb_captured_length(tvb);
}


static int
is_icap_message(const unsigned char *data, int linelen, icap_type_t *type)
{
#define ICAP_COMPARE(string, length, msgtype) {     \
    if (strncmp(data, string, length) == 0) {   \
        if (*type == ICAP_OTHER)        \
            *type = msgtype;        \
        return true;                \
    }                       \
}
    /*
     * From draft-elson-opes-icap-01(72).txt
     */
    if (linelen >= 5) {
        ICAP_COMPARE("ICAP/", 5, ICAP_RESPONSE); /* response */
    }
    if (linelen >= 7) {
        ICAP_COMPARE("REQMOD ", 7, ICAP_REQMOD); /* request mod */
    }
    if (linelen >= 8) {
        ICAP_COMPARE("OPTIONS ", 8, ICAP_OPTIONS); /* options */
        ICAP_COMPARE("RESPMOD ", 8, ICAP_RESPMOD); /* response mod */
    }
    return false;
#undef ICAP_COMPARE
}

void
proto_register_icap(void)
{
    static hf_register_info hf[] = {
        { &hf_icap_response,
          { "Response",     "icap.response",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "true if ICAP response", HFILL }},
        { &hf_icap_reqmod,
          { "Reqmod",       "icap.reqmod",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "true if ICAP reqmod", HFILL }},
        { &hf_icap_respmod,
          { "Respmod",      "icap.respmod",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "true if ICAP respmod", HFILL }},
        { &hf_icap_options,
          { "Options",      "icap.options",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "true if ICAP options", HFILL }},
#if 0
        { &hf_icap_other,
          { "Other",        "icap.other",
            FT_BOOLEAN, BASE_NONE, NULL, 0x0,
            "true if ICAP other", HFILL }},
#endif
    };
    static int *ett[] = {
        &ett_icap,
    };

    proto_icap = proto_register_protocol("Internet Content Adaptation Protocol", "ICAP", "icap");
    proto_register_field_array(proto_icap, hf, array_length(hf));
    proto_register_subtree_array(ett, array_length(ett));

}

void
proto_reg_handoff_icap(void)
{
    dissector_handle_t icap_handle;

    http_handle = find_dissector_add_dependency("http", proto_icap);

    icap_handle = register_dissector("icap", dissect_icap, proto_icap);
    dissector_add_uint_with_preference("tcp.port", TCP_PORT_ICAP, icap_handle);

    /* As ICAPS port is not officially assigned by IANA
     * (de facto standard is 11344), we default to 0
     * to have "decode as" available */
    ssl_dissector_add(0, icap_handle);
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
