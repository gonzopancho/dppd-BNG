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

#include <string.h>
#include <locale.h>

#include <rte_malloc.h>
#include <rte_cycles.h>
#include <rte_eth_ring.h>
#include <rte_atomic.h>
#include <rte_table_hash.h>

#include "run.h"
#include "main.h"
#include "log.h"
#include "quit.h"
#include "route.h"
#include "clock.h"
#include "read_config.h"
#include "defines.h"
#include "version.h"
#include "dppd_args.h"
#include "dppd_assert.h"
#include "dppd_cfg.h"
#include "dppd_shared.h"
#include "dppd_port_cfg.h"
#include "toeplitz.h"
#include "hash_utils.h"
#include "handle_lb_net.h"
#include "dppd_cksum.h"

#if RTE_VERSION < RTE_VERSION_NUM(1,8,0,0)
#define RTE_CACHE_LINE_SIZE CACHE_LINE_SIZE
#endif

rte_atomic32_t lsc;

uint8_t lb_nb_txrings = 0xff;
struct rte_ring *ctrl_rings[RTE_MAX_LCORE*MAX_TASKS_PER_CORE];
/* standard mbuf initialization procedure */
static void dppd_pktmbuf_init(struct rte_mempool *mp, void *opaque_arg, void *_m, unsigned i)
{
	struct rte_mbuf *mbuf = _m;

#if RTE_VERSION >= RTE_VERSION_NUM(1,8,0,0)
	mbuf->tx_offload = CALC_TX_OL(sizeof(struct ether_hdr), sizeof(struct ipv4_hdr));
#else
	mbuf->pkt.vlan_macip.f.l2_len = sizeof(struct ether_hdr);
	mbuf->pkt.vlan_macip.f.l3_len = sizeof(struct ipv4_hdr);
#endif

	rte_pktmbuf_init(mp, opaque_arg, mbuf, i);
}

static void __attribute__((noreturn)) dppd_usage(const char *prgname)
{
	plog_info("\nUsage: %s [-f CONFIG_FILE] [-a|-e] [-s|-i] [-w DEF] [-u] [-t]\n"
		  "\t-f CONFIG_FILE : configuration file to load, ./dppd.cfg by default\n"
		  "\t-l LOG_FILE : log file name, ./dppd.log by default\n"
		  "\t-p : include PID in log file name if default log file is used\n"
		  "\t-a : autostart all cores (by default)\n"
		  "\t-e : don't autostart\n"
		  "\t-s : check configuration file syntax and exit\n"
		  "\t-i : check initialization sequence and exit\n"
		  "\t-u : Listen on UDS /tmp/dppd.sock\n"
		  "\t-t : Listen on TCP port 8474\n"
		  "\t-w : define variable using syntax varname=value\n"
		  "\t     takes precedence over variables defined in CONFIG_FILE\n"
		  , prgname);
	exit(EXIT_FAILURE);
}

/* initialize rte devices and check the number of available ports */
static void init_rte_dev(void)
{
	uint8_t nb_ports, port_id_max, port_id_last;
	struct rte_eth_dev_info dev_info;

	/* get available ports configuration */
	nb_ports = rte_eth_dev_count();
	DPPD_PANIC(nb_ports == 0, "\tError: DPDK could not find any port\n");
	plog_info("\tDPDK has found %u ports\n", nb_ports);

	if (nb_ports > DPPD_MAX_PORTS) {
		plog_warn("\tWarning: I can deal with at most %u ports."
		        " Please update DPPD_MAX_PORTS and recompile.\n", DPPD_MAX_PORTS);

		nb_ports = DPPD_MAX_PORTS;
	}
	port_id_max = nb_ports - 1;
	port_id_last = dppd_last_port_active();
	DPPD_PANIC(port_id_last > port_id_max,
		   "\tError: invalid port(s) specified, last port index active: %d (max index is %d)\n",
		   port_id_last, port_id_max);

	/* Assign ports to DPPD interfaces & Read max RX/TX queues per port */
	for (uint8_t port_id = 0; port_id < nb_ports; ++port_id) {
		/* skip ports that are not enabled */
		if (!dppd_port_cfg[port_id].active) {
			continue;
		}
		plog_info("\tGetting info for rte dev %u\n", port_id);
		rte_eth_dev_info_get(port_id, &dev_info);
		struct dppd_port_cfg* port_cfg = &dppd_port_cfg[port_id];

		port_cfg->max_txq = dev_info.max_tx_queues;
		port_cfg->max_rxq = dev_info.max_rx_queues;
		strncpy(port_cfg->driver_name, dev_info.driver_name, sizeof(port_cfg->driver_name));

		plog_info("\tPort %u : driver='%s' tx_queues=%d rx_queues=%d\n", port_id, port_cfg->driver_name, port_cfg->max_txq, port_cfg->max_rxq);
	}
}

