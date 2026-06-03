/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 * SPDX-License-Identifier: ISC
 */

/*
 * TX pipeline end-to-end latency histogram — debugfs interface
 *
 * Registers two debugfs files per vdev under:
 *   <debugfs_root>/dp/vdev<N>/tx_pipeline_hist/
 *
 *   config  (rw)  — read: current edges + enable state
 *                   write: "enable=1 edges=10,20,50,100,200,500,1000"
 *                          "enable=0"  (disable)
 *
 *   stats   (rw)  — read: full histogram dump with min/avg/p95/p99
 *                   write: "reset"  (zero all counters)
 */

#ifdef WLAN_FEATURE_DP_TX_PIPELINE_HIST

#include "dp_types.h"
#include "dp_internal.h"
#include "dp_tx.h"
#include <qdf_debugfs.h>
#include <qdf_mem.h>
#include <qdf_str.h>

/* debugfs root for all pipeline-hist vdev dirs */
static qdf_dentry_t dp_tx_pl_hist_root;

/*
 * dp_tx_pl_hist_compute_percentile() - find the bin containing the Nth
 *   percentile sample.  Returns the upper edge of that bin in µs, or
 *   UINT64_MAX if no samples have been collected.
 *
 * @stats: snapshot of histogram stats
 * @pct:   percentile (0–100)
 */
static uint64_t
dp_tx_pl_hist_compute_percentile(struct cdp_tx_pipeline_hist_stats *stats,
				 uint32_t pct)
{
	uint64_t threshold, cumulative = 0;
	uint8_t i;

	if (!stats->total_samples)
		return 0;

	threshold = (stats->total_samples * pct + 99) / 100;

	for (i = 0; i <= stats->num_edges; i++) {
		cumulative += stats->count[i];
		if (cumulative >= threshold) {
			if (i < stats->num_edges)
				return stats->edges_us[i];
			return UINT64_MAX; /* overflow bin */
		}
	}
	return UINT64_MAX;
}

/* ─── "config" file ──────────────────────────────────────────────────────── */

