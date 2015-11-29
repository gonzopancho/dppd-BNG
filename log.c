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

#include <sys/types.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

#include "log.h"
#include "display.h"

static pthread_mutex_t file_mtx = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
int log_lvl = DPPD_MAX_LOG_LVL;
static uint64_t tsc_off;
static FILE *fp;

void plog_init(const char *log_name, int log_name_pid)
{
	pid_t pid;
	char buf[128];

	if (*log_name == 0) {
		if (log_name_pid)
			snprintf(buf, sizeof(buf), "%s-%u.log", "dppd", getpid());
		else
			strncpy(buf, "dppd.log", sizeof(buf));
	}
	else {
		strncpy(buf, log_name, sizeof(buf));
	}

	fp = fopen(buf, "w");

	tsc_off = rte_rdtsc();
}

int plog_set_lvl(int lvl)
{
	if (lvl <= DPPD_MAX_LOG_LVL) {
		log_lvl = lvl;
		return 0;
	}

	return -1;
}

static void file_lock(void)
{
	pthread_mutex_lock(&file_mtx);
}

static void file_unlock(void)
{
	pthread_mutex_unlock(&file_mtx);
}

static void file_print(const char *str)
{
	file_lock();
	if (fp != NULL) {
		fputs(str, fp);
		fflush(fp);
	}
	file_unlock();
}
static void plog_buf(const char* buf)
{
	file_print(buf);
#ifdef BRAS_STATS
	display_print(buf);
#else
	/* ncurses never initialized */
	fputs(buf, stdout);
	fflush(stdout);
#endif
}

static const char* lvl_to_str(int lvl)
{
	switch (lvl) {
	case DPPD_LOG_ERR:  return "Error";
	case DPPD_LOG_WARN: return "Warning";
	case DPPD_LOG_INFO: return "";
	case DPPD_LOG_DBG:  return "Debug";
	default: return "?";
	}
}

#define DUMP_PKT_LEN 128
static void dump_pkt(char *dst, size_t dst_size, const struct rte_mbuf *mbuf)
{
	const struct ether_hdr *peth = rte_pktmbuf_mtod(mbuf, const struct ether_hdr *);
	const struct ipv4_hdr *dpip = (const struct ipv4_hdr *)(peth + 1);
	const uint8_t *pkt_bytes = (const uint8_t *)peth;
	const uint16_t len = rte_pktmbuf_pkt_len(mbuf);
	size_t str_len = 0;

	str_len = snprintf(dst, dst_size, "pkt_len: %u, Eth: %x, Proto: %#06x",
			   len, peth->ether_type, dpip->next_proto_id);

	for (uint16_t i = 0; i < len && i < DUMP_PKT_LEN && str_len < dst_size; ++i) {
		if (i % 16 == 0) {
			str_len += snprintf(dst + str_len, dst_size - str_len, "\n%04x  ", i);
		}
		else if (i % 8 == 0) {
			str_len += snprintf(dst + str_len, dst_size - str_len, " ");
		}
		str_len += snprintf(dst + str_len, dst_size - str_len, "%02x ", pkt_bytes[i]);
	}
	if (str_len < dst_size)
		snprintf(dst + str_len, dst_size - str_len, "\n");
}

static void vplog(int lvl, const char *format, va_list ap, const struct rte_mbuf *mbuf)
{
	char buf[1024];

	if (lvl > log_lvl)
		return;

	if (format == NULL && mbuf == NULL)
		return;

	if (format) {
		snprintf(buf, sizeof(buf), "%s%s", lvl_to_str(lvl), lvl == DPPD_LOG_INFO? "" : ": ");
		vsnprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1, format, ap);
	}

	if (mbuf) {
		dump_pkt(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1, mbuf);
	}
	plog_buf(buf);
}

static void vplogx(int lvl, const char *format, va_list ap, const struct rte_mbuf *mbuf)
{
	char buf[1024];
	uint64_t now_off;

	if (lvl > log_lvl)
		return;

	if (format == NULL && mbuf == NULL)
		return;

	if (format) {
		now_off = rte_rdtsc() - tsc_off;

		snprintf(buf, sizeof(buf), "C%u %02"PRIu64" ms%s%s: ",
			 rte_lcore_id(), now_off/(rte_get_tsc_hz() / 1000),
			 lvl != DPPD_LOG_INFO? " " : "", lvl_to_str(lvl));
		vsnprintf(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1, format, ap);
	}

	if (mbuf) {
		dump_pkt(buf + strlen(buf), sizeof(buf) - strlen(buf) - 1, mbuf);
	}
	plog_buf(buf);
}

#if DPPD_MAX_LOG_LVL >= DPPD_LOG_INFO
void plog_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplog(DPPD_LOG_INFO, fmt, ap, NULL);
	va_end(ap);
}

void plogx_info(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_INFO, fmt, ap, NULL);
	va_end(ap);
}

void plogd_info(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplog(DPPD_LOG_INFO, fmt, ap, mbuf);
	va_end(ap);
}

void plogdx_info(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_INFO, fmt, ap, mbuf);
	va_end(ap);
}
#endif

#if DPPD_MAX_LOG_LVL >= DPPD_LOG_ERR
void plog_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplog(DPPD_LOG_ERR, fmt, ap, NULL);
	va_end(ap);
}

void plogx_err(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_ERR, fmt, ap, NULL);
	va_end(ap);
}

void plogd_err(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_ERR, fmt, ap, mbuf);
	va_end(ap);
}

void plogdx_err(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_ERR, fmt, ap, mbuf);
	va_end(ap);
}
#endif

#if DPPD_MAX_LOG_LVL >= DPPD_LOG_WARN
void plog_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplog(DPPD_LOG_WARN, fmt, ap, NULL);
	va_end(ap);
}

void plogx_warn(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_WARN, fmt, ap, NULL);
	va_end(ap);
}

void plogd_warn(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplog(DPPD_LOG_WARN, fmt, ap, mbuf);
	va_end(ap);
}

void plogdx_warn(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_WARN, fmt, ap, mbuf);
	va_end(ap);
}
#endif

#if DPPD_MAX_LOG_LVL >= DPPD_LOG_DBG
void plog_dbg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplog(DPPD_LOG_DBG, fmt, ap, NULL);
	va_end(ap);
}

void plogx_dbg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_DBG, fmt, ap, NULL);
	va_end(ap);
}

void plogd_dbg(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplog(DPPD_LOG_DBG, fmt, ap, mbuf);
	va_end(ap);
}

void plogdx_dbg(const struct rte_mbuf *mbuf, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vplogx(DPPD_LOG_DBG, fmt, ap, mbuf);
	va_end(ap);
}
#endif