/* Create rte ring-backed devices */
static uint8_t init_rte_ring_dev(void)
{
	uint8_t nb_ring_dev = 0;

	for (uint8_t portid = 0; portid < DPPD_MAX_PORTS; ++portid) {
		struct dppd_port_cfg* port_cfg = &dppd_port_cfg[portid];
		if (port_cfg->rx_ring[0] != '\0') {
			plog_info("\tRing-backed port %u: rx='%s' tx='%s'\n", portid, port_cfg->rx_ring, port_cfg->tx_ring);

			struct rte_ring* rx_ring = rte_ring_lookup(port_cfg->rx_ring);
			DPPD_PANIC(rx_ring == NULL, "Ring %s not found for port %d!\n", port_cfg->rx_ring, portid);
			struct rte_ring* tx_ring = rte_ring_lookup(port_cfg->tx_ring);
			DPPD_PANIC(tx_ring == NULL, "Ring %s not found for port %d!\n", port_cfg->tx_ring, portid);

			int ret = rte_eth_from_rings(port_cfg->name, &rx_ring, 1, &tx_ring, 1, rte_socket_id());
			DPPD_PANIC(ret != 0, "Failed to create eth_dev from rings for port %d\n", portid);

			port_cfg->port_conf.intr_conf.lsc = 0; /* Link state interrupt not supported for ring-backed ports */

			nb_ring_dev++;
		}
	}

	return nb_ring_dev;
}

static void check_consistent_cfg(void)
{
	const struct lcore_cfg *lconf;
	const struct task_args *targ;
	uint32_t lcore_id = -1;

	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			targ = &lconf->targs[task_id];
			DPPD_PANIC((targ->flags & TASK_ARG_RX_RING) && targ->rx_rings[0] == 0 && !targ->tx_opt_ring_task,
			           "Configuration Error - Core %u task %u Receiving from ring, but nobody xmitting to this ring\n", lcore_id, task_id);

			for (uint8_t ring_idx = 0; ring_idx < targ->nb_rxrings; ++ring_idx) {
				plog_info("\tCore %u, task %u, rx_ring[%u] %p\n", lcore_id, task_id, ring_idx, targ->rx_rings[ring_idx]);
			}
			if (targ->nb_txports == 0 && targ->nb_txrings == 0) {
				DPPD_PANIC(!(targ->task_init->flag_features & TASK_NO_TX),
				           "\tCore %u task %u does not transmit or drop packet: no tx_ports and no tx_rings\n", lcore_id, task_id);
			}
		}
	}
}

static void configure_if_tx_queues(struct task_args *targ, uint8_t socket)
{
	uint8_t if_port;

	for (uint8_t i = 0; i < targ->nb_txports; ++i) {
		if_port = targ->tx_port_queue[i].port;

		DPPD_PANIC(if_port == NO_PORT_AVAIL, "port misconfigured, exiting\n");

		DPPD_PANIC(!dppd_port_cfg[if_port].active, "\tPort %u not used, skipping...\n", if_port);

		targ->tx_port_queue[i].queue = dppd_port_cfg[if_port].n_txq;
		dppd_port_cfg[if_port].socket = socket;
		if (dppd_port_cfg[if_port].tx_ring[0] == '\0') {  // Rings-backed port can use single queue
			dppd_port_cfg[if_port].n_txq++;
		}

	}
}

static void configure_if_rx_queues(struct task_args *targ, uint8_t socket)
{
	for (int i=0;i<targ->nb_rxports;i++) {
		uint8_t if_port = targ->rx_ports[i];

		if (if_port == NO_PORT_AVAIL) {
			return;
		}

		DPPD_PANIC(!dppd_port_cfg[if_port].active, "Port %u not used, aborting...\n", if_port);

		targ->rx_queues[i] = dppd_port_cfg[if_port].n_rxq;
		dppd_port_cfg[if_port].pool[targ->rx_queues[i]] = targ->pool;
		dppd_port_cfg[if_port].pool_size[targ->rx_queues[i]] = targ->nb_mbuf - 1;
		dppd_port_cfg[if_port].n_rxq++;
		dppd_port_cfg[if_port].socket = socket;
	}
}

static void configure_if_queues(void)
{
	struct lcore_cfg *lconf;
	uint8_t socket;
	uint32_t lcore_id = -1;

	while(dppd_core_next(&lcore_id, 0) == 0) {
		socket = rte_lcore_to_socket_id(lcore_id);
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
			configure_if_tx_queues(targ, socket);
			configure_if_rx_queues(targ, socket);
		}
	}
}

static char* gen_ring_name(uint32_t idx)
{
	static char retval[] = "XX";
	static const char* ring_names =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"[\\]^_`!\"#$%&'()*+,-./:;<="
		">?@{|}0123456789";

	retval[0] = ring_names[idx % strlen(ring_names)];
	idx /= strlen(ring_names);
	retval[1] = idx ? ring_names[(idx - 1) % strlen(ring_names)] : 0;

	return retval;
}

static int sym_lb_enabled(void)
{
	struct lcore_cfg *lconf;
	struct task_args *starg;
	uint8_t has_lb_qinq = 0, has_lb_net = 0, has_fv_rss = 0;
	uint32_t lcore_id;

	/* The current implementation requires that if symmetric load
	   balancing is used that all load balancers have the same
	   number of workers. This is not a function requirement,
	   rather it is what the current implementation supports. If
	   symmetric load balancing is not used, the number of workers
	   receiving from a specific LB does is not have to be the
	   same as the number of workers receiving from another LB. An
	   example would be use-cases where the upstream and
	   downstream directions are not the same in terms of
	   processing requirements. In this case, it is favorable to
	   use a different number of workers in each direction. */

	lcore_id = -1;
	while(dppd_core_next(&lcore_id, 1) == 0) {
		lconf = &lcore_cfg[lcore_id];

		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			starg = &lconf->targs[task_id];
			if (LB_QINQ == starg->mode)
				has_lb_qinq = 1;
			else if (LB_NET == starg->mode)
				has_lb_net = 1;
		}
	}

	struct dppd_port_cfg *port_cfg;
	uint8_t nb_ports = dppd_last_port_active();

	for (uint8_t port_id = 0; port_id < nb_ports; ++port_id) {
		if (!dppd_port_cfg[port_id].active) {
			continue;
		}

		port_cfg = &dppd_port_cfg[port_id];

		if (port_cfg->n_rxq > 1 && 0 ==
		    strcmp(port_cfg->driver_name, "rte_i40e_pmd")) {
			has_fv_rss = 1;
			break;
		}
	}

	return has_lb_net && (has_lb_qinq || has_fv_rss);
}

