/*
* Author: Christian Huitema
* Copyright (c) 2022, Private Octopus, Inc.
* All rights reserved.
*
* Permission to use, copy, modify, and distribute this software for any
* purpose with or without fee is hereby granted, provided that the above
* copyright notice and this permission notice appear in all copies.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
* ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
* WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
* DISCLAIMED. IN NO EVENT SHALL Private Octopus, Inc. BE LIABLE FOR ANY
* DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
* (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
* LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
* ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
* (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
* SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdlib.h>
#include <string.h>
#include "picoquic.h"
#include "picoquic_utils.h"
#include "picosocks.h"
#include "picoquic_internal.h"
#include "picoquictest_internal.h"
#include "tls_api.h"
#include "picoquic_qlog.h"
#include "picoquic_newreno.h"
#include "picoquic_cubic.h"
#include "picoquic_bbr.h"
#include "picoquic_bbr1.h"
#include "picoquic_prague.h"
#include "c4.h"

#include "picoquictest_dualq.h"

static test_api_stream_desc_t test_scenario_l4s[] = {
    { 4, 0, 257, 1000000 },
    { 8, 4, 257, 1000000 },
    { 12, 8, 257, 1000000 },
    { 16, 12, 257, 1000000 }
};


static int l4s_congestion_test(picoquic_congestion_algorithm_t* ccalgo, int do_l4s, uint64_t max_completion_time, uint64_t max_losses, uint64_t max_rttvar,
    size_t nb_link_states, test_vary_link_spec_t* link_state)
{
    uint64_t simulated_time = 0;
    uint64_t queue_delay_max = 20000;
    uint64_t l4s_max = max_rttvar;
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    picoquic_connection_id_t initial_cid = { {0x45, 0xcc, 0, 0, 0, 0, 0, 0}, 8 };
    int ret;

    initial_cid.id[2] = ccalgo->congestion_algorithm_number;
    initial_cid.id[3] = (uint8_t)nb_link_states;

    for (size_t i = 0; i < nb_link_states; i++) {
        if (2 * link_state->microsec_latency > queue_delay_max) {
            queue_delay_max = 2 * link_state->microsec_latency;
            /* l4s_max = link_state->microsec_latency / 4; */
        }
    }

    ret = tls_api_init_ctx_ex(&test_ctx, PICOQUIC_INTERNAL_TEST_VERSION_1, PICOQUIC_TEST_SNI, PICOQUIC_TEST_ALPN, &simulated_time, NULL, NULL, 0, 1, 0, &initial_cid);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }
    else {
        test_ctx->c_to_s_link->queue_delay_max = queue_delay_max;
        test_ctx->s_to_c_link->queue_delay_max = queue_delay_max;
    }

    /* Set the congestion algorithm to specified value.
     * Initialize L4S behavior.
     * Request a packet trace */
    if (ret == 0) {
        picoquic_set_default_congestion_algorithm(test_ctx->qclient, ccalgo);
        picoquic_set_default_congestion_algorithm(test_ctx->qserver, ccalgo);
        picoquic_set_congestion_algorithm(test_ctx->cnx_client, ccalgo);


        if (do_l4s) {
            test_ctx->ecn_support = 1;
            test_ctx->packet_ecn_default = PICOQUIC_ECN_ECT_1;
            if ((ret = dualq_configure(test_ctx->c_to_s_link, l4s_max)) == 0) {
                ret = dualq_configure(test_ctx->s_to_c_link, l4s_max);
            }
        }

        if (ret == 0) {
            picoquic_set_qlog(test_ctx->qserver, ".");

            ret = tls_api_one_scenario_body_ex(test_ctx, &simulated_time,
                test_scenario_l4s, sizeof(test_scenario_l4s), 0, 0, 0, queue_delay_max, max_completion_time, nb_link_states, link_state);
        }
    }

    /* Verify that L4S ECN feedback was received properly */
    /* verify that the delay meets target requirements */
    /* Verify that the losses are as expected, "low loss" part of L4S */
    if (ret == 0) {
        if (test_ctx->cnx_server == NULL) {
            DBG_PRINTF("%s", "Cannot assess server connection");
            ret = -1;
        }
        else if (test_ctx->cnx_server->nb_retransmission_total > max_losses) {
            DBG_PRINTF("Noted %" PRIu64 " losses, expected maximum %" PRIu64, test_ctx->cnx_server->nb_retransmission_total, max_losses);
            ret = -1;
        }
        else if (test_ctx->cnx_server->path[0]->rtt_variant > max_rttvar) {
            DBG_PRINTF("RTT variant %" PRIu64 ", expected maximum %" PRIu64, test_ctx->cnx_server->path[0]->rtt_variant, max_rttvar);
            ret = -1;
        }
    }

    /* Free the resource, which will close the log file.
     */

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
        test_ctx = NULL;
    }

    return ret;
}

/* This test is used for reference. Perf is bad, because each CE mark is
 * a congestion mark.
 */
int l4s_reno_test(void)
{
    picoquic_congestion_algorithm_t* ccalgo = picoquic_newreno_algorithm;

    int ret = l4s_congestion_test(ccalgo, 1, 5600000, 45, 3000, 0, NULL);

    return ret;
}

int l4s_prague_test(void)
{
    picoquic_congestion_algorithm_t* ccalgo = picoquic_prague_algorithm;

    int ret = l4s_congestion_test(ccalgo, 1, 4200000, 9, 4500, 0, NULL);

    return ret;
}

int l4s_bbr_test(void)
{
    picoquic_congestion_algorithm_t* ccalgo = picoquic_bbr_algorithm;

    int ret = l4s_congestion_test(ccalgo, 1, 3800000, 21, 3000, 0, NULL);

    return ret;
}


