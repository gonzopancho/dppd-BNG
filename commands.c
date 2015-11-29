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

#include <rte_table_hash.h>

#include "commands.h"
#include "log.h"
#include "tx_worker.h"
#include "dppd_args.h"
#include "hash_utils.h"
#include "dppd_cfg.h"
#include "dppd_port_cfg.h"
#include "defines.h"
#include "handle_qos.h"
#include "thread_qos.h"
#include "handle_qinq_encap4.h"

void start_core_all(void)
{
	uint32_t cores[RTE_MAX_LCORE];
	uint32_t lcore_id;
	char tmp[256];
	int cnt = 0;

	dppd_core_to_str(tmp, sizeof(tmp), 0);
	plog_info("Starting cores: %s\n", tmp);

	lcore_id = -1;
	while (dppd_core_next(&lcore_id, 0) == 0) {
		cores[cnt++] = lcore_id;
	}
	start_cores(cores, cnt);
}

void stop_core_all(void)
{
	uint32_t cores[RTE_MAX_LCORE];
	uint32_t lcore_id;
	char tmp[256];
	int cnt = 0;

	dppd_core_to_str(tmp, sizeof(tmp), 0);
	plog_info("Stopping cores: %s\n", tmp);

	lcore_id = -1;
	while (dppd_core_next(&lcore_id, 0) == 0) {
		cores[cnt++] = lcore_id;
	}

	stop_cores(cores, cnt);
}

void start_cores(uint32_t *cores, int count)
{
	for (int i = 0; i < count; ++i) {
		if (!dppd_core_active(cores[i], 0)) {
			plog_warn("Can't start core %u: core is not active\n", cores[i]);
		}
		else if (rte_eal_get_lcore_state(cores[i]) != RUNNING) {
			plog_info("Starting core %u\n", cores[i]);
			lconf_set_terminated(&lcore_cfg[cores[i]], 0);
			rte_eal_remote_launch(dppd_work_thread, NULL, cores[i]);
		}
		else {
			plog_warn("Core %u is already running\n", cores[i]);
		}
	}
}

void stop_cores(uint32_t *cores, int count)
{
	for (int i = 0; i < count; ++i) {
		if (!dppd_core_active(cores[i], 0)) {
			plog_warn("Can't stop core %u: core is not active\n", cores[i]);
		} else
			lconf_set_terminated(&lcore_cfg[cores[i]], 1);
	}

	for (int i = 0; i < count; ++i) {
		if (dppd_core_active(cores[i], 0)) {
			if ((rte_eal_get_lcore_state(cores[i]) == RUNNING) ||
			(rte_eal_get_lcore_state(cores[i]) == FINISHED)) {
				plog_info("stopping core %u...", cores[i]);
				rte_eal_wait_lcore(cores[i]);
				plog_info(" OK\n");
			}
			else {
				plog_info("core %u in state %d\n", cores[i], rte_eal_get_lcore_state(cores[i]));
			}
		}
	}

}

void cmd_mem_layout(void)
{
	const struct rte_memseg* memseg = rte_eal_get_physmem_layout();

	for (uint32_t i = 0; i < RTE_MAX_MEMSEG; i++) {
		if (memseg[i].addr == NULL)
			break;

		const char *sz_str;
		switch (memseg[i].hugepage_sz >> 20) {
		case 2:
			sz_str = "2MB";
			break;
		case 1024:
			sz_str = "1GB";
			break;
		default:
			sz_str = "??";
		}

		plog_info("Segment %u: [%#lx-%#lx] at %p using %zu pages of %s\n",
			i,
			memseg[i].phys_addr,
			memseg[i].phys_addr + memseg[i].len,
			memseg[i].addr,
			memseg[i].len/memseg[i].hugepage_sz, sz_str);
	}
}