static void init_rings(void)
{
	struct lcore_cfg *lconf, *lworker;
	struct task_args *starg, *dtarg;
	struct rte_ring *ring;
	uint32_t n_pkt_rings = 0, n_ctrl_rings = 0, ring_count = 0, n_opt_ring = 0;
	uint32_t lcore_id;
	uint8_t lb_nb_txrings_check;

	lb_nb_txrings_check = sym_lb_enabled();

	lcore_id = -1;
	while(dppd_core_next(&lcore_id, 1) == 0) {
		lconf = &lcore_cfg[lcore_id];
		uint8_t socket = rte_lcore_to_socket_id(lcore_id);
		plog_info("\t*** Initializing rings on core %u ***\n", lcore_id);
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			starg = &lconf->targs[task_id];
			uint8_t tot_nb_txrings = 0;
			for (uint8_t idx = 0; idx < MAX_PROTOCOLS; ++idx) {
				if (!starg->thread_list[idx].active) {
					continue;
				}

				for (uint8_t ring_idx = 0; ring_idx < starg->thread_list[idx].nb_threads; ++ring_idx, ++tot_nb_txrings) {
					DPPD_ASSERT(ring_idx < MAX_WT_PER_LB);
					DPPD_ASSERT(tot_nb_txrings < MAX_RINGS_PER_CORE);

					uint8_t lcore_worker = starg->thread_list[idx].thread_id[ring_idx];
					DPPD_ASSERT(dppd_core_active(lcore_worker));
					lworker = &lcore_cfg[lcore_worker];
					uint8_t dest_task = starg->thread_list[idx].dest_task;

					plog_info("\t\tCreating ring (size: %u) to connect core %u (socket %u) with worker core %u (socket %u) worker %u ...\n",
					        starg->ring_size, lcore_id, socket, lcore_worker, rte_lcore_to_socket_id(lcore_worker), ring_idx);
					/* socket used is the one that the sending core resides on */

					if (starg->thread_list[idx].type) {
						struct rte_ring **dring = NULL;

						if (starg->thread_list[idx].type == CTRL_TYPE_MSG)
							dring = &lworker->ctrl_rings_m[dest_task];
						else if (starg->thread_list[idx].type == CTRL_TYPE_PKT) {
							dring = &lworker->ctrl_rings_p[dest_task];
							starg->flags |= TASK_ARG_CTRL_RINGS_P;
						}

						if (*dring == NULL)
							ring = rte_ring_create(gen_ring_name(ring_count++), starg->ring_size, socket, RING_F_SC_DEQ);
						else
							ring = *dring;
						DPPD_PANIC(ring == NULL, "Cannot create ring to connect I/O core %u with worker core %u\n", lcore_id, lcore_worker);

						starg->tx_rings[tot_nb_txrings] = ring;
						*dring = ring;
						if (lcore_id == dppd_cfg.master) {
							ctrl_rings[lcore_worker*MAX_TASKS_PER_CORE + dest_task] = ring;
						}

						plog_info("\t\tCore %u task %u to -> core %u task %u ctrl_ring %s %p %s\n",
							lcore_id, task_id, lcore_worker, dest_task, starg->thread_list[idx].type == CTRL_TYPE_PKT?
							"pkt" : "msg", ring, ring->name);
						n_ctrl_rings++;
						continue;
					}

					dtarg = &lworker->targs[dest_task];
					lworker->targs[dest_task].worker_thread_id = ring_idx;
					DPPD_ASSERT(dtarg->flags & TASK_ARG_RX_RING);
					DPPD_ASSERT(dest_task < lworker->nb_tasks);
					/* will skip inactive rings */

					/* If all the following conditions are met, the ring can be optimized away. */
					if (starg->lconf->id == dtarg->lconf->id &&
					    starg->nb_txrings == 1 && idx == 0 && dtarg->task &&
					    dtarg->tot_rxrings == 1 && starg->task == dtarg->task - 1) {
						plog_info("\t\tOptimizing away ring on core %u from task %u to task %u\n", dtarg->lconf->id, starg->task, dtarg->task);
						/* No need to set up ws_mbuf. */
						starg->tx_opt_ring = 1;
						/* During init of destination task, the buffer in the
						   source task will be initialized. */
						dtarg->tx_opt_ring_task = starg;
						n_opt_ring++;
						++dtarg->nb_rxrings;
						continue;
					}

					ring = rte_ring_create(gen_ring_name(ring_count++), starg->ring_size, socket, RING_F_SP_ENQ | RING_F_SC_DEQ);
					DPPD_PANIC(ring == NULL, "Cannot create ring to connect I/O core %u with worker core %u\n", lcore_id, lcore_worker);

					starg->tx_rings[tot_nb_txrings] = ring;
					dtarg->rx_rings[dtarg->nb_rxrings] = ring;
					++dtarg->nb_rxrings;
					DPPD_ASSERT(dtarg->nb_rxrings < MAX_RINGS_PER_TASK);
					dtarg->nb_slave_threads = starg->thread_list[idx].nb_threads;
					//if (strcmp(starg->task_init->mode_str, "lbnetwork") == 0) {
					dtarg->lb_friend_core = lcore_id;
					dtarg->lb_friend_task = task_id;
					plog_info("\t\tWorker thread %d has core %d, task %d as a lb friend\n", lcore_worker, lcore_id, task_id);
					plog_info("\t\tCore %u task %u tx_ring[%u] -> core %u task %u rx_ring[%u] %p %s %u WT\n",
					        lcore_id, task_id, ring_idx, lcore_worker, dest_task, dtarg->nb_rxrings, ring, ring->name,
					        dtarg->nb_slave_threads);
					++n_pkt_rings;
				}

				if (lb_nb_txrings_check && (LB_QINQ == starg->mode || LB_NET == starg->mode)) {
					if (lb_nb_txrings == 0xff) {
						lb_nb_txrings = starg->nb_worker_threads;
					}
					else if (lb_nb_txrings != starg->nb_worker_threads) {
						DPPD_PANIC(tot_nb_txrings != 1, "QinQ LB and network LB should have the same number of tx_rings: %u != %u\n", lb_nb_txrings, starg->nb_txrings);
					}
				}
			}
		}
	}

	plog_info("\tInitialized %d rings (%d pkt rings, %d ctrl rings) %d worker_threads\n", ring_count, n_pkt_rings, n_ctrl_rings, lb_nb_txrings);
	if (n_opt_ring) {
		plog_info("\tOptimized away %d rings\n", n_opt_ring);
	}
}

