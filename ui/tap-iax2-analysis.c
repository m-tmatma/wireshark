/* tap-iax2-analysis.c
 * IAX2 analysis addition for Wireshark
 *
 * based on rtp_analysis.c
 * Copyright 2003, Alcatel Business Systems
 * By Lars Ruoff <lars.ruoff@gmx.net>
 *
 * based on tap_rtp.c
 * Copyright 2003, Iskratel, Ltd, Kranj
 * By Miha Jemec <m.jemec@iskratel.si>
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "config.h"

#include <math.h>

#include <glib.h>

#include <epan/dissectors/packet-iax2.h>

#include "tap-iax2-analysis.h"

/****************************************************************************/
/* This comes from tap-rtp-common.c */
/****************************************************************************/

void
iax2_packet_analyse(tap_iax2_stat_t *statinfo,
        packet_info *pinfo,
        const struct _iax2_info_t *iax2info)
{
    double current_time;
    double current_jitter;
    double current_diff;

    statinfo->flags = 0;
    /* check payload type */
    if (iax2info->ftype == AST_FRAME_VOICE) {
        if (iax2info->csub != statinfo->pt) {
            statinfo->flags |= STAT_FLAG_PT_CHANGE;
        }
        statinfo->pt = iax2info->csub;
    }

    /* store the current time and calculate the current jitter */
    current_time = nstime_to_sec(&pinfo->rel_ts);
    current_diff = fabs (current_time - statinfo->time - (((double)iax2info->timestamp - (double)statinfo->timestamp)/1000));
    current_jitter = statinfo->jitter + ( current_diff - statinfo->jitter)/16;
    statinfo->delta = current_time - (statinfo->time);
    statinfo->jitter = current_jitter;
    statinfo->diff = current_diff;

    /* calculate the BW in Kbps adding the IP+IAX2 header to the RTP -> 20bytes(IP)+ 4bytes(Mini) = 24bytes */
    statinfo->bw_history[statinfo->bw_index].bytes = iax2info->payload_len + 24;
    statinfo->bw_history[statinfo->bw_index].time = current_time;
    /* check if there are more than 1sec in the history buffer to calculate BW in bps. If so, remove those for the calculation */
    while ((statinfo->bw_history[statinfo->bw_start_index].time+1) < current_time) {
        statinfo->total_bytes -= statinfo->bw_history[statinfo->bw_start_index].bytes;
        statinfo->bw_start_index++;
        if (statinfo->bw_start_index == BUFF_BW) {
            statinfo->bw_start_index = 0;
        }
    };
    statinfo->total_bytes += iax2info->payload_len + 24;
    statinfo->bandwidth = (double)(statinfo->total_bytes*8)/1000;
    statinfo->bw_index++;
    if (statinfo->bw_index == BUFF_BW) {
        statinfo->bw_index = 0;
    }

    /*  is this the first packet we got in this direction? */
    if (statinfo->first_packet) {
        statinfo->start_seq_nr = 0;
        statinfo->start_time = current_time;
        statinfo->delta = 0;
        statinfo->jitter = 0;
        statinfo->diff = 0;
        statinfo->flags |= STAT_FLAG_FIRST;
        statinfo->first_packet = false;
    }
    /* is it a regular packet? */
    if (!(statinfo->flags & STAT_FLAG_FIRST)
            && !(statinfo->flags & STAT_FLAG_MARKER)
            && !(statinfo->flags & STAT_FLAG_PT_CN)
            && !(statinfo->flags & STAT_FLAG_WRONG_TIMESTAMP)
            && !(statinfo->flags & STAT_FLAG_FOLLOW_PT_CN)) {
        /* include it in maximum delta calculation */
        if (statinfo->delta > statinfo->max_delta) {
            statinfo->max_delta = statinfo->delta;
            statinfo->max_nr = pinfo->num;
        }
        /* maximum and mean jitter calculation */
        if (statinfo->jitter > statinfo->max_jitter) {
            statinfo->max_jitter = statinfo->jitter;
        }
        statinfo->mean_jitter = (statinfo->mean_jitter*statinfo->total_nr + current_jitter) / (statinfo->total_nr+1);
    }
    /* regular payload change? (CN ignored) */
    if (!(statinfo->flags & STAT_FLAG_FIRST)
            && !(statinfo->flags & STAT_FLAG_PT_CN)) {
        if ((statinfo->pt != statinfo->reg_pt)
                && (statinfo->reg_pt != PT_UNDEFINED)) {
            statinfo->flags |= STAT_FLAG_REG_PT_CHANGE;
        }
    }

    /* set regular payload*/
    if (!(statinfo->flags & STAT_FLAG_PT_CN)) {
        statinfo->reg_pt = statinfo->pt;
    }

    /* TODO: lost packets / duplicated:  we should infer this from timestamp... */
    statinfo->time = current_time;
    statinfo->timestamp = iax2info->timestamp; /* millisecs */
    statinfo->stop_seq_nr = 0;
    statinfo->total_nr++;

    return;
}
