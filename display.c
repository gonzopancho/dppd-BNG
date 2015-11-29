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

#ifdef BRAS_STATS
#include <curses.h>
#endif

#include <rte_cycles.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <signal.h>

#include "handle_lat.h"
#include "cqm.h"
#include "msr.h"
#include "display.h"
#include "log.h"
#include "commands.h"
#include "main.h"
#include "stats.h"
#include "dppd_args.h"
#include "dppd_cfg.h"
#include "dppd_assert.h"
#include "version.h"
#include "dppd_port_cfg.h"


struct key_code {
	const char *seq;
	int key;
};

static struct key_code key_codes[] = {
	{"[11~", KEY_F(1)},
	{"[12~", KEY_F(2)},
	{"[13~", KEY_F(3)},
	{"[14~", KEY_F(4)},
	{"[15~", KEY_F(5)},
	{"[17~", KEY_F(6)},
	{"[18~", KEY_F(7)},
	{"[19~", KEY_F(8)},
	{"[20~", KEY_F(9)},
	{"[21~", KEY_F(10)},
	{"[23~", KEY_F(11)},
	{"[24~", KEY_F(12)},
	{"[A", KEY_UP},
	{"[B", KEY_DOWN},
	{"[C", KEY_RIGHT},
	{"[D", KEY_LEFT},
	{"[5~", KEY_PPAGE},
	{"[6~", KEY_NPAGE},
	{"OP", KEY_F(1)},
	{"OQ", KEY_F(2)},
	{"OR", KEY_F(3)},
	{"OS", KEY_F(4)},
	{0,0},
};

struct screen_state {
	char chosen_screen;
	int chosen_page;
};

struct screen_state screen_state;
static void stats_display_layout(uint8_t in_place);

/* Set up the display mutex  as recursive. This enables threads to use
   display_[un]lock() to lock  the display when multiple  calls to for
   instance plog_info() need to be made. */

static pthread_mutex_t disp_mtx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;

static void display_lock(void)
{
	pthread_mutex_lock(&disp_mtx);
}

static void display_unlock(void)
{
	pthread_mutex_unlock(&disp_mtx);
}

#ifdef BRAS_STATS

struct cqm_related {
	struct cqm_features	features;
	uint8_t			supported;
	uint32_t		last_rmid;
};

struct cqm_related cqm;

struct global_stats {
	uint64_t tx_pps;
	uint64_t rx_pps;
	uint64_t tx_tot;
	uint64_t rx_tot;
	uint64_t rx_tot_beg;
	uint64_t tx_tot_beg;
	uint64_t avg_start;
	uint8_t  started_avg;
	uint64_t rx_avg;
	uint64_t tx_avg;
	uint64_t tot_ierrors;
	uint64_t last_tsc;
};

struct port_stats {
	uint64_t tot_tx_pkt_count;
	uint64_t tot_tx_pkt_drop;
	uint64_t tot_rx_pkt_count;

	uint64_t tsc[2];
	uint32_t tx_pkt_count[2];
	uint32_t tx_pkt_drop[2];
	uint32_t rx_pkt_count[2];
	uint32_t empty_cycles[2];
};

struct core_port {
	struct task_stats *stats;
	struct port_stats *port_stats;
	uint8_t lcore_id;
	uint8_t port_id;
	/* flags set if total RX/TX values need to be reported set at
	   initialization time, only need to access stats values in port */
	uint8_t flags;
};

struct lcore_stats {
	struct port_stats port_stats[MAX_TASKS_PER_CORE];
	uint32_t rmid;
	uint64_t cqm_data;
	uint64_t cqm_bytes;
	uint64_t cqm_fraction;
	uint64_t afreq[2];
	uint64_t mfreq[2];
};

struct eth_stats {
	uint64_t tsc[2];
	uint64_t no_mbufs[2];
	uint64_t ierrors[2];
	uint64_t rx_tot[2];
	uint64_t tx_tot[2];
	uint64_t rx_bytes[2];
	uint64_t tx_bytes[2];
};

/* Advanced text output */
static WINDOW *scr = NULL, *win_txt, *win_cmd, *win_stat, *win_title, *win_help;

#define MAX_STRID      64
#define MAX_CMDLEN     256
static char str[MAX_CMDLEN] = {0};

static void refresh_cmd_win(void)
{
	display_lock();
	werase(win_cmd);
	waddstr(win_cmd, str);
	wrefresh(win_cmd);
	display_unlock();
}

/* Stores all readed values from the cores, displaying is done afterwards because
   displaying introduces overhead. If displaying was done right after the values
   are read, inaccuracy is introduced for later cores */
int last_stat; /* 0 or 1 to track latest 2 measurements */
static struct lcore_stats  lcore_stats[RTE_MAX_LCORE];
static struct core_port    core_ports[RTE_MAX_LCORE *MAX_TASKS_PER_CORE];
static struct core_port    *core_port_ordered[RTE_MAX_LCORE*MAX_TASKS_PER_CORE];
static struct global_stats global_stats;
static struct eth_stats    eth_stats[16];
static uint8_t nb_tasks_tot;
static uint8_t nb_interface;
static uint8_t nb_active_interfaces;
static uint16_t core_port_height;
static uint64_t start_tsc;
static uint64_t beg_tsc, end_tsc;
static int msr_support;
static int col_offset;

static uint16_t n_mempools;
static uint16_t n_latency;

struct task_lat_stats {
	struct task_lat *task;
	uint8_t lcore_id;
	uint8_t task_id;
	uint8_t rx_port; /* Currently only one */
};

struct task_lat_stats task_lats[64];
struct lat_test lat_stats[64]; /* copy of stats when running update stats. */

struct mempool_stats {
	struct rte_mempool *pool;
	uint16_t port;
	uint16_t queue;
	size_t free;
	size_t size;
};

struct mempool_stats mempool_stats[64];

/* Colors used in the interface */
enum colors {
	INVALID_COLOR,
	WHITE_ON_BLUE,
	RED_ON_BLACK,
	BLACK_ON_CYAN,
	BLACK_ON_WHITE,
	BLACK_ON_YELLOW,
	WHITE_ON_RED,
};

static WINDOW *create_subwindow(int height, int width, int y_pos, int x_pos)
{
	WINDOW *win = subwin(scr, height, width, y_pos, x_pos);
	touchwin(scr);
	return win;
}

/* Format string capable [mv]waddstr() wrappers */
__attribute__((format(printf, 4, 5))) static inline void mvwaddstrf(WINDOW* win, int y, int x, const char *fmt, ...)
{
	va_list ap;

	char buf[1024];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	wmove(win, y, x);
	if (x > COLS - 1) {
		return ;
	}

	/* to prevent strings from wrapping and */
	if (strlen(buf) > (uint32_t)COLS - x) {
		buf[COLS - 1 - x] = 0;
	}
	waddstr(win, buf);
}
uint64_t stats_core_task_tot_rx(uint8_t lcore_id, uint8_t task_id)
{
	return lcore_stats[lcore_id].port_stats[task_id].tot_rx_pkt_count;
}

uint64_t stats_core_task_tot_tx(uint8_t lcore_id, uint8_t task_id)
{
	return lcore_stats[lcore_id].port_stats[task_id].tot_tx_pkt_count;
}

uint64_t stats_core_task_tot_drop(uint8_t lcore_id, uint8_t task_id)
{
	return lcore_stats[lcore_id].port_stats[task_id].tot_tx_pkt_drop;
}

uint64_t stats_core_task_last_tsc(uint8_t lcore_id, uint8_t task_id)
{
	return lcore_stats[lcore_id].port_stats[task_id].tsc[last_stat];
}

#define PORT_STATS_RX 0x01
#define PORT_STATS_TX 0x02


// Red: link down; Green: link up
static short link_color(const uint8_t if_port)
{
	return COLOR_PAIR(dppd_port_cfg[if_port].link_up? WHITE_ON_BLUE : WHITE_ON_RED);
}

static void init_core_port(struct core_port *core_port, uint8_t lcore_id, uint8_t port_id, struct task_stats *stats, uint8_t flags)
{
	core_port->lcore_id = lcore_id;
	core_port->port_id = port_id;
	core_port->stats = stats;

	core_port->port_stats = &lcore_stats[lcore_id].port_stats[port_id];
	core_port->flags |= flags;

	if (cqm.supported && lcore_stats[lcore_id].rmid == 0) {
		++cqm.last_rmid; // 0 not used (by default all cores are 0)
		plog_info("setting up rmid: %d\n", cqm.last_rmid);
		lcore_stats[lcore_id].rmid = cqm.last_rmid;
	}
}

static struct core_port *set_line_no(const uint8_t lcore_id, const uint8_t port_id)
{
	for (uint8_t active_core_port = 0; active_core_port < nb_tasks_tot; ++active_core_port) {
		struct core_port *core_port = &core_ports[active_core_port];
		if (lcore_id == core_port->lcore_id && port_id == core_port->port_id) {
			return core_port;
		}
	}
	return NULL;
}