/* Decide if there are cores that need specific configurations to be loaded. */
static uint16_t get_shared_req(uint8_t socket_id, struct dppd_shared* dppd_shared)
{
	struct lcore_cfg* lconf;
	struct task_args *targ;
	uint16_t config_files = 0;
	uint32_t lcore_id = -1;

	while(dppd_core_next(&lcore_id, 0) == 0) {
		if (rte_lcore_to_socket_id(lcore_id) != socket_id) {
			continue;
		}

		lconf = &lcore_cfg[lcore_id];

		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			config_files |= lconf->targs[task_id].task_init->flag_req_data;
			if (lconf->targs[task_id].task_init->flag_req_data) {
				lconf->targs[task_id].dppd_shared = dppd_shared;
			}
		}
	}
	return config_files;
}

static void shuffle_mempool(struct rte_mempool* mempool, uint32_t nb_mbuf)
{
	struct rte_mbuf** pkts = rte_zmalloc_socket(NULL, nb_mbuf*sizeof(struct rte_mbuf*), RTE_CACHE_LINE_SIZE, rte_socket_id());
	uint64_t got = 0;

	while (rte_mempool_get_bulk(mempool, (void**)(pkts + got), 1) == 0)
		++got;

	while (got) {
		int idx;
		do {
			idx = rand() % nb_mbuf - 1;
		} while (pkts[idx] == 0);

		rte_mempool_put_bulk(mempool, (void**)&pkts[idx], 1);
		pkts[idx] = 0;
		--got;
	};
	rte_free(pkts);
}

static void setup_mempools(struct lcore_cfg* lcore_cfg)
{
	struct lcore_cfg *lconf;
	struct task_args *targ;
	char name[64];
	uint32_t lcore_id = -1;

	if (dppd_cfg.flags & UNIQUE_MEMPOOL_PER_SOCKET) {
		struct rte_mempool     *pool[MAX_SOCKETS];
		uint32_t mbuf_count[MAX_SOCKETS] = {0};
		uint32_t nb_cache_mbuf[MAX_SOCKETS] = {0};
		while(dppd_core_next(&lcore_id, 0) == 0) {
			lconf = &lcore_cfg[lcore_id];
			uint8_t socket = rte_lcore_to_socket_id(lcore_id);
			DPPD_ASSERT(socket < MAX_SOCKETS);
			for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
				targ = &lconf->targs[task_id];
				if (targ->rx_ports[0] != NO_PORT_AVAIL) {
					DPPD_ASSERT(targ->nb_mbuf != 0);
					mbuf_count[socket] += targ->nb_mbuf;
					if (nb_cache_mbuf[socket] == 0)
						nb_cache_mbuf[socket] = targ->nb_cache_mbuf;
					else {
						DPPD_PANIC(nb_cache_mbuf[socket] != targ->nb_cache_mbuf,
							   "all mbuf_cahe must have the same size if using a unique mempool per socket\n");
					}
				}
			}
		}
		for (int i=0;i<MAX_SOCKETS;i++) {
			if (mbuf_count[i] != 0) {
				sprintf(name, "socket_%u_pool", i);
				pool[i] = rte_mempool_create(name,
								mbuf_count[i] - 1, MBUF_SIZE,
								nb_cache_mbuf[i],
								sizeof(struct rte_pktmbuf_pool_private),
								rte_pktmbuf_pool_init, NULL,
								dppd_pktmbuf_init, NULL,
								i, 0);
				DPPD_PANIC(pool[i] == NULL, "\t\tError: cannot create mempool for socket %u\n", i);
				plog_info("\t\tMempool %p size = %u * %u cache %u, socket %d\n", pool[i],
					mbuf_count[i], MBUF_SIZE, nb_cache_mbuf[i], i);

				if (dppd_cfg.flags & DSF_SHUFFLE) {
					shuffle_mempool(pool[i], mbuf_count[i]);
				}
			}
		}
		lcore_id = -1;
		while(dppd_core_next(&lcore_id, 0) == 0) {
			lconf = &lcore_cfg[lcore_id];
			uint8_t socket = rte_lcore_to_socket_id(lcore_id);
			for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
				targ = &lconf->targs[task_id];
				if (targ->rx_ports[0] != NO_PORT_AVAIL) {
					/* use this pool for the interface that the core is receiving from */
					/* If one core receives from multiple ports, all the ports use the same mempool */
					targ->pool = pool[socket];
					/* Set the number of mbuf to the number of the unique mempool, so that the used and free work */
					targ->nb_mbuf = mbuf_count[socket];
					plog_info("\t\tMempool %p size = %u * %u cache %u, socket %d\n", targ->pool,
					targ->nb_mbuf, MBUF_SIZE, targ->nb_cache_mbuf, socket);
				}
			}
		}
	}


	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		uint8_t socket = rte_lcore_to_socket_id(lcore_id);
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			targ = &lconf->targs[task_id];

			if (targ->rx_ports[0] != NO_PORT_AVAIL) {
				/* allocate memory pool for packets */
				DPPD_ASSERT(targ->nb_mbuf != 0);
				/* use this pool for the interface that the core is receiving from */
				/* If one core receives from multiple ports, all the ports use the same mempool */
				sprintf(name, "core_%u_port_%u_pool", lcore_id, task_id);
				targ->pool = rte_mempool_create(name,
								targ->nb_mbuf - 1, MBUF_SIZE,
								targ->nb_cache_mbuf,
								sizeof(struct rte_pktmbuf_pool_private),
								rte_pktmbuf_pool_init, NULL,
								dppd_pktmbuf_init, lconf,
								socket, 0);
				DPPD_PANIC(targ->pool == NULL, "\t\tError: cannot create mempool for core %u port %u\n", lcore_id, task_id);
				plog_info("\t\tMempool %p size = %u * %u cache %u, socket %d\n", targ->pool,
					targ->nb_mbuf, MBUF_SIZE, targ->nb_cache_mbuf, socket);
				if (dppd_cfg.flags & DSF_SHUFFLE) {
					shuffle_mempool(targ->pool, targ->nb_mbuf);
				}
			}
		}
	}
}