int l4s_bbr1_test(void)
{
    picoquic_congestion_algorithm_t* ccalgo = picoquic_bbr1_algorithm;

    int ret = l4s_congestion_test(ccalgo, 1, 3600000, 150, 3000, 0, NULL);

    return ret;
}

int l4s_c4_test(void)
{
    picoquic_congestion_algorithm_t* ccalgo = c4_algorithm;

    int ret = l4s_congestion_test(ccalgo, 1, 3650000, 50, 3000, 0, NULL);

    return ret;
}

test_vary_link_spec_t l4s_link_updown[] = {
    { 1000000, 10000000, 10000000, 10000},
    { 2000000,  2000000,  2000000, 10000},
    { 8000000, 10000000, 10000000, 10000},
};

size_t nb_l4s_link_updown = sizeof(l4s_link_updown) / sizeof(test_vary_link_spec_t);

int l4s_prague_updown_test(void)
{
    picoquic_congestion_algorithm_t* ccalgo = picoquic_prague_algorithm;

    int ret = l4s_congestion_test(ccalgo, 1, 6300000, 55, 6000, nb_l4s_link_updown, l4s_link_updown);

    return ret;
}

int l4s_bbr_updown_test(void)
{
#if defined(_WINDOWS) && !defined(_WINDOWS64)
    return 0;
#else
    picoquic_congestion_algorithm_t* ccalgo = picoquic_bbr_algorithm;

    int ret = l4s_congestion_test(ccalgo, 1, 5800000, 69, 3000, nb_l4s_link_updown, l4s_link_updown);

    return ret;
#endif
}

/* One Prague flow sends data_size bytes over a 100 Mbps link, whose L4S queue
 * marks CE with a probability rising from 0 at min_th to 1 at max_th of
 * sojourn time, a step if both are equal. The flow should use most of the
 * link without building a queue beyond max_th.
 */
static int l4s_prague_link_test(uint64_t latency, uint64_t min_th, uint64_t max_th, size_t data_size,
    uint64_t max_completion_time)
{
    picoquic_congestion_algorithm_t* ccalgo = picoquic_prague_algorithm;
    uint64_t simulated_time = 0;
    uint64_t picosec_per_byte = (1000000ull * 8) / 100;
    uint64_t queue_delay_max = 2 * latency + 100000;
    picoquic_connection_id_t initial_cid = { {0x45, 0xcc, 0x9a, 0, 0, 0, 0, 0}, 8 };
    picoquic_test_tls_api_ctx_t* test_ctx = NULL;
    int ret;

    initial_cid.id[3] = (uint8_t)(latency / 1000);
    initial_cid.id[4] = (uint8_t)(min_th / 100);
    initial_cid.id[5] = (uint8_t)(max_th / 100);

    ret = tls_api_one_scenario_init_ex(&test_ctx, &simulated_time, PICOQUIC_INTERNAL_TEST_VERSION_1, NULL, NULL, &initial_cid);

    if (ret == 0 && test_ctx == NULL) {
        ret = -1;
    }

    if (ret == 0) {
        picoquic_set_default_congestion_algorithm(test_ctx->qclient, ccalgo);
        picoquic_set_default_congestion_algorithm(test_ctx->qserver, ccalgo);
        picoquic_set_congestion_algorithm(test_ctx->cnx_client, ccalgo);
        test_ctx->ecn_support = 1;
        test_ctx->packet_ecn_default = PICOQUIC_ECN_ECT_1;
        test_ctx->c_to_s_link->microsec_latency = latency;
        test_ctx->c_to_s_link->picosec_per_byte = picosec_per_byte;
        test_ctx->c_to_s_link->queue_delay_max = queue_delay_max;
        test_ctx->s_to_c_link->microsec_latency = latency;
        test_ctx->s_to_c_link->picosec_per_byte = picosec_per_byte;
        test_ctx->stream0_flow_release = 1;
        test_ctx->immediate_exit = 1;

        if ((ret = dualq_configure(test_ctx->c_to_s_link, max_th)) == 0) {
            dualq_state_t* dqs = (dualq_state_t*)test_ctx->c_to_s_link->aqm_state;

            dqs->minTh = min_th;
            dqs->maxTh = max_th;
            dqs->range = max_th - min_th;
        }
    }

    if (ret == 0) {
        ret = tls_api_one_scenario_body(test_ctx, &simulated_time, NULL, 0, data_size, 0, 0,
            queue_delay_max, max_completion_time);
    }

    if (ret == 0 && test_ctx->cnx_client->path[0]->smoothed_rtt > 2 * latency + max_th) {
        DBG_PRINTF("Smoothed RTT %" PRIu64 ", expected at most %" PRIu64,
            test_ctx->cnx_client->path[0]->smoothed_rtt, 2 * latency + max_th);
        ret = -1;
    }

    if (test_ctx != NULL) {
        tls_api_delete_ctx(test_ctx);
    }

    return ret;
}

/* A 1 ms marking step at 20 ms RTT. Pacing bursts longer than the step get
 * marked even when there is no standing queue.
 */
int l4s_prague_shallow_test(void)
{
    return l4s_prague_link_test(10000, 1000, 1000, 50000000, 5500000);
}

/* The RFC 9332 ramp from 3 to 6 ms at 40 ms RTT. The window must not be
 * reduced a second time for the slow start overshoot, since recovering by
 * one packet per RTT takes seconds at that RTT.
 */
int l4s_prague_long_rtt_test(void)
{
    return l4s_prague_link_test(20000, 3000, 6000, 50000000, 5000000);
}