static void init_active_eth_ports(void)
{
	nb_interface = rte_eth_dev_count();

	nb_active_interfaces = 0;
	for (uint8_t i = 0; i < nb_interface; ++i) {
		if (dppd_port_cfg[i].active) {
			nb_active_interfaces++;
		}
	}
}

static void init_mempools(void)
{
	n_mempools = 0;
	uint32_t n_max_mempools = sizeof(dppd_port_cfg[0].pool)/sizeof(dppd_port_cfg[0].pool[0]);
	for (uint8_t i = 0; i < DPPD_MAX_PORTS; ++i) {
		if (dppd_port_cfg[i].active && n_mempools < 64) {
			for (uint8_t j = 0; j < n_max_mempools; ++j) {
				if (dppd_port_cfg[i].pool[j] && dppd_port_cfg[i].pool_size[j]) {
					mempool_stats[n_mempools].pool = dppd_port_cfg[i].pool[j];
					mempool_stats[n_mempools].port = i;
					mempool_stats[n_mempools].queue = j;
					mempool_stats[n_mempools].size = dppd_port_cfg[i].pool_size[j];
					n_mempools++;
				}
			}
		}
	}
}

static void init_latency(void)
{
	struct lcore_cfg *lconf;
	uint32_t lcore_id = -1;

	n_latency = 0;

	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
			/* TODO: make this work with multiple ports
			   and with rings. Currently, only showing lat
			   tasks which have 1 RX port. */
			if (!strcmp(targ->task_init->mode_str, "lat") && targ->nb_rxports == 1) {
				task_lats[n_latency].task = (struct task_lat *)lconf->task[task_id];
				task_lats[n_latency].lcore_id = lcore_id;
				task_lats[n_latency].task_id = task_id;
				task_lats[n_latency].rx_port = targ->rx_ports[0];
				if (++n_latency == 64)
					return ;
			}
		}
	}
}

/* Populate active_core_ports for stats reporting, the order of the cores matters
   for reporting the most accurate results. TX cores should updated first (to prevent
   negative Loss stats). This will also calculate the number of core ports used by
   other display functions. */
static void init_active_core_ports(void)
{
	struct lcore_cfg *lconf;
	uint32_t lcore_id;

	/* add cores that are receiving from and sending to physical ports first */
	lcore_id = -1;
	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
			struct task_stats *stats = &lconf->task[task_id]->aux->stats;
			if (targ->nb_rxrings == 0 && targ->nb_txrings == 0) {
				init_core_port(&core_ports[nb_tasks_tot], lcore_id, task_id, stats, PORT_STATS_RX | PORT_STATS_TX);
				++nb_tasks_tot;
			}
		}
	}

	/* add cores that are sending to physical ports second */
	lcore_id = -1;
	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
			struct task_stats *stats = &lconf->task[task_id]->aux->stats;
			if (targ->nb_rxrings != 0 && targ->nb_txrings == 0) {
				init_core_port(&core_ports[nb_tasks_tot], lcore_id, task_id, stats, PORT_STATS_TX);
				++nb_tasks_tot;
			}
		}
	}

	/* add cores that are receiving from physical ports third */
	lcore_id = -1;
	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
			struct task_stats *stats = &lconf->task[task_id]->aux->stats;
			if (targ->nb_rxrings == 0 && targ->nb_txrings != 0) {
				init_core_port(&core_ports[nb_tasks_tot], lcore_id, task_id, stats, PORT_STATS_RX);
				++nb_tasks_tot;
			}
		}
	}

	/* add cores that are working internally (no physical ports attached) */
	lcore_id = -1;
	while(dppd_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->nb_tasks; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
			struct task_stats *stats = &lconf->task[task_id]->aux->stats;
			if (targ->nb_rxrings != 0 && targ->nb_txrings != 0) {
				init_core_port(&core_ports[nb_tasks_tot], lcore_id, task_id, stats, 0);
				++nb_tasks_tot;
			}
		}
	}
}

static void (*ncurses_sigwinch)(int);

static void sigwinch(int in)
{
	if (ncurses_sigwinch)
		ncurses_sigwinch(in);
	refresh();
	stats_display_layout(0);
}

static void set_signal_handler(void)
{
	struct sigaction old;

	sigaction(SIGWINCH, NULL, &old);
	ncurses_sigwinch = old.sa_handler;

	signal(SIGWINCH, sigwinch);
}

void display_init(unsigned avg_start, unsigned duration)
{
	scr = initscr();
	start_color();
	/* Assign default foreground/background colors to color number -1 */
	use_default_colors();

	init_pair(WHITE_ON_BLUE,   COLOR_WHITE,  COLOR_BLUE);
	init_pair(RED_ON_BLACK,     COLOR_RED,  COLOR_BLACK);
	init_pair(BLACK_ON_CYAN,   COLOR_BLACK,  COLOR_CYAN);
	init_pair(BLACK_ON_WHITE,  COLOR_BLACK,  COLOR_WHITE);
	init_pair(BLACK_ON_YELLOW, COLOR_BLACK,  COLOR_YELLOW);
	init_pair(WHITE_ON_RED,    COLOR_WHITE,  COLOR_RED);

	wbkgd(scr, COLOR_PAIR(WHITE_ON_BLUE));

	/* nodelay(scr, TRUE); */
	noecho();

	/* Create fullscreen log window. When stats are displayed
	   later, it is recreated with appropriate dimensions. */
	win_txt = create_subwindow(0, 0, 0, 0);
	wbkgd(win_txt, COLOR_PAIR(0));

	idlok(win_txt, FALSE);
	/* Get scrolling */
	scrollok(win_txt, TRUE);
	/* Leave cursor where it was */
	leaveok(win_txt, TRUE);

	refresh();

	set_signal_handler();

	core_port_height = (LINES - 5 - 2 - 3);
	if (core_port_height > nb_tasks_tot) {
		core_port_height = nb_tasks_tot;
	}
	start_tsc = rte_rdtsc();
	beg_tsc = start_tsc;
	/* + 1 for rounding */
	end_tsc = duration? start_tsc + (avg_start + duration + 1) * rte_get_tsc_hz() : 0;

	global_stats.avg_start = start_tsc + avg_start*rte_get_tsc_hz();
	stats_update();

	stats_display_layout(0);
}

static void stats_display_latency(void)
{
	display_lock();

	wattron(win_stat, A_BOLD);
	/* Labels */
	mvwaddstrf(win_stat, 3, 0,   "Core");
	mvwaddstrf(win_stat, 4, 0,   "  Nb");
	mvwvline(win_stat, 4, 4,  ACS_VLINE, n_latency + 2);
	mvwaddstrf(win_stat, 3, 5, " Port Nb");
	mvwaddstrf(win_stat, 4, 5, "  RX");

	mvwaddstrf(win_stat, 3, 31, "Measured Latency");

	mvwvline(win_stat, 4, 13,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 14, "  Min (us)");
	mvwvline(win_stat, 4, 26,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 27, "  Max (us)");
	mvwvline(win_stat, 4, 39,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 40, "  Avg (us)");
	mvwvline(win_stat, 4, 52,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 53, " STDDev (us)");
	mvwvline(win_stat, 4, 65,  ACS_VLINE, n_mempools + 1);

	wattroff(win_stat, A_BOLD);

	for (uint16_t i = 0; i < n_latency; ++i) {
		mvwaddstrf(win_stat, 5 + i, 0, "%2u/%1u",
			   task_lats[i].lcore_id,
			   task_lats[i].task_id);

		mvwaddstrf(win_stat, 5 + i, 8, "%u",
			   task_lats[i].rx_port);
	}

	display_unlock();
}

static void stats_display_mempools(void)
{
	display_lock();

	wattron(win_stat, A_BOLD);
	/* Labels */
	mvwaddstrf(win_stat, 3, 2,   "Port");
	mvwaddstrf(win_stat, 4, 0,   "  Nb");
	mvwvline(win_stat, 4, 4,  ACS_VLINE, n_mempools + 2);
	mvwaddstrf(win_stat, 4, 5,   "Queue");

	mvwvline(win_stat, 2, 10,  ACS_VLINE, n_mempools + 3);
	mvwaddstrf(win_stat, 3, 50, "Sampled statistics");
	mvwaddstrf(win_stat, 4, 11, "Occup (%%)");
	mvwvline(win_stat, 4, 20,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 21, "    Used (#)");
	mvwvline(win_stat, 4, 33,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 34, "    Free (#)");
	mvwvline(win_stat, 4, 46,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 47, "   Total (#)");
	mvwvline(win_stat, 4, 59,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 60, " Mem Used (KB)");
	mvwvline(win_stat, 4, 74,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 75, " Mem Free (KB)");
	mvwvline(win_stat, 4, 89,  ACS_VLINE, n_mempools + 1);
	mvwaddstrf(win_stat, 4, 90, " Mem Tot  (KB)");
	mvwvline(win_stat, 3, 104,  ACS_VLINE, n_mempools + 2);
	wattroff(win_stat, A_BOLD);

	for (uint16_t i = 0; i < n_mempools; ++i) {
		mvwaddstrf(win_stat, 5 + i, 0, "%4u", mempool_stats[i].port);
		mvwaddstrf(win_stat, 5 + i, 5, "%5u", mempool_stats[i].queue);
		mvwaddstrf(win_stat, 5 + i, 47, "%12zu", mempool_stats[i].size);
		mvwaddstrf(win_stat, 5 + i, 90, "%14zu", mempool_stats[i].size * MBUF_SIZE/1024);
	}

	display_unlock();
}