static void setup_wt_table(uint8_t *wt_table, uint8_t nb_worker_threads, struct qinq_to_gre_lookup_entry *qinq_to_gre_lookup, uint32_t count, __attribute__((unused)) uint32_t req_flags)
{
	/* WIP
	if (req_flags & HASH_BASED_WT_TABLE) {
		for (uint32_t i = 0; i < count; ++i) {
                        uint64_t cvlan = rte_bswap16((qinq_to_gre_lookup[i].cvlan & 0xFF0F));
                        uint64_t svlan = rte_bswap16((qinq_to_gre_lookup[i].svlan & 0xFF0F));
                        uint64_t qinq = rte_bswap64((svlan << 32) | cvlan);
                        queue = hash_crc32(&qinq, 8, 0) % nb_worker_threads;
			wt_table[PKT_TO_LUTQINQ(svlan, cvlan)] = qinq_to_gre_lookup[i].gre_id % nb_worker_threads;


	} else
	*/{
		for (uint32_t i = 0; i < count; ++i) {
			uint16_t svlan = qinq_to_gre_lookup[i].svlan;
			uint16_t cvlan = qinq_to_gre_lookup[i].cvlan;
			wt_table[PKT_TO_LUTQINQ(svlan, cvlan)] = qinq_to_gre_lookup[i].gre_id % nb_worker_threads;
		}
	}
}

static void set_task_lconf(void)
{
	struct lcore_cfg *lconf;
	uint32_t lcore_id = -1;

	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			lconf->targs[task_id].lconf = lconf;
		}
	}
}

static void setup_all_task_structs(void)
{
	struct lcore_cfg *lconf;
	uint32_t lcore_id = -1;

	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			lconf->task[task_id] = init_task_struct(&lconf->targs[task_id]);
		}
	}
}

static void init_port_addr(void)
{
	struct dppd_port_cfg *port_cfg;

	for (uint8_t portid = 0; portid < DPPD_MAX_PORTS; ++portid) {
		if (!dppd_port_cfg[portid].active) {
			continue;
		}
		port_cfg = &dppd_port_cfg[portid];

		switch (port_cfg->type) {
		case DPPD_PORT_MAC_HW:
			rte_eth_macaddr_get(portid, &port_cfg->eth_addr);
			break;
		case DPPD_PORT_MAC_RAND:
			eth_random_addr(port_cfg->eth_addr.addr_bytes);
			break;
		case DPPD_PORT_MAC_SET:
			break;
		}
	}
}

