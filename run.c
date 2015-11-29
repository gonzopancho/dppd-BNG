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

#include <inttypes.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <rte_launch.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>

#include "run.h"
#include "handle_gen.h"
#include "parse_utils.h"
#include "dppd_cfg.h"
#include "dppd_port_cfg.h"
#include "cqm.h"
#include "quit.h"
#include "commands.h"
#include "dppd_args.h"
#include "main.h"
#include "log.h"
#include "display.h"
#include "handle_acl.h"
#include "input.h"
#include "input_curses.h"
#include "input_conn.h"

#include "handle_routing.h"
#include "handle_qinq_decap4.h"

struct input *inputs[32];
int n_inputs;
int max_input_fd;

inline int check_core_task(int lcore_id, int task_id);

inline int check_core_task(int lcore_id, int task_id)
{
	if (lcore_id >= RTE_MAX_LCORE) {
		plog_err("Invalid core id %u (lcore ID above %d)\n", lcore_id, RTE_MAX_LCORE);
		return 0;
	}
	else if (task_id >= lcore_cfg[lcore_id].nb_tasks) {
		plog_err("Invalid task id (valid task IDs for core %u are below %u)\n",
			 lcore_id, lcore_cfg[lcore_id].nb_tasks);
		return 0;
	}
	return 1;
}

int reg_input(struct input *in)
{
	if (n_inputs == sizeof(inputs)/sizeof(inputs[0]))
		return -1;

	for (int i = 0; i < n_inputs; ++i) {
		if (inputs[i] == in)
			return -1;
	}
	inputs[n_inputs++] = in;
	max_input_fd = RTE_MAX(in->fd, max_input_fd);

	return 0;
}

void unreg_input(struct input *in)
{
	int rm, i;

	for (rm = 0; rm < n_inputs; ++rm) {
		if (inputs[rm] == in) {
			break;
		}
	}

	if (rm == n_inputs)
		return ;

	for (i = rm + 1; i < n_inputs; ++i) {
		inputs[i - 1] = inputs[i];
	}

	n_inputs--;
	max_input_fd = 0;
	for (i = 0; i < n_inputs; ++i) {
		max_input_fd = RTE_MAX(inputs[i]->fd, max_input_fd);
	}
}

static unsigned update_interval = 1000;
int stop_dppd = 0; /* set to 1 to stop dppd */

static void print_rx_tx_info(void)
{
	uint32_t lcore_id = -1;
	while(dppd_core_next(&lcore_id, 0) == 0) {
		for (uint8_t task_id = 0; task_id < lcore_cfg[lcore_id].nb_tasks; ++task_id) {
			struct task_args *targ = &lcore_cfg[lcore_id].targs[task_id];

			plog_info("Core %u:", lcore_id);
			if (targ->rx_ports[0] != NO_PORT_AVAIL) {
				for (int i=0;i<targ->nb_rxports;i++) {
					plog_info(" RX port %u (queue %u)", targ->rx_ports[i], targ->rx_queues[i]);
				}
			}
			else {
				for (uint8_t j = 0; j < targ->nb_rxrings; ++j) {
					plog_info(" RX ring[%u,%u] %p", task_id, j, targ->rx_rings[j]);
				}
			}
			plog_info(" ==>");
			for (uint8_t j = 0; j < targ->nb_txports; ++j) {
				plog_info(" TX port %u (queue %u)", targ->tx_port_queue[j].port,
					targ->tx_port_queue[j].queue);
			}

			for (uint8_t j = 0; j < targ->nb_txrings; ++j) {
				plog_info(" TX ring %p", targ->tx_rings[j]);
			}

			plog_info("\n");
		}
	}
}