static void stats_display_eth_ports(void)
{
	display_lock();
	wattron(win_stat, A_BOLD);
	/* Labels */
	mvwaddstrf(win_stat, 3, 2,   "Port");
	mvwaddstrf(win_stat, 4, 0,   "  Nb");
	mvwvline(win_stat, 4, 4,  ACS_VLINE, nb_active_interfaces + 2);
	mvwaddstrf(win_stat, 4, 5,   "Name");

	mvwvline(win_stat, 2, 13,  ACS_VLINE, nb_active_interfaces + 3);
	mvwaddstrf(win_stat, 3, 14, "                        Statistics per second");
	mvwaddstrf(win_stat, 4, 14, "no mbufs (#)");
	mvwvline(win_stat, 4, 26,  ACS_VLINE, nb_active_interfaces + 1);
	mvwaddstrf(win_stat, 4, 27, "ierrors (#)");
	mvwvline(win_stat, 4, 39,  ACS_VLINE, nb_active_interfaces + 1);

	mvwaddstrf(win_stat, 4, 40, "RX (Kpps)");
	mvwvline(win_stat, 4, 49,  ACS_VLINE, nb_active_interfaces + 1);
	mvwaddstrf(win_stat, 4, 50, "TX (Kpps)");
	mvwvline(win_stat, 4, 59,  ACS_VLINE, nb_active_interfaces + 1);
	mvwaddstrf(win_stat, 4, 60, " RX (bps)");
	mvwvline(win_stat, 4, 69,  ACS_VLINE, nb_active_interfaces + 1);
	mvwaddstrf(win_stat, 4, 70, " TX (bps)");
	mvwvline(win_stat, 3, 79,  ACS_VLINE, nb_active_interfaces + 2);

	mvwaddstrf(win_stat, 3, 80, "                          Total Statistics");
	mvwaddstrf(win_stat, 4, 80, "               RX");
	mvwvline(win_stat, 4, 97,  ACS_VLINE, nb_active_interfaces + 1);
	mvwaddstrf(win_stat, 4, 98, "              TX");
	mvwvline(win_stat, 4, 114,  ACS_VLINE, nb_active_interfaces + 1);
	mvwaddstrf(win_stat, 4, 115, "    no mbufs (#)");
	mvwvline(win_stat, 4, 131,  ACS_VLINE, nb_active_interfaces + 1);
	mvwaddstrf(win_stat, 4, 132, "     ierrors (#)");
	mvwvline(win_stat, 3, 148,  ACS_VLINE, nb_active_interfaces + 2);

	wattroff(win_stat, A_BOLD);
	uint8_t count = 0;
	for (uint8_t i = 0; i < nb_interface; ++i) {
		if (dppd_port_cfg[i].active) {
			mvwaddstrf(win_stat, 5 + count, 0, "%4u", i);
			mvwaddstrf(win_stat, 5 + count, 5, "%8s", dppd_port_cfg[i].name);
			count++;
		}
	}
	display_unlock();
}

static void stats_display_core_ports(unsigned chosen_page)
{
	display_lock();
	if (msr_support) {
		col_offset = 20;
	}
	/* Sub-section separator lines */
	mvwvline(win_stat, 4,  4,  ACS_VLINE, nb_tasks_tot + 1);
	mvwvline(win_stat, 4, 13,  ACS_VLINE, nb_tasks_tot + 1);
	mvwvline(win_stat, 4, 33,  ACS_VLINE, nb_tasks_tot + 1);
	mvwvline(win_stat, 4, 53,  ACS_VLINE, nb_tasks_tot + 1);
	mvwvline(win_stat, 4, 63,  ACS_VLINE, nb_tasks_tot + 1);
	mvwvline(win_stat, 4, 73,  ACS_VLINE, nb_tasks_tot + 1);
	if (msr_support){
		mvwvline(win_stat, 4, 83,  ACS_VLINE, nb_tasks_tot + 1);
		mvwvline(win_stat, 4, 93,  ACS_VLINE, nb_tasks_tot + 1);
	}
	mvwvline(win_stat, 4, 98 + col_offset,  ACS_VLINE, nb_tasks_tot + 1);
	mvwvline(win_stat, 4, 113 + col_offset, ACS_VLINE, nb_tasks_tot + 1);
	if (cqm.supported) {
		mvwvline(win_stat, 4, 143 + col_offset, ACS_VLINE, nb_tasks_tot + 1);
	}

	wattron(win_stat, A_BOLD);
	/* Section separators (bold) */
	mvwvline(win_stat, 3, 23, ACS_VLINE, nb_tasks_tot + 2);
	mvwvline(win_stat, 3, 44, ACS_VLINE, nb_tasks_tot + 2);
	mvwvline(win_stat, 3, 83 + col_offset, ACS_VLINE, nb_tasks_tot + 2);
	if (cqm.supported) {
		mvwvline(win_stat, 3, 118 + col_offset, ACS_VLINE, nb_tasks_tot + 2);
	}

	/* Labels */
	mvwaddstrf(win_stat, 3, 8,   "Core/Task");
	mvwaddstrf(win_stat, 4, 0,   "  Nb");
	mvwaddstrf(win_stat, 4, 5,   "Name");
	mvwaddstrf(win_stat, 4, 14,  "Mode     ");

	mvwaddstrf(win_stat, 3, 24, " Port ID/Ring Name");
	mvwaddstrf(win_stat, 4, 24, "       RX");
	mvwaddstrf(win_stat, 4, 34, "        TX");

	if (!msr_support) {
		mvwaddstrf(win_stat, 3, 45, "        Statistics per second         ");
	}
	else {
		mvwaddstrf(win_stat, 3, 45, "                  Statistics per second                   ");
	}
	mvwaddstrf(win_stat, 4, 45, "%s", "Idle (%)");
	mvwaddstrf(win_stat, 4, 54, "   RX (k)");
	mvwaddstrf(win_stat, 4, 64, "   TX (k)");
	mvwaddstrf(win_stat, 4, 74, " Drop (k)");
	if (msr_support) {
		mvwaddstrf(win_stat, 4, 84, "      CPP");
		mvwaddstrf(win_stat, 4, 94, "Clk (GHz)");
	}

	mvwaddstrf(win_stat, 3, 84 + col_offset, "              Total Statistics             ");
	mvwaddstrf(win_stat, 4, 84 + col_offset, "            RX");
	mvwaddstrf(win_stat, 4, 99 + col_offset, "            TX");
	mvwaddstrf(win_stat, 4, 114 + col_offset, "          Drop");


	if (cqm.supported) {
		mvwaddstrf(win_stat, 3, 129 + col_offset, "  Cache QoS Monitoring  ");
		mvwaddstrf(win_stat, 4, 129 + col_offset, "occupancy (KB)");
		mvwaddstrf(win_stat, 4, 144 + col_offset, " fraction");
	}
	wattroff(win_stat, A_BOLD);

	uint16_t line_no = 0;
	uint32_t lcore_id = -1;
	while(dppd_core_next(&lcore_id, 0) == 0) {
		const struct lcore_cfg *const cur_core = &lcore_cfg[lcore_id];

		for (uint8_t task_id = 0; task_id < cur_core->nb_tasks; ++task_id) {
			const struct task_args *const targ = &cur_core->targs[task_id];

			if (line_no >= core_port_height * chosen_page && line_no < core_port_height * (chosen_page + 1)) {
				// Core number and name
				if ((cur_core->flags & PCFG_TERMINATE))
					wbkgdset(win_stat, COLOR_PAIR(RED_ON_BLACK));
				if (task_id == 0)
					mvwaddstrf(win_stat, line_no % core_port_height + 5, 0, "%2u/%1u", lcore_id, task_id);
				else
					mvwaddstrf(win_stat, line_no % core_port_height + 5, 0, "   %1u", task_id);
				if ((cur_core->flags & PCFG_TERMINATE))
					wbkgdset(win_stat, COLOR_PAIR(WHITE_ON_BLUE));
				mvwaddstrf(win_stat, line_no % core_port_height + 5, 5, "%s", task_id == 0 ? cur_core->name : "");
				mvwaddstrf(win_stat, line_no % core_port_height + 5, 14, "%.9s", targ->task_init->mode_str);
				if (strlen(targ->task_init->mode_str) > 9)
					mvwaddstrf(win_stat, line_no % core_port_height + 5, 22 , "~");
				// Rx port information
				if (targ->nb_rxrings == 0) {
					for (int i=0;i<targ->nb_rxports;i++) {
						wattron(win_stat, A_BOLD);
						wbkgdset(win_stat, link_color(targ->rx_ports[i]));
						mvwaddstrf(win_stat, line_no % core_port_height + 5, 26+3*i, "%2u", targ->rx_ports[i]);
						wbkgdset(win_stat, COLOR_PAIR(WHITE_ON_BLUE));
						wattroff(win_stat, A_BOLD);
					}
				}
				uint8_t ring_id;
				for (ring_id = 0; ring_id < targ->nb_rxrings && ring_id < 9; ++ring_id) {
					mvwaddstrf(win_stat, line_no % core_port_height + 5, 24 + ring_id, "%s", targ->rx_rings[ring_id]->name);
				}
				if (ring_id == 9 && ring_id < targ->nb_rxrings) {
					mvwaddstrf(win_stat, line_no % core_port_height + 5, 24 + ring_id -1 , "~");
				}
				// Tx port information
				if (targ->runtime_flags & TASK_ROUTING) {
					wattron(win_stat, A_BOLD);
					wbkgdset(win_stat, COLOR_PAIR(WHITE_ON_BLUE));
					mvwaddstrf(win_stat, line_no % core_port_height + 5, 34, "r:");
					uint8_t pos = 36;
					for (uint8_t i = 0; i < targ->nb_txports; ++i) {
						if (i) {
							if (pos - 36>= 9) {
								mvwaddstrf(win_stat, line_no % core_port_height + 5, pos -1, "~");
								break;
							}
							mvwaddstrf(win_stat, line_no % core_port_height + 5, pos, ",");
							++pos;
						}
						if (pos -36>= 10) {
							mvwaddstrf(win_stat, line_no % core_port_height + 5, pos -1, "~");
							break;
						}

						wbkgdset(win_stat, link_color(targ->tx_port_queue[i].port));
						if (targ->tx_port_queue[i].port > 9 && pos -36>= 7) {
							mvwaddstrf(win_stat, line_no % core_port_height + 5, pos -1, "~");
							break;
						}
						mvwaddstrf(win_stat, line_no % core_port_height + 5, pos, "%u", targ->tx_port_queue[i].port);
						wbkgdset(win_stat, COLOR_PAIR(WHITE_ON_BLUE));
						++pos;
					}
					uint8_t ring_id;
					for (ring_id = 0; ring_id < targ->nb_txrings && targ->nb_txrings < 10; ++ring_id) {
						mvwaddstrf(win_stat, line_no % core_port_height + 5, pos++, "%s", targ->tx_rings[ring_id]->name);
					}
					if (ring_id == 10 && ring_id < targ->nb_txrings) {
						mvwaddstrf(win_stat, line_no % core_port_height + 5, pos -1, "~");
					}
					wattroff(win_stat, A_BOLD);
					wbkgdset(win_stat, COLOR_PAIR(WHITE_ON_BLUE));
				}
				else {
					uint8_t pos = 36;
					for (uint8_t i = 0; i < targ->nb_txports; ++i) {
						if (i) {
							if (pos - 36 >= 9) {
								mvwaddstrf(win_stat, line_no % core_port_height + 5, pos -1, "~");
								break;
							}
							mvwaddstrf(win_stat, line_no % core_port_height + 5, pos, ",");
							++pos;
						}

						if (pos - 36 >= 10) {
							mvwaddstrf(win_stat, line_no % core_port_height + 5, pos -1, "~");
							break;
						}
						wbkgdset(win_stat, link_color(targ->tx_port_queue[i].port));
						mvwaddstrf(win_stat, line_no % core_port_height + 5, pos, "%u", targ->tx_port_queue[i].port);
						wbkgdset(win_stat, COLOR_PAIR(WHITE_ON_BLUE));
						pos++;
					}
					uint8_t ring_id;
					for (ring_id = 0; ring_id < targ->nb_txrings && ring_id < 10; ++ring_id) {
						mvwaddstrf(win_stat, line_no % core_port_height + 5, 34 + ring_id, "%s", targ->tx_rings[ring_id]->name);
					}
					if (ring_id == 10 && ring_id < targ->nb_txrings)
						mvwaddstrf(win_stat, line_no % core_port_height + 5, 34 + ring_id-1, "~");
				}
			}
			DPPD_ASSERT(line_no < RTE_MAX_LCORE*MAX_TASKS_PER_CORE);
			core_port_ordered[line_no] = set_line_no(lcore_id, task_id);
			++line_no;
		}
	}
	display_unlock();
}