void cmd_dump(uint8_t lcore_id, uint8_t task_id, uint32_t nb_packets)
{
	plog_info("dump %u %u %u\n", lcore_id, task_id, nb_packets);
	if (lcore_id > RTE_MAX_LCORE) {
		plog_warn("core_id to high, maximum allowed is: %u\n", RTE_MAX_LCORE);
	}
	else if (task_id >= lcore_cfg[lcore_id].nb_tasks) {
		plog_warn("task_id to high, should be in [0, %u]\n", lcore_cfg[lcore_id].nb_tasks - 1);
	}
	else {
		rte_atomic32_set(&lcore_cfg[lcore_id].task[task_id]->aux->task_dump.n_print, nb_packets);
		lconf_set_dump_req(&lcore_cfg[lcore_id], 1);
	}
}

void cmd_rx_distr_start(uint32_t lcore_id)
{
	lconf_set_rx_distr(&lcore_cfg[lcore_id], 1);
}

void cmd_rx_distr_stop(uint32_t lcore_id)
{
	lconf_set_rx_distr_stop(&lcore_cfg[lcore_id], 1);
}

void cmd_rx_distr_rst(uint32_t lcore_id)
{
	lconf_set_rx_distr_rst(&lcore_cfg[lcore_id], 1);
}

void cmd_rx_distr_show(uint32_t lcore_id)
{
	for (uint32_t i = 0; i < lcore_cfg[lcore_id].nb_tasks; ++i) {
		struct task_base *t = lcore_cfg[lcore_id].task[i];
		plog_info("t[%u]: ", i);
		for (uint32_t j = 0; j < sizeof(t->aux->rx_bucket)/sizeof(t->aux->rx_bucket[0]); ++j) {
			plog_info("%u ", t->aux->rx_bucket[j]);
		}
		plog_info("\n");
	}
}

void cmd_ringinfo_all(void)
{
	struct lcore_cfg *lconf;
	uint32_t lcore_id = -1;

	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			cmd_ringinfo(lcore_id, task_id);
		}
	}
}

void cmd_ringinfo(uint8_t lcore_id, uint8_t task_id)
{
	struct lcore_cfg *lconf;
	struct rte_ring *ring;
	struct task_args* targ;
	uint32_t count;

	if (!dppd_core_active(lcore_id, 0)) {
		plog_info("lcore %u is not active\n", lcore_id);
		return;
	}
	lconf = &lcore_cfg[lcore_id];
	if (task_id >= lconf->nb_tasks) {
		plog_warn("Invalid task index %u: lcore %u has %u tasks\n", task_id, lcore_id, lconf->nb_tasks);
		return;
	}

	targ = &lconf->targs[task_id];
	plog_info("Core %u task %u: %u rings\n", lcore_id, task_id, targ->nb_rxrings);
	for (uint8_t i = 0; i < targ->nb_rxrings; ++i) {
		ring = targ->rx_rings[i];
		count = ring->prod.mask + 1;
		plog_info("\tRing %u:\n", i);
		plog_info("\t\tFlags: %s,%s\n", ring->flags & RING_F_SP_ENQ? "sp":"mp", ring->flags & RING_F_SC_DEQ? "sc":"mc");
		plog_info("\t\tMemory size: %zu bytes\n", rte_ring_get_memsize(count));
		plog_info("\t\tOccupied: %u/%u\n", rte_ring_count(ring), count);
	}
}

static int port_is_valid(uint8_t port_id)
{
	if (port_id > DPPD_MAX_PORTS) {
		plog_info("requested port is higher than highest supported port ID (%u)\n", DPPD_MAX_PORTS);
		return 0;
	}

	struct dppd_port_cfg* port_cfg = &dppd_port_cfg[port_id];
	if (!port_cfg->active) {
		plog_info("Port %u is not active\n", port_id);
		return 0;
	}
	return 1;
}

void cmd_port_up(uint8_t port_id)
{
	int err;

	if (!port_is_valid(port_id)) {
		return ;
	}

	if ((err = rte_eth_dev_set_link_up(port_id)) == 0) {
		plog_info("Bringing port %d up\n", port_id);
	}
	else {
		plog_warn("Failed to bring port %d up with error %d\n", port_id, err);
	}
}