void process_input(int* needs_refresh, const char *str, int fd, void (*cb)(int fd, const char *data, size_t len))
{
	static unsigned lcore_id, task_id, nb_packets, val, id,
		port, queue, rate, ip[4], prefix, next_hop_idx,
		interface, gre_id, svlan, cvlan, mac[6], user;
	unsigned count;
	float speed;
	uint32_t pkt_size;
	static char mode[20];
	struct rte_ring *ring;
	unsigned short offset;
	uint32_t value;
	uint8_t value_len;

	if (strcmp(str, "quit") == 0) {
		plog_info("Leaving...\n");
		stop_core_all();
		stop_dppd = 1;
	}
	else if (sscanf(str, "dump %u %u %u", &lcore_id, &task_id, &nb_packets) == 3) {
		if (lcore_id >= RTE_MAX_LCORE) {
			plog_err("Invalid core id %u (lcore ID above %d)\n", lcore_id, RTE_MAX_LCORE);
		}
		else if (!dppd_core_active(lcore_id, 0)) {
			plog_err("Invalid core id %u (lcore is not active)\n", lcore_id);
		}
		else {
			cmd_dump(lcore_id, task_id, nb_packets);
		}
	}
	else if (sscanf(str, "rate %u %u %u", &queue, &port, &rate) == 3) {
		if (port > DPPD_MAX_PORTS) {
			plog_err("Max port id allowed is %u (specified %u)\n", DPPD_MAX_PORTS, port);
		}
		else if (!dppd_port_cfg[port].active) {
			plog_err("Port %u not active\n", port);
		}
		else if (queue >= dppd_port_cfg[port].n_txq) {
			plog_err("Number of active queues is %u\n",
				 dppd_port_cfg[port].n_txq);
		}
		else if (rate > dppd_port_cfg[port].link_speed) {
			plog_err("Max rate allowed on port %u queue %u is %u Mbps\n",
				 port, queue, dppd_port_cfg[port].link_speed);
		}
		else {
			if (rate == 0) {
				plog_info("Disabling rate limiting on port %u queue %u\n",
					  port, queue);
			}
			else {
				plog_info("Setting rate limiting to %u Mbps on port %u queue %u\n",
					  rate, port, queue);
			}
			rte_eth_set_queue_rate_limit(port, queue, rate);
		}
	}
	else if (sscanf(str, "count %u %u %u", &lcore_id, &task_id, &count) == 3) {
		if (check_core_task(lcore_id, task_id)) {
			if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "gen")) {
				plog_err("Core %u task %u is not generating packets\n", lcore_id, task_id);
			}
			else {
				((struct task_gen *)lcore_cfg[lcore_id].task[task_id])->pkt_count = count;
				plog_info("Core %u task %u stopping after %u packets\n", lcore_id, task_id, count);
			}
		}
	}
	else if (sscanf(str, "pkt_size %u %u %d", &lcore_id, &task_id, &pkt_size) == 3) {
		if (check_core_task(lcore_id, task_id)) {
			if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "gen")) {
				plog_err("Core %u task %u is not generating packets\n", lcore_id, task_id);
			}
			else if (pkt_size > 1514 || pkt_size < 34) {    // 34 for 14 + 20 (MAC, EtherType and IP)
				plog_err("pkt_size out of range (must be betweeen 34 and 1514)\n");
			}
			else {
				((struct task_gen *)lcore_cfg[lcore_id].task[task_id])->pkt_size = pkt_size;
				plog_info("Setting pkt_size to %u \n", pkt_size);
			}
		}
	}

	else if (sscanf(str, "speed %u %u %f", &lcore_id, &task_id, &speed) == 3) {
		if (check_core_task(lcore_id, task_id)) {
			if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "gen")) {
				plog_err("Core %u task %u is not generating packets\n", lcore_id, task_id);
			}
			else if (speed > 100.0f || speed < 0.0f) {
				plog_err("Speed out of range (must be betweeen 0%% and 100%%)\n");
			}
			else {
				uint64_t bps = speed * 12500000;

				((struct task_gen *)lcore_cfg[lcore_id].task[task_id])->rate_bps = bps;
				plog_info("Setting rate to %"PRIu64" Bps\n", bps);
			}
		}
	}
	else if (sscanf(str, "speed_byte %u %u %u", &lcore_id, &task_id, &value) == 3) {
		if (check_core_task(lcore_id, task_id)) {
			if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "gen")) {
				plog_err("Core %u task %u is not generating packets\n", lcore_id, task_id);
			}
			else if (value > 1250000000) {
				plog_err("Speed out of range (must be <= 1250000000)\n");
			}
			else {
				((struct task_gen *)lcore_cfg[lcore_id].task[task_id])->rate_bps = value;
				plog_info("Setting rate to %"PRIu32" Bps\n", value);
			}
		}
	}
	else if (strcmp(str, "reset values all") == 0) {
		lcore_id = -1;
        	while (dppd_core_next(&lcore_id, 0) == 0) {
			for (task_id = 0; task_id < lcore_cfg[lcore_id].nb_tasks; task_id++) {
				if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "gen") == 0) {
					struct task_gen *task = ((struct task_gen *)lcore_cfg[lcore_id].task[task_id]);
					plog_info("Resetting values on core %d task %d from %d values\n", lcore_id, task_id, task->n_values);
					task->n_values = 0;
				}
			}
		}
	}
	else if (sscanf(str, "reset values %u %u", &lcore_id, &task_id) == 2) {
		if (check_core_task(lcore_id, task_id)) {
			if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "gen")) {
				plog_err("Core %u task %u is not generating packets\n", lcore_id, task_id);
			}
			else {
				struct task_gen *task = ((struct task_gen *)lcore_cfg[lcore_id].task[task_id]);
				plog_info("Resetting values on core %d task %d from %d values\n", lcore_id, task_id, task->n_values);
				task->n_values = 0;
			}
		}
	}
	else if (sscanf(str, "set value %u %u %hu %u %hhu", &lcore_id, &task_id, &offset, &value, &value_len) == 5) {
		if (check_core_task(lcore_id, task_id)) {
			if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "gen")) {
				plog_err("Core %u task %u is not generating packets\n", lcore_id, task_id);
			}
			else if (offset > ETHER_MAX_LEN) {
				plog_err("Offset out of range (must be less then %u)\n", ETHER_MAX_LEN);
			}
			else if (value_len > 4) {
				plog_err("Length out of range (must be less then 4)\n");
			}
			else {
				struct task_gen *task = ((struct task_gen *)lcore_cfg[lcore_id].task[task_id]);
				if (task->n_values >= 64) {
					plog_info("Unable to set Byte %"PRIu16" to %"PRIu8" - too many value set\n", offset, value);
				}
				else {
					task->value[task->n_values] = rte_cpu_to_be_32(value) >> ((4 - value_len) * 8);
					task->offset[task->n_values] = offset;
					task->value_len[task->n_values] = value_len;
					task->n_values++;
					plog_info("Setting Byte %"PRIu16" to %"PRIu32"\n", offset, value);
				}
			}
		}
	}
	else if (sscanf(str, "thread info %u %u", &lcore_id, &task_id) == 2) {
		cmd_thread_info(lcore_id, task_id);
	}
	else if (sscanf(str, "verbose %u", &id) == 1) {
		if (plog_set_lvl(id) != 0) {
			plog_err("Cannot set log level to %u\n", id);
		}
	}
	else if (sscanf(str, "rx distr start %u", &id) == 1) {
		cmd_rx_distr_start(id);
	}
	else if (sscanf(str, "rx distr stop %u", &id) == 1) {
		cmd_rx_distr_stop(id);
	}
	else if (sscanf(str, "rx distr reset %u", &id) == 1) {
		cmd_rx_distr_rst(id);
	}
	else if (sscanf(str, "rx distr show %u", &lcore_id)) {
		cmd_rx_distr_show(lcore_id);
	}
	else if (strcmp(str, "stop all") == 0) {
		stop_core_all();
		*needs_refresh = 1;
	}
	else if (strcmp(str, "start all") == 0) {
		start_core_all();
		*needs_refresh = 1;
	}
	else if (strncmp(str, "stop ", strlen("stop ")) == 0) {
		uint32_t cores[32] = {0};
		int ret;
		ret = parse_list_set(cores, str + strlen("stop "), 32);
		if (ret < 0) {
			plog_err("Syntax error\n");
		}
		stop_cores(cores, ret);
		*needs_refresh = 1;
	}
	else if (strncmp(str, "start ", strlen("start ")) == 0) {
		uint32_t cores[32] = {0};
		int ret;
		ret = parse_list_set(cores, str + strlen("start "), 32);
		if (ret < 0) {
			plog_err("Syntax error\n");
		}
		start_cores(cores, ret);
		*needs_refresh = 1;
	}
	else if (strcmp(str, "tot stats") == 0) {
		uint64_t tot_rx = global_total_rx();
		uint64_t tot_tx = global_total_tx();
		uint64_t last_tsc = global_last_tsc();
		if (fd != -1) {
			char buf[128];
			snprintf(buf, sizeof(buf),
				 "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n",
				 tot_rx, tot_tx, last_tsc, rte_get_tsc_hz());
			cb(fd, buf, strlen(buf));
		}
		else {
			plog_info("RX: %"PRIu64", TX: %"PRIu64"\n", tot_rx, tot_tx);
		}
	}
	else if (strcmp(str, "tot ierrors per sec") == 0) {
		uint64_t tot = tot_ierrors_per_sec();
		uint64_t last_tsc = global_last_tsc();

		if (fd != -1) {
			char buf[128];
			snprintf(buf, sizeof(buf),
				 "%"PRIu64",%"PRIu64",%"PRIu64"\n",
				 tot, last_tsc, rte_get_tsc_hz());
			cb(fd, buf, strlen(buf));
		}
		else {
			plog_info("ierrors: %"PRIu64"\n", tot);
		}
	}
	else if (strcmp(str, "tot ierrors tot") == 0) {
		uint64_t tot = tot_ierrors_tot();
		uint64_t last_tsc = global_last_tsc();

		if (fd != -1) {
			char buf[128];
			snprintf(buf, sizeof(buf),
				 "%"PRIu64",%"PRIu64",%"PRIu64"\n",
				 tot, last_tsc, rte_get_tsc_hz());
			cb(fd, buf, strlen(buf));
		}
		else {
			plog_info("ierrors: %"PRIu64"\n", tot);
		}
	}
	else if (strcmp(str, "pps stats") == 0) {
		uint64_t tot_rx = global_pps_rx();
		uint64_t tot_tx = global_pps_tx();
		uint64_t last_tsc = global_last_tsc();
		if (fd != -1) {
			char buf[128];
			snprintf(buf, sizeof(buf),
				 "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n",
				 tot_rx, tot_tx, last_tsc, rte_get_tsc_hz());
			cb(fd, buf, strlen(buf));
		}
		else {
			plog_info("RX: %"PRIu64", TX: %"PRIu64"\n", tot_rx, tot_tx);
		}
	}
	else if (sscanf(str, "lat stats %u %u", &lcore_id, &task_id) == 2) {
		if (check_core_task(lcore_id, task_id)) {
			if (strcmp(lcore_cfg[lcore_id].targs[task_id].task_init->mode_str, "lat")) {
				plog_err("Core %u task %u is not measuring latency\n", lcore_id, task_id);
			}
			else {
				uint64_t lat_min, lat_max, lat_avg, last_tsc;

				lat_min = stats_core_task_lat_min(lcore_id, task_id);
				lat_max = stats_core_task_lat_max(lcore_id, task_id);
				lat_avg = stats_core_task_lat_avg(lcore_id, task_id);
				last_tsc = stats_core_task_last_tsc(lcore_id, task_id);

				if (fd != -1) {
					char buf[128];
					snprintf(buf, sizeof(buf),
						 "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n",
						 lat_min, lat_max, lat_avg, last_tsc, rte_get_tsc_hz());
					cb(fd, buf, strlen(buf));
				}
				else {
					plog_info("min: %"PRIu64", max: %"PRIu64", avg: %"PRIu64"\n",
						  lat_min, lat_max, lat_avg);
				}
			}
		}
	}
	else if (sscanf(str, "core stats %u %u", &lcore_id, &task_id) == 2) {
		if (check_core_task(lcore_id, task_id)) {
			uint64_t tot_rx = stats_core_task_tot_rx(lcore_id, task_id);
			uint64_t tot_tx = stats_core_task_tot_tx(lcore_id, task_id);
			uint64_t tot_drop = stats_core_task_tot_drop(lcore_id, task_id);
			uint64_t last_tsc = stats_core_task_last_tsc(lcore_id, task_id);

			if (fd != -1) {
				char buf[128];
				snprintf(buf, sizeof(buf),
					 "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n",
					 tot_rx, tot_tx, tot_drop, last_tsc, rte_get_tsc_hz());
				cb(fd, buf, strlen(buf));
			}
			else {
				plog_info("RX: %"PRIu64", TX: %"PRIu64", DROP: %"PRIu64"\n",
					  tot_rx, tot_tx, tot_drop);
			}
		}
	}
	else if (strcmp(str, "reset stats") == 0) {
		stats_reset();
	}
	else if (sscanf(str, "update interval %u", &val) == 1) {
		if (val < 10) {
			plog_err("Minimum update interval is 10 ms\n");
		}
		else {
			plog_info("Setting update interval to %d ms\n", val);
			update_interval = val;
		}
	}
	else if (sscanf(str, "port_stats %u", &val) == 1) {
		struct get_port_stats s;
		if (stats_port(val, &s)) {
			plog_err("Invalid port %u\n", val);
		}
		else {
			char buf[256];
			snprintf(buf, sizeof(buf),
				 "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64","
				 "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64","
				 "%"PRIu64",%"PRIu64",%"PRIu64",%"PRIu64"\n",
				 s.no_mbufs_diff, s.ierrors_diff,
				 s.rx_bytes_diff, s.tx_bytes_diff,
				 s.rx_pkts_diff, s.tx_pkts_diff,
				 s.rx_tot, s.tx_tot,
				 s.no_mbufs_tot, s.ierrors_tot,
				 s.last_tsc, s.prev_tsc);
			plog_info("%s", buf);
			if (fd != -1) {
				cb(fd, buf, strlen(buf));
			}
		}
	}
	else if (sscanf(str, "port info %u", &val) == 1) {
		cmd_portinfo(val);
	}
	else if (sscanf(str, "port up %u", &val) == 1) {
		cmd_port_up(val);
	}
	else if (sscanf(str, "port down %u", &val) == 1) {
		cmd_port_down(val);
	}
	else if (sscanf(str, "ring info %u %u", &lcore_id, &task_id) == 2) {
		cmd_ringinfo(lcore_id, task_id);
	}
	else if (strcmp(str, "ring info all") == 0) {
		cmd_ringinfo_all();
	}
	else if (strncmp(str, "rule add ", 9) == 0) {
		str += strlen("rule add ");

		char *space = strstr(str, " ");
		if (space) {
			space = strstr(space + 1, " ");
		}
		else {
			plog_err("Invalid syntax in rule add command (see help)\n");
		}

		*space = 0;
		if (sscanf(str, "%u %u", &lcore_id, &task_id) != 2) {
			plog_info("%s\n", str);
			plog_err("Parsing core and task id in arp add command failed (see help)\n");
		}
		else {
			str = space + 1;
			char *fields[9];
			char str_cpy[255];
			strncpy(str_cpy, str, 255);
			// example add rule command: rule add 15 0 1&0x0fff 1&0x0fff 0&0 128.0.0.0/1 128.0.0.0/1 5000-5000 5000-5000 allow
			int ret = rte_strsplit(str_cpy, 255, fields, 9, ' ');
			if (ret != 8) {
				plog_err("Invalid syntax in rule add command (see help)\n");
				return ;
			}

			struct acl4_rule rule;
			struct acl4_rule* prule = &rule;
			if (str_to_rule(&rule, fields, -1, 1) == 0) {
				ring = ctrl_rings[lcore_id*MAX_TASKS_PER_CORE + task_id];
				if (lcore_id >= RTE_MAX_LCORE || !lcore_cfg[lcore_id].nb_tasks) {
					plog_err("Adding rule failed: Core %u not active\n", lcore_id);
				}
				else if (task_id >= lcore_cfg[lcore_id].nb_tasks) {
					plog_err("Adding rule failed: Task %u is not active on core %u\n", task_id, lcore_id);
				}
				else if (!ring) {
					plog_err("No ring for control messages to core %u task %u\n", lcore_id, task_id);
				}
				else {
					while (rte_ring_sp_enqueue_bulk(ring, (void *const *)&prule, 1));
					while (!rte_ring_empty(ring));
				}
			}
			else {
				plog_err("Invalid syntax in rule add command (see help)\n");
			}
		}
	}
	else if (sscanf(str, "route add %u %u %u.%u.%u.%u/%u %u", &lcore_id, &task_id,
			ip, ip + 1, ip + 2, ip + 3, &prefix, &next_hop_idx) == 8) {
		ring = ctrl_rings[lcore_id*MAX_TASKS_PER_CORE + task_id];
		if (lcore_id >= RTE_MAX_LCORE || !lcore_cfg[lcore_id].nb_tasks) {
			plog_err("Adding route failed: Core %u not active\n", lcore_id);
		}
		else if (task_id >= lcore_cfg[lcore_id].nb_tasks) {
			plog_err("Adding route failed: Task %u is not active on core %u\n", task_id, lcore_id);
		}
		else if (!ring) {
			plog_err("No ring for control messages to core %u task %u\n", lcore_id, task_id);
		}
		else {
			struct route_msg rmsg;
			struct route_msg *pmsg = &rmsg;

			rmsg.ip_bytes[0] = ip[0];
			rmsg.ip_bytes[1] = ip[1];
			rmsg.ip_bytes[2] = ip[2];
			rmsg.ip_bytes[3] = ip[3];
			rmsg.prefix = prefix;
			rmsg.nh = next_hop_idx;
			while (rte_ring_sp_enqueue_bulk(ring, (void *const *)&pmsg, 1));
			while (!rte_ring_empty(ring));
		}
	}
	else if (!strncmp(str, "arp add ", 8)) {
		struct arp_msg amsg;
		struct arp_msg *pmsg = &amsg;
		str = str + strlen("arp add ");

		char *space = strstr(str, " ");
		if (space) {
			space = strstr(space + 1, " ");
		}
		else {
			plog_err("Invalid arp add command syntax (see help)\n");
		}

		*space = 0;
		if (sscanf(str, "%u %u", &lcore_id, &task_id) != 2) {
			plog_info("%s\n", str);
			plog_err("Parsing core and task id in arp add command failed (see help)\n");
		}
		else {

			str = space + 1;

			if (str_to_arp_msg(&amsg, str) == 0) {
				ring = ctrl_rings[lcore_id*MAX_TASKS_PER_CORE + task_id];
				if (lcore_id >= RTE_MAX_LCORE || !lcore_cfg[lcore_id].nb_tasks) {
					plog_err("Failed to add route because core %u not active\n", lcore_id);
				}
				else if (task_id >= lcore_cfg[lcore_id].nb_tasks) {
					plog_err("Task %u is not active on core %u\n", task_id, lcore_id);
				}
				else if (!ring) {
					plog_err("No ring for control messages to core %u task %u\n", lcore_id, task_id);
				}
				else {

					while (rte_ring_sp_enqueue_bulk(ring, (void *const *)&pmsg, 1));
					while (!rte_ring_empty(ring));
				}
			}
			else {
				plog_err("Invalid arp add command syntax (see help)\n");
			}
		}
	}
	else if (!strcmp(str, "mem info")) {
		plog_info("Memory layout:\n");
		cmd_mem_layout();
	}
	else if (strcmp(str, "help") == 0) {
		plog_info("Available commands:\n"
			  "\tquit\n"
			  "\tstart all\n"
			  "\tstop all\n"
			  "\tstart <core id>\n"
			  "\tstop <core id>\n"
			  "\tverbose <level>\n"
			  "\trx distr start <core id>\n"
			  "\trx distr stop <core id>\n"
			  "\trx distr reset <core id>\n"
			  "\trx distr show <core id>\n"
			  "\treset stats\n"
			  "\tupdate interval <value>\n"
			  "\tmem info\n"
			  "\tport info <port id>\n"
			  "\tport down <port id>\n"
			  "\tport up <port id>\n"
			  "\tring info <core id> <task id>\n"
			  "\tring info all\n"
			  "\tdump <core id> <task id> <nb packets>\n"
			  "\trate <port id> <queue id> <rate> (rate does not include preamble, SFD and IFG)\n"
			  "\trule add <core id> <task id> svlan_id&mask cvlan_id&mask ip_proto&mask source_ip/prefix destination_ip/prefix range dport_range action\n"
			  "\troute add <core id> <task id> <ip/prefix> <next hop id> (for example, route add 10.0.16.0/24 9)\n"
			  "\tcount <core id> <task id> <count>\n"
			  "\ttot stats\n"
			  "\ttot ierrors per sec\n"
			  "\ttot ierrors tot\n"
			  "\tpps stats\n"
			  "\tlat stats <core_id> <task_id>\n"
			  "\tpkt_size <core_id> <task_id> <pkt_size>\n"
			  "\tspeed <core_id> <task_id> <speed percentage>\n"
			  "\tspeed_byte <core_id> <task_id> <speed in bytes per sec>\n"
			  "\treset all values\n"
			  "\treset values <core_id> <task_id>\n"
			  "\tset value <core_id> <task_id> <offset> <value> <value_len>\n"
			  "\tthread info <core_id> <task_id>\n"
			  "\tarp add <core id> <task id> <port id> <gre id> <svlan> <cvlan> <ip addr> <mac addr> <user>\n");
	}
	else {
		plog_err("Unknown command: %s\n", str);
	}
}