static void stats_display_layout(uint8_t in_place)
{
	uint8_t cur_stats_height;

	switch (screen_state.chosen_screen) {
	case 0:
		cur_stats_height = core_port_height;
		break;
	case 1:
		cur_stats_height = nb_active_interfaces;
		break;
	case 2:
		cur_stats_height = n_mempools;
		break;
	case 3:
		cur_stats_height = n_latency;
		break;
	default:
		cur_stats_height = core_port_height;
	}

	display_lock();
	if (!in_place) {
		// moving existing windows does not work
		delwin(win_txt);
		delwin(win_title);
		delwin(win_cmd);
		delwin(win_txt);
		delwin(win_help);

		clear();
	}

	if (!in_place) {
		win_stat = create_subwindow(cur_stats_height + 5, 0, 0, 0);
		win_title = create_subwindow(3, 41, 0, 41);
		win_cmd = create_subwindow(1, 0, cur_stats_height + 5,  0);
		win_txt = create_subwindow(LINES - cur_stats_height - 5 - 2, 0, cur_stats_height + 5 + 1, 0);
		win_help = create_subwindow(1, 0, LINES - 1, 0);
	}
	/* Title box */
	wbkgd(win_title, COLOR_PAIR(BLACK_ON_CYAN));
	box(win_title, 0, 0);
	char title_str[40];
	snprintf(title_str, 40, "%s %s: %s", PROGRAM_NAME, VERSION_STR, dppd_cfg.name);

	mvwaddstrf(win_title, 1, (40 - strlen(title_str))/2, "%s", title_str);

	/* Stats labels and separator lines */
	/* Upper left stats block */
	mvwaddstrf(win_stat, 0, 83, "Tot Time:");
	mvwaddstrf(win_stat, 1, 83, "Time:");
	mvwaddstrf(win_stat, 2, 83, "Remaining:");

	mvwaddstrf(win_stat, 0, 0, "%%:");
	mvwaddstrf(win_stat, 0, 15, "Tot %%:");
	mvwaddstrf(win_stat, 1, 0, "Rx: %10s pps (%10s avg)", "", "");
	mvwaddstrf(win_stat, 2, 0, "Tx: %10s pps (%10s avg)", "", "");

	/* Upper right stats block */
	mvwaddstrf(win_stat, 0, 100, "Rx:");
	mvwaddstrf(win_stat, 1, 100, "Tx:");
	mvwaddstrf(win_stat, 2, 100, "Diff:");

	/* Command line */
	wbkgd(win_cmd, COLOR_PAIR(BLACK_ON_YELLOW));
	idlok(win_cmd, FALSE);
	/* Move cursor at insertion point */
	leaveok(win_cmd, FALSE);

	/* Help/status bar */
	wbkgd(win_help, COLOR_PAIR(BLACK_ON_WHITE));
	werase(win_help);
	waddstr(win_help, "Enter 'help' or command, <ESC> or 'quit' to exit, 1-4 to switch screens and 0 to reset stats");
	wrefresh(win_help);
	mvwin(win_help, LINES - 1, 0);
	/* Log window */
	idlok(win_txt, FALSE);
	/* Get scrolling */
	scrollok(win_txt, TRUE);

	/* Leave cursor where it was */
	leaveok(win_txt, TRUE);

	wbkgd(win_txt, COLOR_PAIR(BLACK_ON_CYAN));
	wrefresh(win_txt);

	/* Draw everything to the screen */
	refresh();
	display_unlock();


	switch (screen_state.chosen_screen) {
	case 0:
		stats_display_core_ports(screen_state.chosen_page);
		break;
	case 1:
		stats_display_eth_ports();
		break;
	case 2:
		stats_display_mempools();
		break;
	case 3:
		stats_display_latency();
		break;
	}

	refresh_cmd_win();
	display_stats();
}

void display_end(void)
{
	pthread_mutex_destroy(&disp_mtx);

	if (scr != NULL) {
		endwin();
	}
}

static void update_global_stats(uint8_t task_id, struct global_stats *global_stats)
{
	const struct port_stats *port_stats = core_ports[task_id].port_stats;
	const uint64_t delta_t = port_stats->tsc[last_stat] - port_stats->tsc[!last_stat];
	uint64_t diff;

	if (core_ports[task_id].flags & PORT_STATS_RX) {
		diff = port_stats->rx_pkt_count[last_stat] - port_stats->rx_pkt_count[!last_stat];
		global_stats->rx_tot += diff;
		global_stats->rx_pps += diff * rte_get_tsc_hz() / delta_t;
	}

	if (core_ports[task_id].flags & PORT_STATS_TX) {
		diff = port_stats->tx_pkt_count[last_stat] - port_stats->tx_pkt_count[!last_stat];
		global_stats->tx_tot += diff;
		global_stats->tx_pps += diff * rte_get_tsc_hz() / delta_t;
	}

	global_stats->last_tsc = RTE_MAX(global_stats->last_tsc, port_stats->tsc[last_stat]);
}