void cmd_port_down(uint8_t port_id)
{
	int err;

	if (!port_is_valid(port_id)) {
		return ;
	}

	if ((err = rte_eth_dev_set_link_down(port_id)) == 0) {
		plog_info("Bringing port %d down\n", port_id);
	}
	else {
		plog_warn("Failed to bring port %d down with error %d\n", port_id, err);
	}
}

void cmd_portinfo(uint8_t port_id)
{
	if (!port_is_valid(port_id)) {
		return ;
	}
	struct dppd_port_cfg* port_cfg = &dppd_port_cfg[port_id];

	plog_info("Port info for port %u\n", port_id);
	plog_info("\tName: %s\n", port_cfg->name);
	plog_info("\tDriver: %s\n", port_cfg->driver_name);
	plog_info("\tMac address: "MAC_BYTES_FMT"\n", MAC_BYTES(port_cfg->eth_addr.addr_bytes));
	plog_info("\tLink speed: %u Mbps\n", port_cfg->link_speed);
	plog_info("\tLink status: %s\n", port_cfg->link_up? "up" : "down");
	plog_info("\tSocket: %u\n", port_cfg->socket);
	plog_info("\tPromiscuous: %s\n", port_cfg->promiscuous? "yes" : "no");
	plog_info("\tNumber of RX/TX descriptors: %u/%u\n", port_cfg->n_rxd, port_cfg->n_txd);
	plog_info("\tNumber of RX/TX queues: %u/%u (max: %u/%u)\n", port_cfg->n_rxq, port_cfg->n_txq, port_cfg->max_rxq, port_cfg->max_txq);
	plog_info("\tMemory pools:\n");
	for (uint8_t i = 0; i < 32; ++i) {
		if (port_cfg->pool[i]) {
			plog_info("\t\tname: %s (%p)\n", port_cfg->pool[i]->name, port_cfg->pool[i]);
		}
	}
}

void cmd_thread_info(uint8_t lcore_id, uint8_t task_id)
{
	plog_info("thread_info %u %u \n", lcore_id, task_id);
	if (lcore_id > RTE_MAX_LCORE) {
		plog_warn("core_id to high, maximum allowed is: %u\n", RTE_MAX_LCORE);
	}
	if (!dppd_core_active(lcore_id, 0)) {
		plog_warn("lcore %u is not active\n", lcore_id);
		return;
	}
	if (task_id >= lcore_cfg[lcore_id].nb_tasks) {
		plog_warn("task_id to high, should be in [0, %u]\n", lcore_cfg[lcore_id].nb_tasks - 1);
		return;
	}
	if (lcore_cfg[lcore_id].thread_x == thread_qos) {
		struct task_qos *task;
		if (task_id > 0) {
			plog_warn("for QoS only one port per core\n");
			return;
		}
		task = (struct task_qos *)(lcore_cfg[lcore_id].task[task_id]);
		plog_info("core %d, task %d: %d mbufs stored in QoS\n", lcore_id, task_id, task->nb_buffered_pkts);
#ifdef ENABLE_EXTRA_USER_STATISTICS
	}
	else if (lcore_cfg[lcore_id].targs[task_id].mode == QINQ_ENCAP4) {
		struct task_qinq_encap4 *task;
		task = (struct task_qinq_encap4 *)(lcore_cfg[lcore_id].task[task_id]);
		for (int i=0;i<task->n_users;i++) {
			if (task->stats_per_user[i])
				plog_info("User %d: %d packets\n", i, task->stats_per_user[i]);
		}
#endif
	}
	else {
		// Only QoS thread info so far
		plog_info("core %d, task %d: not a qos core(%p != %p)\n", lcore_id, task_id, lcore_cfg[lcore_id].thread_x,  thread_qos);
	}
}