static void update_link_states(void)
{
	struct dppd_port_cfg *port_cfg;
	struct rte_eth_link link;

	for (uint8_t portid = 0; portid < DPPD_MAX_PORTS; ++portid) {
		if (!dppd_port_cfg[portid].active) {
			continue;
		}

		port_cfg  = &dppd_port_cfg[portid];
		rte_eth_link_get_nowait(portid, &link);
		port_cfg->link_up = link.link_status;
		port_cfg->link_speed = link.link_speed;
	}
}

static int tsc_diff_to_tv(uint64_t beg, uint64_t end, struct timeval *tv)
{
	if (end < beg) {
		return -1;
	}

	uint64_t diff = end - beg;
	uint64_t sec_tsc = rte_get_tsc_hz();
	uint64_t sec = diff/sec_tsc;

	tv->tv_sec = sec;
	diff -= sec*sec_tsc;
	tv->tv_usec = diff*1000000/sec_tsc;

	return 0;
}

/* start main loop */
void __attribute__((noreturn)) run(uint32_t flags)
{
	stats_init();
	display_init(dppd_cfg.start_time, dppd_cfg.duration_time);

	print_rx_tx_info();

	/* start all tasks on worker cores */
	if (flags & DSF_AUTOSTART) {
		start_core_all();
	}

#ifndef BRAS_STATS
	while(1) {sleep(1000000);}
#endif

	uint64_t cur_tsc = rte_rdtsc();
	uint64_t next_update = cur_tsc + rte_get_tsc_hz();
	uint64_t stop_tsc = 0;
	int needs_refresh = 0;
	int ret = 0;

	int32_t lsc_local;
	if (dppd_cfg.duration_time != 0) {
		stop_tsc = cur_tsc + dppd_cfg.start_time*rte_get_tsc_hz() + dppd_cfg.duration_time*rte_get_tsc_hz();
	}

	if (flags & DSF_LISTEN_TCP)
		DPPD_PANIC(reg_input_tcp(), "Failed to start listening on TCP port 8474: %s\n", strerror(errno));
	if (flags & DSF_LISTEN_UDS)
		DPPD_PANIC(reg_input_uds(), "Failed to start listening on UDS /tmp/dppd.sock: %s\n", strerror(errno));

	reg_input_curses();

	fd_set in_fd, out_fd, err_fd;
	struct timeval tv;
	FD_ZERO(&in_fd);
	FD_ZERO(&out_fd);
	FD_ZERO(&err_fd);

	while (stop_dppd == 0) {
		cur_tsc = rte_rdtsc();

		/* Multiplex input handling with display. */
		if (tsc_diff_to_tv(cur_tsc, next_update, &tv) == 0) {
			for (int i = 0; i < n_inputs; ++i) {
				FD_SET(inputs[i]->fd, &in_fd);
			}
			ret = select(max_input_fd + 1, &in_fd, &out_fd, &err_fd, &tv);
		}
		else {
			FD_ZERO(&in_fd);
		}

		if (ret) {
			for (int i = 0; i < n_inputs; ++i) {
				if (FD_ISSET(inputs[i]->fd, &in_fd)) {
					inputs[i]->proc_input(inputs[i], &needs_refresh);
					if (needs_refresh) {
						display_refresh();
					}
				}
			}
			FD_ZERO(&in_fd);
			ret = 0;
		}
		else {
			next_update += rte_get_tsc_hz()*update_interval/1000;

			stats_update();

			lsc_local = rte_atomic32_read(&lsc);

			if (lsc_local) {
				rte_atomic32_dec(&lsc);
				update_link_states();
				display_refresh();
			}

			display_stats();
		}

		if (stop_tsc && cur_tsc >= stop_tsc) {
			stop_dppd = 1;
		}
	}

	plog_info("total RX: %"PRIu64", total TX: %"PRIu64", average RX: %"PRIu64" pps, average TX: %"PRIu64" pps\n",
		global_total_rx(),
		global_total_tx(),
		global_avg_rx(),
		global_avg_tx());

	if (dppd_cfg.flags & DSF_WAIT_ON_QUIT) {
		stop_core_all();
	}

	display_end();
	exit(EXIT_SUCCESS);
}