static void display_core_port_stats(uint8_t task_id)
{
	const int line_no = task_id % core_port_height;

	const struct port_stats *port_stats = core_port_ordered[task_id]->port_stats;

	/* delta_t in units of clock ticks */
	uint64_t delta_t = port_stats->tsc[last_stat] - port_stats->tsc[!last_stat];

	uint64_t empty_cycles = port_stats->empty_cycles[last_stat] - port_stats->empty_cycles[!last_stat];

	if (empty_cycles > delta_t) {
		empty_cycles = 10000;
	}
	else {
		empty_cycles = empty_cycles * 10000 / delta_t;
	}

	// empty_cycles has 2 digits after point, (usefull when only a very small idle time)
	mvwaddstrf(win_stat, line_no + 5, 47, "%3lu.%02lu", empty_cycles / 100, empty_cycles % 100);

	// Display per second statistics in Kpps unit
	delta_t *= 1000;

	uint64_t nb_pkt;
	nb_pkt = (port_stats->rx_pkt_count[last_stat] - port_stats->rx_pkt_count[!last_stat]) * rte_get_tsc_hz();
	if (nb_pkt && nb_pkt < delta_t) {
		mvwaddstrf(win_stat, line_no + 5, 54, "    0.%03lu", nb_pkt * 1000 / delta_t);
	}
	else {
		mvwaddstrf(win_stat, line_no + 5, 54, "%9lu", nb_pkt / delta_t);
	}

	nb_pkt = (port_stats->tx_pkt_count[last_stat] - port_stats->tx_pkt_count[!last_stat]) * rte_get_tsc_hz();
	if (nb_pkt && nb_pkt < delta_t) {
		mvwaddstrf(win_stat, line_no + 5, 64, "    0.%03lu", nb_pkt * 1000 / delta_t);
	}
	else {
		mvwaddstrf(win_stat, line_no + 5, 64, "%9lu", nb_pkt / delta_t);
	}

	nb_pkt = (port_stats->tx_pkt_drop[last_stat] - port_stats->tx_pkt_drop[!last_stat]) * rte_get_tsc_hz();
	if (nb_pkt && nb_pkt < delta_t) {
		mvwaddstrf(win_stat, line_no + 5, 74, "    0.%03lu", nb_pkt * 1000 / delta_t);
	}
	else {
		mvwaddstrf(win_stat, line_no + 5, 74, "%9lu", nb_pkt / delta_t);
	}

	if (msr_support) {
		uint8_t lcore_id = core_port_ordered[task_id]->lcore_id;
		uint64_t adiff = lcore_stats[lcore_id].afreq[last_stat] - lcore_stats[lcore_id].afreq[!last_stat];
		uint64_t mdiff = lcore_stats[lcore_id].mfreq[last_stat] - lcore_stats[lcore_id].mfreq[!last_stat];

		if ((port_stats->rx_pkt_count[last_stat] - port_stats->rx_pkt_count[!last_stat]) && mdiff) {
			mvwaddstrf(win_stat, line_no + 5, 84, "%9lu", delta_t/(port_stats->rx_pkt_count[last_stat] - port_stats->rx_pkt_count[!last_stat])*adiff/mdiff/1000);
		}
		else {
			mvwaddstrf(win_stat, line_no + 5, 84, "%9lu", 0L);
		}

		uint64_t mhz;
		if (mdiff)
			mhz = rte_get_tsc_hz()*adiff/mdiff/1000000;
		else
			mhz = 0;

		mvwaddstrf(win_stat, line_no + 5, 94, "%5lu.%03lu", mhz/1000, mhz%1000);
	}

	// Total statistics (packets)
	mvwaddstrf(win_stat, line_no + 5, 84 + col_offset, "%14lu", port_stats->tot_rx_pkt_count);
	mvwaddstrf(win_stat, line_no + 5, 99 + col_offset, "%14lu", port_stats->tot_tx_pkt_count);
	mvwaddstrf(win_stat, line_no + 5, 114 + col_offset, "%14lu", port_stats->tot_tx_pkt_drop);

	if (cqm.supported) {
		uint8_t lcore_id = core_port_ordered[task_id]->lcore_id;
		mvwaddstrf(win_stat, line_no + 5, 129 + col_offset, "%14lu", lcore_stats[lcore_id].cqm_bytes >> 10);
		mvwaddstrf(win_stat, line_no + 5, 144 + col_offset, "%6lu.%02lu", lcore_stats[lcore_id].cqm_fraction/100, lcore_stats[lcore_id].cqm_fraction%100);
	}
}

void stats_init(void)
{
	init_active_core_ports();
	init_active_eth_ports();
	init_mempools();
	init_latency();

	if ((msr_support = !msr_init()) == 0) {
		plog_warn("Failed to open msr pseudo-file (missing msr kernel module?)\n");
	}

	if (cqm_is_supported()) {
		if (!msr_support) {
			plog_warn("CPU supports CQM but msr module not loaded. Disabling CQM stats.\n");
		}
		else {
			if (0 != cqm_get_features(&cqm.features)) {
				plog_warn("Failed to get CQM features\n");
				cqm.supported = 0;
			}
			else {
				cqm_init_stat_core(rte_lcore_id());
				cqm.supported = 1;
			}

			for (uint8_t i = 0; i < RTE_MAX_LCORE; ++i) {
				cqm_assoc(i, lcore_stats[i].rmid);
			}
		}
	}
}