/* Initialize cores and allocate mempools */
static void init_lcores(void)
{
	struct lcore_cfg *lconf = 0;
	struct dppd_shared dppd_shared[MAX_SOCKETS];
	uint32_t lcore_id = -1;

	memset(&dppd_shared, 0, sizeof(struct dppd_shared)*MAX_SOCKETS);

	while(dppd_core_next(&lcore_id, 0) == 0) {
		uint8_t socket = rte_lcore_to_socket_id(lcore_id);
		DPPD_PANIC(socket + 1 > MAX_SOCKETS, "Can't configure core %u (on socket %u). MAX_SOCKET is set to %d\n", lcore_id, socket, MAX_SOCKETS);
	}

	/* need to allocate mempools as the first thing to use the lowest possible address range */
	plog_info("=== Initializing mempools ===\n");
	setup_mempools(lcore_cfg_init);

	lcore_cfg = rte_zmalloc_socket("lcore_cfg_hp", RTE_MAX_LCORE * sizeof(struct lcore_cfg), RTE_CACHE_LINE_SIZE, rte_socket_id());
	DPPD_PANIC(lcore_cfg == NULL, "Could not allocate memory for core control structures\n");
	rte_memcpy(lcore_cfg, lcore_cfg_init, RTE_MAX_LCORE * sizeof(struct lcore_cfg));

	set_task_lconf();

	plog_info("=== Initializing port addresses ===\n");
	init_port_addr();

	plog_info("=== Initializing queue numbers on cores ===\n");
	configure_if_queues();

	plog_info("=== Initializing rings on cores ===\n");
	init_rings();

	for (uint8_t socket_id = 0; socket_id < MAX_SOCKETS; ++socket_id) {
		uint32_t req_flags = get_shared_req(socket_id, &dppd_shared[socket_id]);
		if (req_flags) {
			plog_info("=== Initializing shared data structures on socket %u ===\n", socket_id);
		}
		if (req_flags & REQ_WT_TABLE) {
			dppd_shared[socket_id].worker_thread_table = rte_zmalloc_socket(NULL, 0x1000000, RTE_CACHE_LINE_SIZE, socket_id);
			DPPD_PANIC(dppd_shared[socket_id].worker_thread_table == NULL, "Error creating worker thread table");
		}

		if (req_flags & REQ_GRE_TABLE) {
			plog_info("\tReading GRE config from file %s\n", dppd_cfg.path_gre_cfg);
			plog_info("\tCreating tables for %d worker_threads\n", lb_nb_txrings);
			dppd_shared[socket_id].qinq_to_gre_lookup = read_gre_table_config(dppd_cfg.path_gre_cfg, socket_id, &dppd_shared[socket_id].qinq_to_gre_lookup_count);
			if (req_flags & REQ_WT_TABLE) {
				setup_wt_table(dppd_shared[socket_id].worker_thread_table, lb_nb_txrings, dppd_shared[socket_id].qinq_to_gre_lookup, dppd_shared[socket_id].qinq_to_gre_lookup_count, req_flags);
			}
			DPPD_PANIC(NULL == dppd_shared[socket_id].qinq_to_gre_lookup, "Error allocating memory for mapping between qinq and gre from %s\n", dppd_cfg.path_gre_cfg);
			DPPD_PANIC(0 == dppd_shared[socket_id].qinq_to_gre_lookup_count, "Error reading mapping between qinq and gre from %s\n", dppd_cfg.path_gre_cfg);
		}

		if (req_flags & REQ_USER_TABLE) {
			plog_info("\tReading user table config from file %s\n", dppd_cfg.path_user_cfg);
			dppd_shared[socket_id].user_table = read_user_table_config(dppd_cfg.path_user_cfg, &dppd_shared[socket_id].qinq_to_gre_lookup, &dppd_shared[socket_id].qinq_to_gre_lookup_count, socket_id);
			DPPD_PANIC(NULL == dppd_shared[socket_id].user_table, "Failed to allocate user lookup table\n");
		}

		if (req_flags & REQ_NEXT_HOP) {
			plog_info("\tReading next hop config from %s\n", dppd_cfg.path_next_hop_cfg);
			dppd_shared[socket_id].next_hop = read_next_hop_config(dppd_cfg.path_next_hop_cfg, socket_id);
		}

		if (req_flags & REQ_LPM4) {
			plog_info("\tReading IPv4 config from %s\n", dppd_cfg.path_ipv4_cfg);
			dppd_shared[socket_id].ipv4_lpm = read_lpm4_config(dppd_cfg.path_ipv4_cfg, socket_id);
			DPPD_PANIC(NULL == dppd_shared[socket_id].ipv4_lpm, "Failed to allocate IPv4 LPM\n");
		}

		if (req_flags & REQ_LPM6) {
			plog_info("\tReading IPv6 config from %s\n", dppd_cfg.path_ipv6_cfg);
			read_lpm6_config(dppd_cfg.path_ipv6_cfg, socket_id);
		}

		if (req_flags & REQ_DSCP) {
			plog_info("\tReading dscp config from %s\n", dppd_cfg.path_dscp_cfg);
			dppd_shared[socket_id].dscp = read_dscp_config(dppd_cfg.path_dscp_cfg, socket_id);
		}

		if (req_flags & REQ_CPE_TABLE) {
			plog_info("\tReading cpe table config from %s\n", dppd_cfg.path_cpe_table_cfg);
			read_cpe_table_config(&dppd_shared[socket_id].cpe_table_entries, &dppd_shared[socket_id].cpe_table_entries_count, dppd_cfg.path_cpe_table_cfg, dppd_cfg.cpe_table_ports, DPPD_MAX_PORTS);
		}

		if (req_flags & REQ_IPV6_TUNNEL) {
			plog_info("\tReading IPv6 Tunnel config from %s\n", dppd_cfg.path_ipv6_tunnel_cfg);
			dppd_shared[socket_id].ipv6_tun_binding_table = read_ipv6_tun_bindings(dppd_cfg.path_ipv6_tunnel_cfg, socket_id);
		}
	}

	plog_info("=== Checking configuration consistency ===\n");
	check_consistent_cfg();

	lcore_id = -1;
	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];

		plog_info("\t*** Initializing core %u (%u tasks) ***\n", lcore_id, lconf->nb_tasks);
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
#ifdef ENABLE_EXTRA_USER_STATISTICS
			targ->n_users = dppd_shared[rte_lcore_to_socket_id(lcore_id)].qinq_to_gre_lookup_count;
