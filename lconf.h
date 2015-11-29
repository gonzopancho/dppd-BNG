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

#ifndef _LCONF_H_
#define _LCONF_H_

#include "task_init.h"
#include "stats.h"

struct lcore_cfg {
	struct task_base	*task[MAX_TASKS_PER_CORE];
	uint8_t			nb_tasks;		// Used by ALL
	void (*flush_queues[MAX_TASKS_PER_CORE])(struct task_base *tbase);

	void (*period_func)(void* data);
	void*                   period_data;
	uint64_t                period_timeout;       // call periodic_func after periodic_timeout cycles

	uint64_t                ctrl_timeout;
	void (*ctrl_func_m[MAX_TASKS_PER_CORE])(struct task_base *tbase, void **data, uint16_t n_msgs);
	struct rte_ring         *ctrl_rings_m[MAX_TASKS_PER_CORE];

	void (*ctrl_func_p[MAX_TASKS_PER_CORE])(struct task_base *tbase, struct rte_mbuf **mbufs, uint16_t n_pkts);
	struct rte_ring         *ctrl_rings_p[MAX_TASKS_PER_CORE];

	// Following variables are not accessed in main loop
	uint32_t		flags;			// PCFG_* flags below
	uint8_t			active_task;
	uint8_t			id;
	char			name[MAX_NAME_SIZE];
	struct task_args        targs[MAX_TASKS_PER_CORE];
	int (*thread_x)(struct lcore_cfg* lconf);
} __rte_cache_aligned;

static inline void lconf_flush_all_stats(struct lcore_cfg *lconf)
{
	for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
		task_stats_flush(&lconf->task[task_id]->aux->stats);
	}
}

/* This function is only run on low load (when no bulk was sent within
   last drain_timeout (16kpps if DRAIN_TIMEOUT = 2 ms) */
static inline void lconf_flush_all_queues(struct lcore_cfg *lconf)
{
	struct task_base *task;
	uint8_t n_tasks = lconf->nb_tasks;

	for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
		task = lconf->task[task_id];
		if (!(task->flags & FLAG_TX_FLUSH) || (task->flags & FLAG_NEVER_FLUSH)) {
			task->flags |= FLAG_TX_FLUSH;
			continue;
		}
		lconf->flush_queues[task_id](task);
	}
}

/* flags for lcore_cfg */
#define PCFG_RX_DISTR_STOP	0x00000001 /* Stop collecting data for the distribution */
#define PCFG_RX_DISTR_RST	0x00000002 /* Reset data collection */
#define PCFG_RX_DISTR		0x00000004 /* Create a distribution of the number of packets returned during RX */
#define PCFG_TERMINATE		0x00000008 /* thread terminate flag */
#define PCFG_DUMP_REQ		0x00000010 /* dumping packets requested, will need to enable dumping functions */

static inline int lconf_flags_empty(struct lcore_cfg *lconf)
{
	return *(volatile uint32_t *)&lconf->flags == 0;
}

static inline void lconf_flags_clear(struct lcore_cfg *lconf)
{
	*(volatile uint32_t *)&lconf->flags = 0;
}

static inline int lconf_is_terminated(struct lcore_cfg *lconf)
{
	return ((*(volatile uint32_t *)&lconf->flags) & PCFG_TERMINATE);
}

static inline void lconf_set_terminated(struct lcore_cfg *lconf, int val)
{
	if (val)
		lconf->flags |= PCFG_TERMINATE;
	else
		lconf->flags &= ~PCFG_TERMINATE;
}

static inline int lconf_is_dump_req(struct lcore_cfg *lconf)
{
	return ((*(volatile uint32_t *)&lconf->flags) & PCFG_DUMP_REQ);
}

static inline void lconf_set_dump_req(struct lcore_cfg *lconf, int val)
{
	if (val)
		lconf->flags |= PCFG_DUMP_REQ;
	else
		lconf->flags &= ~PCFG_DUMP_REQ;
}

static inline int lconf_is_rx_distr(struct lcore_cfg *lconf)
{
	return ((*(volatile uint32_t *)&lconf->flags) & PCFG_RX_DISTR);
}

static inline void lconf_set_rx_distr(struct lcore_cfg *lconf, int val)
{
	if (val)
		lconf->flags |= PCFG_RX_DISTR;
	else
		lconf->flags &= ~PCFG_RX_DISTR;
}

static inline int lconf_is_rx_distr_stop(struct lcore_cfg *lconf)
{
	return ((*(volatile uint32_t *)&lconf->flags) & PCFG_RX_DISTR_STOP);
}

static inline void lconf_set_rx_distr_stop(struct lcore_cfg *lconf, int val)
{
	if (val)
		lconf->flags |= PCFG_RX_DISTR_STOP;
	else
		lconf->flags &= ~PCFG_RX_DISTR_STOP;
}

static inline int lconf_is_rx_distr_rst(struct lcore_cfg *lconf)
{
	return ((*(volatile uint32_t *)&lconf->flags) & PCFG_RX_DISTR_RST);
}

static inline void lconf_set_rx_distr_rst(struct lcore_cfg *lconf, int val)
{
	if (val)
		lconf->flags |= PCFG_RX_DISTR_RST;
	else
		lconf->flags &= ~PCFG_RX_DISTR_RST;
}

/* Returns non-zero when terminate has been requested */
int lconf_do_flags(struct lcore_cfg *lconf);

#endif /* _LCONF_H_ */