void stats_update(void)
{
	/* Keep track of last 2 measurements. */
	last_stat = !last_stat;

	if (nb_tasks_tot == 0) {
		return;
	}

	for (uint8_t task_id = 0; task_id < nb_tasks_tot; ++task_id) {
		struct task_stats *stats = core_ports[task_id].stats;
		struct port_stats *cur_port_stats = core_ports[task_id].port_stats;

		/* Read TX first and RX second, in order to prevent displaying
		   a negative packet loss. Depending on the configuration
		   (when forwarding, for example), TX might be bigger than RX. */
		cur_port_stats->tsc[last_stat] = rte_rdtsc();
		cur_port_stats->tx_pkt_count[last_stat] = rte_atomic32_read(&stats->tx_pkt_count);
		cur_port_stats->tx_pkt_drop[last_stat]  = rte_atomic32_read(&stats->tx_pkt_drop);
		cur_port_stats->rx_pkt_count[last_stat] = rte_atomic32_read(&stats->rx_pkt_count);
		cur_port_stats->empty_cycles[last_stat] = rte_atomic32_read(&stats->empty_cycles);
	}

	if (msr_support) {
		for (uint8_t lcore_id = 0; lcore_id < RTE_MAX_LCORE; ++lcore_id) {
			if (lcore_stats[lcore_id].rmid) {
				cqm_read_ctr(&lcore_stats[lcore_id].cqm_data, lcore_stats[lcore_id].rmid);
			}
			msr_read(&lcore_stats[lcore_id].afreq[last_stat], lcore_id, 0xe8);
			msr_read(&lcore_stats[lcore_id].mfreq[last_stat], lcore_id, 0xe7);
		}
	}

	uint64_t cqm_data_core0 = 0;
	cqm_read_ctr(&cqm_data_core0, 0);

	struct rte_eth_stats eth_stat;
	for (uint8_t port_id = 0; port_id < nb_interface; ++port_id) {
		if (dppd_port_cfg[port_id].active) {
			rte_eth_stats_get(port_id, &eth_stat);
			eth_stats[port_id].tsc[last_stat] = rte_rdtsc();
			eth_stats[port_id].no_mbufs[last_stat] = eth_stat.rx_nombuf;
			eth_stats[port_id].ierrors[last_stat] = eth_stat.ierrors;
			eth_stats[port_id].rx_tot[last_stat] = eth_stat.ipackets;
			eth_stats[port_id].tx_tot[last_stat] = eth_stat.opackets;
			eth_stats[port_id].rx_bytes[last_stat] = eth_stat.ibytes;
			eth_stats[port_id].tx_bytes[last_stat] = eth_stat.obytes;
		}
	}

	for (uint8_t mp_id = 0; mp_id < n_mempools; ++mp_id) {
		/* Note: The function free_count returns the number of used entries. */
		mempool_stats[mp_id].free = rte_mempool_count(mempool_stats[mp_id].pool);
	}

	for (uint16_t i = 0; i < n_latency; ++i) {
		struct task_lat *task_lat = task_lats[i].task;

		if (task_lat->use_lt != task_lat->using_lt)
			continue;

		struct lat_test *lat_test = &task_lat->lt[!task_lat->using_lt];
		if (lat_test->tot_pkts) {
			memcpy(&lat_stats[i], lat_test, sizeof(struct lat_test));
		}

		lat_test->tot_lat = 0;
		lat_test->var_lat = 0;
		lat_test->tot_pkts = 0;
		lat_test->max_lat = 0;
		lat_test->min_lat = -1;
		memset(lat_test->buckets, 0, sizeof(lat_test->buckets));
		task_lat->use_lt = !task_lat->using_lt;
	}


	for (uint8_t task_id = 0; task_id < nb_tasks_tot; ++task_id) {
		struct port_stats *cur_port_stats = core_ports[task_id].port_stats;

		/* no total stats for empty loops */
		cur_port_stats->tot_rx_pkt_count  += cur_port_stats->rx_pkt_count[last_stat] - cur_port_stats->rx_pkt_count[!last_stat];
		cur_port_stats->tot_tx_pkt_count  += cur_port_stats->tx_pkt_count[last_stat] - cur_port_stats->tx_pkt_count[!last_stat];
		cur_port_stats->tot_tx_pkt_drop   += cur_port_stats->tx_pkt_drop[last_stat] - cur_port_stats->tx_pkt_drop[!last_stat];
	}

	global_stats.tx_pps = 0;
	global_stats.rx_pps = 0;
	for (uint8_t task_id = 0; task_id < nb_tasks_tot; ++task_id) {
		update_global_stats(task_id, &global_stats);
	}

	for (uint8_t port_id = 0; port_id < nb_interface; ++port_id) {
		global_stats.tot_ierrors += eth_stats[port_id].ierrors[last_stat] - eth_stats[port_id].ierrors[!last_stat];
	}

	if (cqm.supported) {
		// update CQM stats (calucate fraction and bytes reported) */
		uint64_t total_monitored = cqm_data_core0*cqm.features.upscaling_factor;

		for (uint8_t lcore_id = 0; lcore_id < RTE_MAX_LCORE; ++lcore_id) {
			if (lcore_stats[lcore_id].rmid) {
				lcore_stats[lcore_id].cqm_bytes = lcore_stats[lcore_id].cqm_data*cqm.features.upscaling_factor;
				total_monitored += lcore_stats[lcore_id].cqm_bytes;
			}
		}
		for (uint8_t lcore_id = 0; lcore_id < RTE_MAX_LCORE; ++lcore_id) {
			if (lcore_stats[lcore_id].rmid && total_monitored) {
				lcore_stats[lcore_id].cqm_fraction = lcore_stats[lcore_id].cqm_bytes*10000/total_monitored;
			}
			else
				lcore_stats[lcore_id].cqm_fraction = 0;
		}
	}

	if (global_stats.last_tsc > global_stats.avg_start) {
		if (!global_stats.started_avg) {
			global_stats.rx_tot_beg = global_stats.rx_tot;
			global_stats.tx_tot_beg = global_stats.tx_tot;
			global_stats.started_avg = 1;
			global_stats.avg_start = global_stats.last_tsc;
		}
		else {
			uint64_t avg_tsc_passed = global_stats.last_tsc - global_stats.avg_start;
			uint64_t thresh = ((uint64_t)-1)/rte_get_tsc_hz();
			/* Use only precise arithmetic when there is no overflow */
			if (global_stats.rx_tot - global_stats.rx_tot_beg < thresh) {
				global_stats.rx_avg = (global_stats.rx_tot - global_stats.rx_tot_beg)*rte_get_tsc_hz()/avg_tsc_passed;
			}
			else {
				global_stats.rx_avg = (global_stats.rx_tot - global_stats.rx_tot_beg)/(avg_tsc_passed/rte_get_tsc_hz());
			}

			if (global_stats.tx_tot - global_stats.tx_tot_beg < thresh) {
				global_stats.tx_avg = (global_stats.tx_tot - global_stats.tx_tot_beg)*rte_get_tsc_hz()/avg_tsc_passed;
			}
			else {
				global_stats.tx_avg = (global_stats.tx_tot - global_stats.tx_tot_beg)/(avg_tsc_passed/rte_get_tsc_hz());
			}
		}
	}
}

static void display_stats_general(void)
{
	/* moment when stats were gathered. */
	uint64_t cur_tsc = global_stats.last_tsc;

	// Upper left stats block
	wattron(win_stat, A_BOLD);
	mvwaddstrf(win_stat, 0, 94, "%4lu", (cur_tsc - beg_tsc)/rte_get_tsc_hz());
	mvwaddstrf(win_stat, 1, 94, "%4lu", (cur_tsc - start_tsc)/rte_get_tsc_hz());
	if (end_tsc)
		mvwaddstrf(win_stat, 2, 94, "%4lu", end_tsc > cur_tsc? (end_tsc - cur_tsc)/rte_get_tsc_hz() : 0);
	else
		mvwaddstrf(win_stat, 2, 95, "N/A");

	mvwaddstrf(win_stat, 0, 6, "%8.4f", global_stats.tx_pps > global_stats.rx_pps?
		   100 : global_stats.tx_pps * 100.0 / global_stats.rx_pps);

	mvwaddstrf(win_stat, 0, 22, "%8.4f", global_stats.tx_tot > global_stats.rx_tot?
		   100 : global_stats.tx_tot * 100.0 / global_stats.rx_tot);

	mvwaddstrf(win_stat, 1, 4, "%10lu", global_stats.rx_pps);
	mvwaddstrf(win_stat, 2, 4, "%10lu", global_stats.tx_pps);

	if (global_stats.started_avg) {
		mvwaddstrf(win_stat, 1,  20, "%10lu", global_stats.rx_avg);
		mvwaddstrf(win_stat, 2,  20, "%10lu", global_stats.tx_avg);
	}

	uint64_t rx_tot_ierror = global_stats.rx_tot + global_stats.tot_ierrors;
	mvwaddstrf(win_stat, 0, 31, "[%8.4f]", global_stats.tx_tot > rx_tot_ierror? 100:
		   global_stats.tx_tot * 100.0 / rx_tot_ierror);

	// Upper right stats block
	mvwaddstrf(win_stat, 0, 106, "%12lu", global_stats.rx_tot);
	mvwaddstrf(win_stat, 1, 106, "%12lu", global_stats.tx_tot);

	uint64_t loss = 0;
	if (global_stats.rx_tot > global_stats.tx_tot)
		loss = global_stats.rx_tot - global_stats.tx_tot;
	mvwaddstrf(win_stat, 2, 106, "%12lu", loss);

	wattroff(win_stat, A_BOLD);
}

static void display_stats_core_ports(void)
{
	unsigned chosen_page = screen_state.chosen_page;

	for (uint8_t active_core = core_port_height * chosen_page; active_core < nb_tasks_tot && active_core < core_port_height * (chosen_page + 1); ++active_core) {
		display_core_port_stats(active_core);
	}
}

int stats_port(uint8_t port_id, struct get_port_stats *ps)
{
	if (!dppd_port_cfg[port_id].active)
		return -1;

	ps->no_mbufs_diff = eth_stats[port_id].no_mbufs[last_stat] - eth_stats[port_id].no_mbufs[!last_stat];
	ps->ierrors_diff = eth_stats[port_id].ierrors[last_stat] - eth_stats[port_id].ierrors[!last_stat];
	ps->rx_bytes_diff = eth_stats[port_id].rx_bytes[last_stat] - eth_stats[port_id].rx_bytes[!last_stat];
	ps->tx_bytes_diff = eth_stats[port_id].tx_bytes[last_stat] - eth_stats[port_id].tx_bytes[!last_stat];
	ps->rx_pkts_diff = eth_stats[port_id].rx_tot[last_stat] - eth_stats[port_id].rx_tot[!last_stat];
	ps->tx_pkts_diff = eth_stats[port_id].tx_tot[last_stat] - eth_stats[port_id].tx_tot[!last_stat];

	ps->rx_tot = eth_stats[port_id].rx_tot[last_stat];
	ps->rx_tot = eth_stats[port_id].tx_tot[last_stat];
	ps->no_mbufs_tot = eth_stats[port_id].no_mbufs[last_stat];
	ps->ierrors_tot = eth_stats[port_id].ierrors[last_stat];

	ps->last_tsc = eth_stats[port_id].tsc[last_stat];
	ps->prev_tsc = eth_stats[port_id].tsc[!last_stat];

	return 0;
}