#endif
			if (targ->task_init->early_init) {
				targ->task_init->early_init(targ);
			}
		}
	}

	plog_info("=== Initializing tasks ===\n");
	setup_all_task_structs();
}

static void
lsc_cb(__attribute__((unused)) uint8_t port_id, enum rte_eth_event_type type, __attribute__((unused)) void *param)
{
	struct rte_eth_link link;

	if (RTE_ETH_EVENT_INTR_LSC != type) {
		return;
	}

	rte_atomic32_inc(&lsc);
}

static void init_port(struct dppd_port_cfg *port_cfg)
{
	static char dummy_pool_name[] = "0_dummy";
	struct rte_eth_link link;
	uint8_t port_id;
	int ret;

	port_id =  port_cfg - dppd_port_cfg;
	plog_info("\t*** Initializing port %u ***\n", port_id);
	plog_info("\t\tPort name is set to %s\n", port_cfg->name);
	plog_info("\t\tPort max RX/TX queue is %u/%u\n", port_cfg->max_rxq, port_cfg->max_txq);
	plog_info("\t\tPort driver is %s\n", port_cfg->driver_name);

	DPPD_PANIC(port_cfg->n_rxq == 0 && port_cfg->n_txq == 0,
		   "\t\t port %u is enabled but no RX or TX queues have been configured", port_id);

	if (port_cfg->n_rxq == 0) {
		/* not receiving on this port */
		plog_info("\t\tPort %u had no RX queues, setting to 1\n", port_id);
		port_cfg->n_rxq = 1;
		port_cfg->pool[0] = rte_mempool_create(dummy_pool_name, port_cfg->n_rxd, MBUF_SIZE,
						       0,
						       sizeof(struct rte_pktmbuf_pool_private),
						       rte_pktmbuf_pool_init, NULL,
						       dppd_pktmbuf_init, 0,
						       port_cfg->socket, 0);
		dummy_pool_name[0]++;
	}
	else if (port_cfg->n_txq == 0) {
		/* not sending on this port */
		plog_info("\t\tPort %u had no TX queues, setting to 1\n", port_id);
		port_cfg->n_txq = 1;
	}

	if (port_cfg->n_rxq > 1)  {
		// Enable RSS if multiple receive queues
		port_cfg->port_conf.rxmode.mq_mode       		|= ETH_MQ_RX_RSS;
		port_cfg->port_conf.rx_adv_conf.rss_conf.rss_key 	= toeplitz_init_key;
		port_cfg->port_conf.rx_adv_conf.rss_conf.rss_key_len 	= TOEPLITZ_KEY_LEN;
#if RTE_VERSION >= RTE_VERSION_NUM(2,0,0,0)
		port_cfg->port_conf.rx_adv_conf.rss_conf.rss_hf 	= ETH_RSS_IPV4|ETH_RSS_NONFRAG_IPV4_UDP;
#else
		port_cfg->port_conf.rx_adv_conf.rss_conf.rss_hf 	= ETH_RSS_IPV4|ETH_RSS_NONF_IPV4_UDP;
#endif
	}

	plog_info("\t\tConfiguring port %u... with %u RX queues and %u TX queues\n",
		  port_id, port_cfg->n_rxq, port_cfg->n_txq);

	DPPD_PANIC(port_cfg->n_rxq > port_cfg->max_rxq, "\t\t\tToo many RX queues (configuring %u, max is %u)\n", port_cfg->n_rxq, port_cfg->max_rxq);
	DPPD_PANIC(port_cfg->n_txq > port_cfg->max_txq, "\t\t\tToo many TX queues (configuring %u, max is %u)\n", port_cfg->n_txq, port_cfg->max_txq);


	if (!strcmp(port_cfg->driver_name, "rte_ixgbevf_pmd") ||
	    !strcmp(port_cfg->driver_name, "rte_virtio_pmd")) {
		port_cfg->port_conf.intr_conf.lsc = 0;
		plog_info("\t\tDisabling link state interrupt for VF/virtio (unsupported)\n");
	}

	if (port_cfg->lsc_set_explicitely) {
		port_cfg->port_conf.intr_conf.lsc = port_cfg->lsc_val;
		plog_info("\t\tOverriding link state interrupt configuration to '%s'\n", port_cfg->lsc_val? "enabled" : "disabled");
	}

	ret = rte_eth_dev_configure(port_id, port_cfg->n_rxq,
				    port_cfg->n_txq, &port_cfg->port_conf);
	DPPD_PANIC(ret < 0, "\t\t\trte_eth_dev_configure() failed on port %u: %s (%d)\n", port_id, strerror(-ret), ret);

	if (port_cfg->port_conf.intr_conf.lsc) {
		rte_eth_dev_callback_register(port_id, RTE_ETH_EVENT_INTR_LSC, lsc_cb, NULL);
	}

	plog_info("\t\tMAC address set to "MAC_BYTES_FMT"\n", MAC_BYTES(port_cfg->eth_addr.addr_bytes));
	if_cfg[port_id] = port_cfg->eth_addr;

	/* initialize RX queues */
	for (uint16_t queue_id = 0; queue_id < port_cfg->n_rxq; ++queue_id) {
		plog_info("\t\tSetting up RX queue %u on port %u on socket %u with %u desc (pool 0x%p)\n",
			  queue_id, port_id, port_cfg->socket,
			  port_cfg->n_rxd, port_cfg->pool[queue_id]);

		ret = rte_eth_rx_queue_setup(port_id, queue_id,
					     port_cfg->n_rxd,
					     port_cfg->socket, &port_cfg->rx_conf,
					     port_cfg->pool[queue_id]);

		DPPD_PANIC(ret < 0, "\t\t\trte_eth_rx_queue_setup() failed on port %u: error %s (%d)\n", port_id, strerror(-ret), ret);
	}

	if (!strcmp(port_cfg->driver_name, "rte_virtio_pmd")) {
		port_cfg->tx_conf.txq_flags = ETH_TXQ_FLAGS_NOOFFLOADS;
		plog_info("\t\tDisabling TX offloads (virtio does not support TX offloads)\n");
	}

	/* initialize one TX queue per logical core on each port */
	for (uint16_t queue_id = 0; queue_id < port_cfg->n_txq; ++queue_id) {
		plog_info("\t\tSetting up TX queue %u on socket %u with %u desc\n",
			  queue_id, port_cfg->socket, port_cfg->n_txd);
		ret = rte_eth_tx_queue_setup(port_id, queue_id, port_cfg->n_txd,
					     port_cfg->socket, &port_cfg->tx_conf);
		DPPD_PANIC(ret < 0, "\t\t\trte_eth_tx_queue_setup() failed on port %u: error %d\n", port_id, ret);
	}

	plog_info("\t\tStarting up port %u ...", port_id);
	ret = rte_eth_dev_start(port_id);

	DPPD_PANIC(ret < 0, "\n\t\t\trte_eth_dev_start() failed on port %u: error %d\n", port_id, ret);
	plog_info(" done: ");

	/* Getting link status can be done without waiting if Link
	   State Interrupt is enabled since in that case, if the link
	   is recognized as being down, an interrupt will notify that
	   it has gone up. */
	if (port_cfg->port_conf.intr_conf.lsc)
		rte_eth_link_get_nowait(port_id, &link);
	else
		rte_eth_link_get(port_id, &link);

	port_cfg->link_up = link.link_status;
	port_cfg->link_speed = link.link_speed;
	if (link.link_status) {
		plog_info("Link Up - speed %'u Mbps - %s\n",
			  link.link_speed,
			  (link.link_duplex == ETH_LINK_FULL_DUPLEX) ?
			  "full-duplex" : "half-duplex");
	}
	else {
		plog_info("Link Down\n");
	}

	if (port_cfg->promiscuous) {
		rte_eth_promiscuous_enable(port_id);
		plog_info("\t\tport %u in promiscuous mode\n", port_id);
	}

	if (strcmp(port_cfg->driver_name, "rte_ixgbevf_pmd") && strcmp(port_cfg->driver_name, "rte_i40e_pmd") != 0) {
		for (uint8_t i = 0; i < 16; ++i) {
			ret = rte_eth_dev_set_rx_queue_stats_mapping(port_id, i, i);
			if (ret) {
				plog_info("\t\trte_eth_dev_set_rx_queue_stats_mapping() failed: error %d\n", ret);
			}
			ret = rte_eth_dev_set_tx_queue_stats_mapping(port_id, i, i);
			if (ret) {
				plog_info("\t\trte_eth_dev_set_tx_queue_stats_mapping() failed: error %d\n", ret);
			}
		}
	}
}

