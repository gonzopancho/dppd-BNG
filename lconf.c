/*
  Copyright(c) 2010-2015 Intel Corporation.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "lconf.h"
#include "rx_pkt.h"
#include "tx_pkt.h"

int lconf_do_flags(struct lcore_cfg *lconf)
{
	if (lconf_is_terminated(lconf)) {
		return 1;
	}
	if (lconf_is_dump_req(lconf)) {
		lconf_set_dump_req(lconf, 0);
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_base *t = lconf->task[task_id];

			uint32_t val = rte_atomic32_read(&t->aux->task_dump.n_print);
			if (val) {
				if (t->aux->rx_pkt_orig)
					t->rx_pkt = t->aux->rx_pkt_orig;
				if (t->aux->tx_pkt_orig)
					t->tx_pkt = t->aux->tx_pkt_orig;

				if (t->rx_pkt != rx_pkt_dummy) {
					t->aux->rx_pkt_orig = t->rx_pkt;
					t->rx_pkt = rx_pkt_dump;
				}
				t->aux->tx_pkt_orig = t->tx_pkt;
				t->tx_pkt = tx_pkt_dump;
			}
		}
	}
	if (lconf_is_rx_distr(lconf)) {
		lconf_set_rx_distr(lconf, 0);
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_base *t = lconf->task[task_id];

			t->aux->rx_pkt_orig = t->rx_pkt;
			t->rx_pkt = rx_pkt_distr;
			memset(t->aux->rx_bucket, 0, sizeof(t->aux->rx_bucket));
		}
	}
	if (lconf_is_rx_distr_rst(lconf)) {
		lconf_set_rx_distr_rst(lconf, 0);
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_base *t = lconf->task[task_id];

			memset(t->aux->rx_bucket, 0, sizeof(t->aux->rx_bucket));
		}
	}
	if (lconf_is_rx_distr_stop(lconf)) {
		lconf_set_rx_distr_stop(lconf, 0);
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_base *t = lconf->task[task_id];
			if (t->aux->rx_pkt_orig) {
				t->rx_pkt = t->aux->rx_pkt_orig;
				t->aux->rx_pkt_orig = NULL;
			}
		}
	}
	return 0;
}