static void display_stats_eth_ports(void)
{
	uint8_t count = 0;
	for (uint8_t port_id = 0; port_id < nb_interface; ++port_id) {
		if (dppd_port_cfg[port_id].active) {
			uint64_t delta_t = eth_stats[port_id].tsc[last_stat] - eth_stats[port_id].tsc[!last_stat];
			uint64_t thresh = UINT64_MAX/rte_get_tsc_hz();

			uint64_t no_mbufs_diff = eth_stats[port_id].no_mbufs[last_stat] - eth_stats[port_id].no_mbufs[!last_stat];
			uint64_t ierrors_diff = eth_stats[port_id].ierrors[last_stat] - eth_stats[port_id].ierrors[!last_stat];


			uint64_t rx_bytes_diff = eth_stats[port_id].rx_bytes[last_stat] - eth_stats[port_id].rx_bytes[!last_stat];
			uint64_t tx_bytes_diff = eth_stats[port_id].tx_bytes[last_stat] - eth_stats[port_id].tx_bytes[!last_stat];

			uint64_t rx_diff = eth_stats[port_id].rx_tot[last_stat] - eth_stats[port_id].rx_tot[!last_stat];
			uint64_t tx_diff = eth_stats[port_id].tx_tot[last_stat] - eth_stats[port_id].tx_tot[!last_stat];

			if (no_mbufs_diff < thresh) {
				mvwaddstrf(win_stat, 5 + count, 14, "%12lu", no_mbufs_diff*rte_get_tsc_hz()/delta_t);
			}
			else {
				mvwaddstrf(win_stat, 5 + count, 14, "%12lu", no_mbufs_diff/(delta_t/rte_get_tsc_hz()));
			}

			if (ierrors_diff < thresh) {
				mvwaddstrf(win_stat, 5 + count, 27, "%12lu", ierrors_diff*rte_get_tsc_hz()/delta_t);
			}
			else {
				mvwaddstrf(win_stat, 5 + count, 27, "%12lu", ierrors_diff/(delta_t/rte_get_tsc_hz()));
			}

			if (rx_diff < thresh) {
				mvwaddstrf(win_stat, 5 + count, 40, "%9lu", (rx_diff*rte_get_tsc_hz()/delta_t)/1000);
			}
			else {
				mvwaddstrf(win_stat, 5 + count, 40, "%9lu", (rx_diff/(delta_t/rte_get_tsc_hz()))/1000);
			}

			if (tx_diff < thresh) {
				mvwaddstrf(win_stat, 5 + count, 50, "%9lu", (tx_diff*rte_get_tsc_hz()/delta_t)/1000);
			}
			else {
				mvwaddstrf(win_stat, 5 + count, 50, "%9lu", (tx_diff/(delta_t/rte_get_tsc_hz()))/1000);
			}

			if (rx_bytes_diff < thresh) {
				mvwaddstrf(win_stat, 5 + count, 60, "%9lu", (rx_bytes_diff*rte_get_tsc_hz()/delta_t)/125);
			}
			else {
				mvwaddstrf(win_stat, 5 + count, 60, "%9lu", (rx_bytes_diff/(delta_t/rte_get_tsc_hz()))/125);
			}

			if (tx_bytes_diff < thresh) {
				mvwaddstrf(win_stat, 5 + count, 70, "%9lu", (tx_bytes_diff*rte_get_tsc_hz()/delta_t)/125);
			}
			else {
				mvwaddstrf(win_stat, 5 + count, 70, "%9lu", (tx_bytes_diff/(delta_t/rte_get_tsc_hz()))/125);
			}

			mvwaddstrf(win_stat, 5 + count, 81, "%16lu", eth_stats[port_id].rx_tot[last_stat]);
			mvwaddstrf(win_stat, 5 + count, 98, "%16lu", eth_stats[port_id].tx_tot[last_stat]);

			mvwaddstrf(win_stat, 5 + count, 115, "%16lu", eth_stats[port_id].no_mbufs[last_stat]);
			mvwaddstrf(win_stat, 5 + count, 132, "%16lu", eth_stats[port_id].ierrors[last_stat]);
			count++;
		}
	}
}

static void display_stats_mempools(void)
{
	for (uint16_t i = 0; i < n_mempools; ++i) {
		size_t used = mempool_stats[i].size - mempool_stats[i].free;
		uint32_t used_frac = used*10000/mempool_stats[i].size;

		mvwaddstrf(win_stat, 5 + i, 14, "%3u.%02u", used_frac/100, used_frac % 100);
		mvwaddstrf(win_stat, 5 + i, 21, "%12zu", used);
		mvwaddstrf(win_stat, 5 + i, 34, "%12zu", mempool_stats[i].free);
		mvwaddstrf(win_stat, 5 + i, 60, "%14zu", used * MBUF_SIZE/1024);
		mvwaddstrf(win_stat, 5 + i, 75, "%14zu", mempool_stats[i].free * MBUF_SIZE/1024);
	}
}

uint64_t stats_core_task_lat_min(uint8_t lcore_id, uint8_t task_id)
{
	for (uint16_t i = 0; i < n_latency; ++i) {
		struct task_lat_stats* s = &task_lats[i];
		if (s->lcore_id == lcore_id && s->task_id == task_id) {
			struct lat_test *lat_test = &lat_stats[i];
			if ((lat_test->min_lat << 8) < UINT64_MAX/1000000) {
				return (lat_test->min_lat<<8)*1000000/rte_get_tsc_hz();
			}
			else {
				return (lat_test->min_lat<<8)/(rte_get_tsc_hz()/1000000);
			}
		}
	}

	return 0;
}

uint64_t stats_core_task_lat_max(uint8_t lcore_id, uint8_t task_id)
{
	for (uint16_t i = 0; i < n_latency; ++i) {
		struct task_lat_stats* s = &task_lats[i];
		if (s->lcore_id == lcore_id && s->task_id == task_id) {
			struct lat_test *lat_test = &lat_stats[i];
			if ((lat_test->max_lat << 8) < UINT64_MAX/1000000) {
				return (lat_test->max_lat<<8)*1000000/rte_get_tsc_hz();
			}
			else {
				return (lat_test->max_lat<<8)/(rte_get_tsc_hz()/1000000);
			}
		}
	}

	return 0;
}

uint64_t stats_core_task_lat_avg(uint8_t lcore_id, uint8_t task_id)
{
	for (uint16_t i = 0; i < n_latency; ++i) {
		struct task_lat_stats* s = &task_lats[i];

		if (s->lcore_id == lcore_id && s->task_id == task_id) {
			struct lat_test *lat_test = &lat_stats[i];

			if (!lat_test->tot_pkts) {
				return 0;
			}

			if ((lat_test->tot_lat << 8) < UINT64_MAX/1000000) {
				return (lat_test->tot_lat<<8)*1000000/(lat_test->tot_pkts*rte_get_tsc_hz());
			}
			else {
				return (lat_test->tot_lat<<8)/(lat_test->tot_pkts*rte_get_tsc_hz()/1000000);
			}
		}
	}
	return 0;
}

static void display_stats_latency(void)
{
	for (uint16_t i = 0; i < n_latency; ++i) {
		struct lat_test *lat_test = &lat_stats[i];
		if (lat_test->tot_pkts) {
			uint64_t avg_usec, avg_nsec, min_usec, min_nsec, max_usec, max_nsec;

			if ((lat_test->tot_lat << 8) < UINT64_MAX/1000000) {
				avg_usec = (lat_test->tot_lat<<8)*1000000/(lat_test->tot_pkts*rte_get_tsc_hz());
				avg_nsec = ((lat_test->tot_lat<<8)*1000000 - avg_usec*lat_test->tot_pkts*rte_get_tsc_hz())*1000/(lat_test->tot_pkts*rte_get_tsc_hz());
			}
			else {
				avg_usec = (lat_test->tot_lat<<8)/(lat_test->tot_pkts*rte_get_tsc_hz()/1000000);
				avg_nsec = 0;
			}

			if ((lat_test->min_lat << 8) < UINT64_MAX/1000000) {
				min_usec = (lat_test->min_lat<<8)*1000000/rte_get_tsc_hz();
				min_nsec = ((lat_test->min_lat<<8)*1000000 - min_usec*rte_get_tsc_hz())*1000/rte_get_tsc_hz();
			}
			else {
				min_usec = (lat_test->min_lat<<8)/(rte_get_tsc_hz()/1000000);
				min_nsec = 0;
			}


			if ((lat_test->max_lat << 8) < UINT64_MAX/1000000) {
				max_usec = (lat_test->max_lat<<8)*1000000/rte_get_tsc_hz();
				max_nsec = ((lat_test->max_lat<<8)*1000000 - max_usec*rte_get_tsc_hz())*1000/rte_get_tsc_hz();
			}
			else {
				max_usec = (lat_test->max_lat<<8)/(rte_get_tsc_hz()/1000000);
				max_nsec = 0;
			}

			mvwaddstrf(win_stat, 5 + i, 16, "%6"PRIu64".%03"PRIu64"", min_usec, min_nsec);
			mvwaddstrf(win_stat, 5 + i, 29, "%6"PRIu64".%03"PRIu64"", max_usec, max_nsec);
			mvwaddstrf(win_stat, 5 + i, 42, "%6"PRIu64".%03"PRIu64"", avg_usec, avg_nsec);
			//mvwaddstrf(win_stat, 5 + i, 53, "%12.3f", sqrt((lat_test->var_lat - lat_test->tot_lat*lat_test->tot_lat/lat_test->tot_pkts)/lat_test->tot_pkts)*1000000/rte_get_tsc_hz());
		}
	}
}

void display_refresh(void)
{
	stats_display_layout(1);
}

