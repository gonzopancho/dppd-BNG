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

#include <rte_cycles.h>

#include "dppd_assert.h"
#include "handle_qos.h"
#include "thread_qos.h"
#include "lconf.h"
#include "log.h"

int thread_qos(struct lcore_cfg *lconf)
{
	struct task_qos *task[MAX_TASKS_PER_CORE];
	struct rte_mbuf **mbufs;
	uint64_t cur_tsc = rte_rdtsc();
	uint64_t term_tsc = cur_tsc + TERM_TIMEOUT;
	uint64_t drain_tsc = cur_tsc + DRAIN_TIMEOUT;

	const uint8_t nb_tasks = lconf->nb_tasks;
	for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
		task[task_id] = (struct task_qos *)lconf->task[task_id];
	}

	for (;;) {
		cur_tsc = rte_rdtsc();
		if (cur_tsc > drain_tsc) {
			drain_tsc = cur_tsc + DRAIN_TIMEOUT;
			lconf_flush_all_stats(lconf);

			if (cur_tsc > term_tsc) {
				term_tsc = cur_tsc + TERM_TIMEOUT;
				if (!lconf_flags_empty(lconf) && lconf_do_flags(lconf)) {
					break;
				}
			}
		}

		for (uint8_t task_id = 0; task_id < nb_tasks; ++task_id) {
			struct task_qos *t = task[task_id];
			uint16_t nb_rx = t->base.rx_pkt(&t->base, &mbufs);
			if (likely(nb_rx)) {
				DPPD_ASSERT(nb_rx <= MAX_RING_BURST);
				handle_qos_bulk(&t->base, mbufs, nb_rx);
				// When receiving from port, in vector mode , we might receive max 32 packets
				// This would defeat QoS buffering as always enqueing and dequeuing 32 packets
				if (nb_rx == 32) {
					nb_rx = t->base.rx_pkt(&t->base, &mbufs);
					if (likely(nb_rx)) {
						DPPD_ASSERT(nb_rx <= MAX_RING_BURST);
						handle_qos_bulk(&t->base, mbufs, nb_rx);
					}
				}
			}

			if (t->nb_buffered_pkts) {
				nb_rx = rte_sched_port_dequeue(t->sched_port, mbufs, 32);
				if (likely(nb_rx)) {
					t->nb_buffered_pkts -= nb_rx;
					t->base.tx_pkt(&t->base, mbufs, nb_rx, NULL);
				}
			}
		}
	}
	return 0;
}