static QDF_STATUS
dp_tx_pl_hist_config_show(qdf_debugfs_file_t file, void *arg)
{
	struct dp_vdev *vdev = arg;
	struct cdp_tx_pipeline_hist_cfg *cfg = &vdev->tx_pl_hist_cfg;
	uint8_t i;

	qdf_debugfs_printf(file, "enable=%d num_edges=%u edges_us=",
			   cfg->enable ? 1 : 0, cfg->num_edges);
	for (i = 0; i < cfg->num_edges; i++) {
		if (i)
			qdf_debugfs_printf(file, ",");
		qdf_debugfs_printf(file, "%u", cfg->edges_us[i]);
	}
	qdf_debugfs_printf(file, "\n");
	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS
dp_tx_pl_hist_config_write(void *priv, const char *buf, qdf_size_t len)
{
	struct dp_vdev *vdev = priv;
	struct cdp_tx_pipeline_hist_cfg new_cfg = {0};
	const char *p;
	char tmp[256];
	char *tok, *edge_str;
	uint32_t val;
	uint8_t n = 0;

	if (len >= sizeof(tmp))
		return QDF_STATUS_E_INVAL;

	qdf_mem_copy(tmp, buf, len);
	tmp[len] = '\0';

	/* parse "enable=N" */
	p = strstr(tmp, "enable=");
	if (!p)
		return QDF_STATUS_E_INVAL;
	if (kstrtouint(p + 7, 10, &val))
		return QDF_STATUS_E_INVAL;
	new_cfg.enable = (val != 0);

	if (!new_cfg.enable) {
		dp_tx_pipeline_hist_config(vdev, &new_cfg);
		return QDF_STATUS_SUCCESS;
	}

	/* parse "edges=N1,N2,..." */
	p = strstr(tmp, "edges=");
	if (!p)
		return QDF_STATUS_E_INVAL;

	/* strtok on the edges substring */
	edge_str = (char *)(p + 6);
	tok = strsep(&edge_str, " \t\n");
	while (tok && *tok && n < DP_TX_PIPELINE_HIST_MAX_BINS) {
		char *comma = strsep(&tok, ",");
		while (comma) {
			if (*comma && !kstrtouint(comma, 10, &val)) {
				if (n > 0 && val <= new_cfg.edges_us[n - 1])
					return QDF_STATUS_E_INVAL;
				new_cfg.edges_us[n++] = val;
			}
			comma = strsep(&tok, ",");
		}
		tok = strsep(&edge_str, " \t\n");
	}

	if (!n)
		return QDF_STATUS_E_INVAL;

	new_cfg.num_edges = n;
	dp_tx_pipeline_hist_config(vdev, &new_cfg);
	return QDF_STATUS_SUCCESS;
}

static struct qdf_debugfs_fops dp_tx_pl_hist_config_fops = {
	.show  = dp_tx_pl_hist_config_show,
	.write = dp_tx_pl_hist_config_write,
};

/* ─── "stats" file ───────────────────────────────────────────────────────── */

static QDF_STATUS
dp_tx_pl_hist_stats_show(qdf_debugfs_file_t file, void *arg)
{
	struct dp_vdev *vdev = arg;
	struct cdp_tx_pipeline_hist_stats stats;
	uint64_t avg_us, p95, p99;
	uint8_t i;
	uint32_t prev_edge;

	dp_tx_pipeline_hist_fetch(vdev, &stats);

	avg_us = stats.total_samples ?
		 (stats.total_us / stats.total_samples) : 0;
	p95 = dp_tx_pl_hist_compute_percentile(&stats, 95);
	p99 = dp_tx_pl_hist_compute_percentile(&stats, 99);

	qdf_debugfs_printf(file,
		"TX pipeline latency (vdev %u)  samples=%llu  min=%lluus"
		"  avg=%lluus  p95=%s  p99=%s\n",
		vdev->vdev_id,
		stats.total_samples,
		stats.min_us,
		avg_us,
		(p95 == UINT64_MAX) ? ">=last_edge" :
			({ static char b95[20]; snprintf(b95,sizeof(b95),"%lluu",p95); b95; }),
		(p99 == UINT64_MAX) ? ">=last_edge" :
			({ static char b99[20]; snprintf(b99,sizeof(b99),"%lluu",p99); b99; }));

	prev_edge = 0;
	for (i = 0; i <= stats.num_edges; i++) {
		uint32_t up = (i < stats.num_edges) ? stats.edges_us[i] : 0;

		if (i < stats.num_edges)
			qdf_debugfs_printf(file, " [%6u - %6u) us : %llu\n",
					   prev_edge, up, stats.count[i]);
		else
			qdf_debugfs_printf(file, " [%6u -      ∞) us : %llu\n",
					   prev_edge, stats.count[i]);
		prev_edge = up;
	}
	return QDF_STATUS_SUCCESS;
}

static QDF_STATUS
dp_tx_pl_hist_stats_write(void *priv, const char *buf, qdf_size_t len)
{
	struct dp_vdev *vdev = priv;

	if (len >= 5 && !strncmp(buf, "reset", 5))
		dp_tx_pipeline_hist_reset(vdev);

	return QDF_STATUS_SUCCESS;
}

static struct qdf_debugfs_fops dp_tx_pl_hist_stats_fops = {
	.show  = dp_tx_pl_hist_stats_show,
	.write = dp_tx_pl_hist_stats_write,
};

/* ─── Per-vdev init / deinit ─────────────────────────────────────────────── */

QDF_STATUS dp_tx_pipeline_hist_dbgfs_init(struct dp_vdev *vdev)
{
	char dir_name[32];
	qdf_dentry_t vdev_dir;

	if (!dp_tx_pl_hist_root) {
		dp_tx_pl_hist_root = qdf_debugfs_create_dir("dp", NULL);
		if (!dp_tx_pl_hist_root) {
			dp_err("Failed to create dp debugfs root");
			return QDF_STATUS_E_FAILURE;
		}
	}

	snprintf(dir_name, sizeof(dir_name), "vdev%u_tx_pipeline_hist",
		 vdev->vdev_id);
	vdev_dir = qdf_debugfs_create_dir(dir_name, dp_tx_pl_hist_root);
	if (!vdev_dir) {
		dp_err("Failed to create tx_pipeline_hist dir for vdev %u",
		       vdev->vdev_id);
		return QDF_STATUS_E_FAILURE;
	}

	vdev->tx_pl_hist_dbgfs_dir = vdev_dir;

	dp_tx_pl_hist_config_fops.priv = vdev;
	if (!qdf_debugfs_create_file_simplified("config", 0644, vdev_dir,
						&dp_tx_pl_hist_config_fops)) {
		dp_err("Failed to create config debugfs for vdev %u",
		       vdev->vdev_id);
		return QDF_STATUS_E_FAILURE;
	}

	dp_tx_pl_hist_stats_fops.priv = vdev;
	if (!qdf_debugfs_create_file_simplified("stats", 0644, vdev_dir,
						&dp_tx_pl_hist_stats_fops)) {
		dp_err("Failed to create stats debugfs for vdev %u",
		       vdev->vdev_id);
		return QDF_STATUS_E_FAILURE;
	}

	return QDF_STATUS_SUCCESS;
}

void dp_tx_pipeline_hist_dbgfs_deinit(struct dp_vdev *vdev)
{
	if (vdev->tx_pl_hist_dbgfs_dir) {
		qdf_debugfs_remove_dir_recursive(vdev->tx_pl_hist_dbgfs_dir);
		vdev->tx_pl_hist_dbgfs_dir = NULL;
	}
}

#endif /* WLAN_FEATURE_DP_TX_PIPELINE_HIST */