void display_stats(void)
{
	display_lock();
	switch (screen_state.chosen_screen) {
	case 0:
		display_stats_core_ports();
		break;
	case 1:
		display_stats_eth_ports();
		break;
	case 2:
		display_stats_mempools();
		break;
	case 3:
		display_stats_latency();
		break;
	}
	display_stats_general();
	wrefresh(win_stat);
	display_unlock();
}

#define KEY_ESC 27
static int disp_getch(void)
{
	int ret;
	display_lock();
	ret = wgetch(scr);
	display_unlock();

	/* Need to interpret escape sequence (ncurses keypad relies on
	   terminfo being set correctly which would require the user
	   to correctly configure his terminal). */
	if (ret == 27) {
		char esc_seq[10] = {0};
		int tmp, cnt = 0;
		do {
			fd_set in_fd;
			struct timeval tv;

			tv.tv_sec = 0;
			tv.tv_usec = 10000;

			FD_ZERO(&in_fd);
			FD_SET(fileno(stdin), &in_fd);
			tmp = select(fileno(stdin) + 1, &in_fd, NULL, NULL, &tv);
			if (FD_ISSET(fileno(stdin), &in_fd)) {
				display_lock();
				esc_seq[cnt++] = wgetch(scr);
				display_unlock();
				/* in case another ESC is encountered,
				   drop previous seq only keeping the latest. */
				if (esc_seq[cnt - 1] == 27) {
					cnt = 0;
				}
			}
		} while (tmp && cnt < 9);

		if (cnt == 0)
			return KEY_ESC;
		int i = 0;
		while (key_codes[i].seq != NULL) {
			if (strncmp(esc_seq, key_codes[i].seq, cnt) == 0) {
				return key_codes[i].key;
			}
			i++;
		}
		if (key_codes[i].seq == NULL) {
			plog_info("Unknown escape sequence ESC '%s'\n", esc_seq);
			return -1;
		}
	}

	return ret;
}

const char *get_key(void)
{
	static int lastid = 0;
	static int readid = -1;
	static char last_strs[MAX_STRID][MAX_CMDLEN] = {{0}};
	static unsigned len = 0;
	char key_buf[32] = {0};
	int car;
	int cnt = 0;
	int testid;
	const char *ret = NULL;

	car = disp_getch();
	if (car == -1) {
		return NULL;
	}

	switch(car) {
	case '\n':
	case KEY_ENTER:
		readid = -1;
		if (len == 0) {
			break;
		}
		len = 0;
		lastid = (lastid + 1) % MAX_STRID;
		strncpy(last_strs[lastid], str, MAX_CMDLEN);
		memset(str, 0, sizeof(str));
		refresh_cmd_win();
		ret = last_strs[lastid];
		break;
	case KEY_ESC:
		ret = "quit";
		break;
	case KEY_RESIZE:
		/* can ignore, resize already  during signal reception. */
		break;
	case 127:
	case KEY_BACKSPACE:
		if (len > 0) {
			str[--len] = '\0';
			refresh_cmd_win();
		}
		break;
	case KEY_UP:
		if (readid == -1) {
			testid = lastid;
		}
		else {
			testid = (readid + MAX_STRID - 1) % MAX_STRID;
		}
		if (last_strs[testid][0] != '\0') {
			readid = testid;
			strcpy(str, last_strs[readid]);
			len = strlen(str);
			refresh_cmd_win();
		}
		break;
	case KEY_DOWN:
		if (readid == -1) {
			testid = (lastid + 1) % MAX_STRID;
		}
		else {
			testid = (readid + 1) % MAX_STRID;
		}
		if (last_strs[testid][0] != '\0') {
			readid = testid;
			strcpy(str, last_strs[readid]);
			len = strlen(str);
			refresh_cmd_win();
		}
		break;
	case KEY_PPAGE:
		if (screen_state.chosen_page) {
			--screen_state.chosen_page;
			stats_display_layout(0);
		}
		break;
	case KEY_NPAGE:
		if (nb_tasks_tot > core_port_height * (screen_state.chosen_page + 1)) {
			++screen_state.chosen_page;
			stats_display_layout(0);
		}
		break;
	default:
		if (car >= KEY_F(1) && car <= KEY_F(4)) {
			int num = car - KEY_F(1) + 1;
			if (screen_state.chosen_screen != num - 1) {
				screen_state.chosen_screen = num - 1;
				stats_display_layout(0);
			}
			else
				stats_display_layout(1);
		}
		else if ((car >= '0' && car <= '4') && (len == 0)) {
			int num = car - '0';
			if (num == 0)
				stats_reset();
			else if (screen_state.chosen_screen != num - 1) {
				screen_state.chosen_screen = num - 1;
				stats_display_layout(0);
			}
			else
				stats_display_layout(1);
		}
		else if ((car >= '0' && car <= '9') ||
			 (car >= 'a' && car <= 'z') ||
			 (car >= 'A' && car <= 'Z') ||
			 car == ' ' || car == '-' ||
			 car == '_' || car == ',' ||
			 car == '.') {
			if (len < sizeof(str) - 1) {
				str[len++] = car;
				refresh_cmd_win();
			}
		}
		else {
			plog_warn("car %d, '%c' not supported\n", car, car);
		}
		break;
	}

	return ret;
}

static void reset_port_stats(void)
{
	for (uint8_t port_id = 0; port_id < nb_interface; ++port_id) {
		if (dppd_port_cfg[port_id].active) {
			rte_eth_stats_reset(port_id);
			memset(&eth_stats[port_id], 0, sizeof(struct eth_stats));
		}
	}
}

void stats_reset(void)
{
	uint64_t last_tsc = global_stats.last_tsc;

	memset(&global_stats, 0, sizeof(struct global_stats));
	global_stats.last_tsc = last_tsc;

	for (uint8_t task_id = 0; task_id < nb_tasks_tot; ++task_id) {
		struct port_stats *cur_port_stats = core_ports[task_id].port_stats;
		cur_port_stats->tot_rx_pkt_count = 0;
		cur_port_stats->tot_tx_pkt_count = 0;
		cur_port_stats->tot_tx_pkt_drop = 0;
	}

	reset_port_stats();

	start_tsc = rte_rdtsc();
	global_stats.avg_start = rte_rdtsc();
}

uint64_t global_last_tsc(void)
{
	return global_stats.last_tsc;
}

uint64_t global_total_tx(void)
{
	return global_stats.tx_tot;
}

uint64_t global_total_rx(void)
{
	return global_stats.rx_tot;
}

uint64_t global_avg_tx(void)
{
	return global_stats.tx_avg;
}

uint64_t global_avg_rx(void)
{
	return global_stats.rx_avg;
}

uint64_t global_pps_tx(void)
{
	return global_stats.tx_pps;
}

uint64_t global_pps_rx(void)
{
	return global_stats.rx_pps;
}

uint64_t tot_ierrors_per_sec(void)
{
	uint64_t ret = 0;
	uint64_t *ierrors;

	for (uint8_t port_id = 0; port_id < nb_interface; ++port_id) {
		if (dppd_port_cfg[port_id].active) {
			ierrors = eth_stats[port_id].ierrors;
			ret += ierrors[last_stat] - ierrors[!last_stat];
		}
	}

	return ret;
}

uint64_t tot_ierrors_tot(void)
{
	uint64_t ret = 0;

	for (uint8_t port_id = 0; port_id < nb_interface; ++port_id) {
		if (dppd_port_cfg[port_id].active) {
			ret += eth_stats[port_id].ierrors[last_stat];
		}
	}

	return ret;
}

void display_print(const char *str)
{
	display_lock();

	if (scr == NULL) {
		fputs(str, stdout);
		fflush(stdout);
		display_unlock();
	}

	waddstr(win_txt, str);
	wrefresh(win_txt);

	display_unlock();
}

#endif

#ifndef BRAS_STATS

void display_init(__attribute__((unused)) unsigned avg_start, __attribute__((unused)) unsigned duration){}
void display_end(void){}

const char *get_key(void) {return 0;}

void reset_stats(void){}
void update_stats(void){}
void display_stats(void) {}

uint64_t global_last_tsc(void) {return 0;}
uint64_t global_total_tx(void) {return 0;}
uint64_t global_total_rx(void) {return 0;}
uint64_t global_avg_tx(void) {return 0;}
uint64_t global_avg_rx(void) {return 0;}

uint64_t stats_core_task_tot_rx(__attribute__((unused)) uint8_t lcore_id, __attribute__((unused)) uint8_t task_id) {return 0;}
uint64_t stats_core_task_tot_tx(__attribute__((unused)) uint8_t lcore_id, __attribute__((unused)) uint8_t task_id) {return 0;}
uint64_t stats_core_task_tot_drop(__attribute__((unused)) uint8_t lcore_id, __attribute__((unused)) uint8_t task_id) {return 0;}
uint64_t stats_core_task_last_tsc(__attribute__((unused)) uint8_t lcore_id, __attribute__((unused)) uint8_t task_id) {return 0;}

#endif
