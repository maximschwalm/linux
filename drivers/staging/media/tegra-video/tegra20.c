// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2020 NVIDIA CORPORATION.  All rights reserved.
 */

/*
 * This source file contains Tegra20 and Tegra30 supported video formats,
 * VI and CSI SoC specific data, operations and registers accessors.
 */
#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/clk/tegra.h>
#include <linux/delay.h>
#include <linux/host1x.h>
#include <linux/kthread.h>

#include "csi.h"
#include "vi.h"
#include "tegra20.h"

/* Tegra20 VI operations */
static const struct tegra_vi_ops tegra20_vi_ops = {
	.vi_start_streaming = tegra20_vi_start_streaming,
	.vi_stop_streaming = tegra20_vi_stop_streaming,
};

/* Tegra20 VI SoC data */
const struct tegra_vi_soc tegra20_vi_soc = {
	.video_formats = tegra20_video_formats,
	.nformats = ARRAY_SIZE(tegra20_video_formats),
	.ops = &tegra20_vi_ops,
	.hw_revision = 3,
	.vi_max_channels = 6,
	.vi_max_clk_hz = 998400000,
};

/* Tegra20 CSI operations */
static const struct tegra_csi_ops tegra20_csi_ops = {
	.csi_start_streaming = tegra20_csi_start_streaming,
	.csi_stop_streaming = tegra20_csi_stop_streaming,
	.csi_err_recover = tegra20_csi_error_recover,
};

/* Tegra20 CSI SoC data */
const struct tegra_csi_soc tegra20_csi_soc = {
	.ops = &tegra20_csi_ops,
	.csi_max_channels = 6,
	.clk_names = tegra20_csi_cil_clks,
	.num_clks = ARRAY_SIZE(tegra20_csi_cil_clks),
	.tpg_frmrate_table = tegra20_tpg_frmrate_table,
	.tpg_frmrate_table_size = ARRAY_SIZE(tegra20_tpg_frmrate_table),
};