static void init_active_ports(void)
{
	uint8_t max_port_idx = dppd_last_port_active() + 1;

	plog_info("=== Initializing ports ===\n");
	for (uint8_t portid = 0; portid < max_port_idx; ++portid) {
		if (!dppd_port_cfg[portid].active) {
			continue;
		}
		init_port(&dppd_port_cfg[portid]);
	}
}

int main(int argc, char **argv)
{
	/* set en_US locale to print big numbers with ',' */
	setlocale(LC_NUMERIC, "en_US.utf-8");

	if (dppd_parse_args(argc, argv) != 0){
		dppd_usage(argv[0]);
	}

	plog_init(dppd_cfg.log_name, dppd_cfg.log_name_pid);
	plog_info("=== " PROGRAM_NAME " " VERSION_STR " ===\n");
	plog_info("\tUsing Intel(R) DPDK %s\n", rte_version() + sizeof(RTE_VER_PREFIX));

	if (dppd_read_config_file() != 0 ||
	    dppd_setup_rte(argv[0]) != 0) {
		return EXIT_FAILURE;
	}

	if (dppd_cfg.flags & DSF_CHECK_SYNTAX) {
		plog_info("=== Configuration file syntax has been checked ===\n\n");
		return EXIT_SUCCESS;
	}

	plog_info("=== Initializing rte devices ===\n");
	init_rte_ring_dev();
	init_rte_dev();
	plog_info("=== Calibrating TSC overhead ===\n");
	dppd_init_tsc_overhead();
	plog_info("\tTSC running at %"PRIu64" Hz\n", rte_get_tsc_hz());

	init_lcores();
	init_active_ports();

	if (dppd_cfg.flags & DSF_CHECK_INIT) {
		plog_info("=== Initialization sequence completed ===\n\n");
		return EXIT_SUCCESS;
	}

	/* Current way that works to disable DPDK logging */
	FILE *f = fopen("/dev/null", "r");
	rte_openlog_stream(f);
	plog_info("=== DPPD started ===\n");
	run(dppd_cfg.flags);

	return EXIT_SUCCESS;
}
