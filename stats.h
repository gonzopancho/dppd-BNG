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

#ifndef _STATS_H_
#define _STATS_H_

#include <rte_atomic.h>
#include "clock.h"

#ifdef BRAS_STATS

struct task_stats {
	uint32_t        _rx_pkt_count;
	uint32_t        _tx_pkt_count;
	uint32_t        _tx_pkt_drop;
	uint32_t        _empty_cycles;
	rte_atomic32_t	rx_pkt_count;	// Not accessed for each packet
	rte_atomic32_t	tx_pkt_count;
	rte_atomic32_t	tx_pkt_drop;
	rte_atomic32_t	empty_cycles;
} __attribute__((packed)) __rte_cache_aligned;

#define TASK_STATS_ADD_IDLE(stats, cycles) do {			\
		(stats)->_empty_cycles += (cycles) + rdtsc_overhead_stats; \
	} while(0)							\

#define TASK_STATS_ADD_TX(stats, ntx) do {	\
		(stats)->_tx_pkt_count += ntx;	\
	} while(0)				\

#define TASK_STATS_ADD_DROP(stats, ntx) do {	\
		(stats)->_tx_pkt_drop += ntx;	\
	} while(0)				\

#define TASK_STATS_ADD_RX(stats, ntx) do {	\
		(stats)->_rx_pkt_count += ntx;	\
	} while (0)				\

#define START_EMPTY_MEASSURE() uint64_t cur_tsc = rte_rdtsc();

static inline void task_stats_flush(struct task_stats *stats)
{
	rte_atomic32_set(&stats->empty_cycles, stats->_empty_cycles);
	rte_atomic32_set(&stats->tx_pkt_count, stats->_tx_pkt_count);
	rte_atomic32_set(&stats->tx_pkt_drop,  stats->_tx_pkt_drop);
	rte_atomic32_set(&stats->rx_pkt_count, stats->_rx_pkt_count);
}

#else
#define TASK_STATS_ADD_IDLE(stats, cycles) {}
#define TASK_STATS_ADD_TX(stats, ntx) {}
#define TASK_STATS_ADD_DROP(stats, ntx) {}
#define TASK_STATS_ADD_RX(stats, ntx) {}
#endif

#endif /* _STATS_H_ */
