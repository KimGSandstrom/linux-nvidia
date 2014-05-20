/*
 * tegra210_xbar_alt.h - TEGRA210 XBAR registers
 *
 * Copyright (c) 2014 NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __TEGRA210_XBAR_ALT_H__
#define __TEGRA210_XBAR_ALT_H__

#define TEGRA210_XBAR_PART0_RX					0x0
#define TEGRA210_XBAR_PART1_RX					0x200
#define TEGRA210_XBAR_PART2_RX					0x400
#define TEGRA210_XBAR_RX_STRIDE					0x4
#define TEGRA210_XBAR_AUDIO_RX_COUNT				90

/* This register repeats twice for each XBAR TX CIF */
/* The fields in this register are 1 bit per XBAR RX CIF */

/* Fields in *_CIF_RX/TX_CTRL; used by AHUB FIFOs, and all other audio modules */

#define TEGRA210_AUDIOCIF_CTRL_FIFO_THRESHOLD_SHIFT	24
#define TEGRA210_AUDIOCIF_CTRL_FIFO_THRESHOLD_MASK_US	0x3f
#define TEGRA210_AUDIOCIF_CTRL_FIFO_THRESHOLD_MASK	(TEGRA210_AUDIOCIF_CTRL_FIFO_THRESHOLD_MASK_US << TEGRA210_AUDIOCIF_CTRL_FIFO_THRESHOLD_SHIFT)

/* Channel count minus 1 */
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_CHANNELS_SHIFT	20
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_CHANNELS_MASK_US	0xf
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_CHANNELS_MASK	(TEGRA210_AUDIOCIF_CTRL_AUDIO_CHANNELS_MASK_US << TEGRA210_AUDIOCIF_CTRL_AUDIO_CHANNELS_SHIFT)

/* Channel count minus 1 */
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_CHANNELS_SHIFT	16
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_CHANNELS_MASK_US	0xf
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_CHANNELS_MASK	(TEGRA210_AUDIOCIF_CTRL_CLIENT_CHANNELS_MASK_US << TEGRA210_AUDIOCIF_CTRL_CLIENT_CHANNELS_SHIFT)

#define TEGRA210_AUDIOCIF_BITS_RVDS			0
#define TEGRA210_AUDIOCIF_BITS_8			1
#define TEGRA210_AUDIOCIF_BITS_12			2
#define TEGRA210_AUDIOCIF_BITS_16			3
#define TEGRA210_AUDIOCIF_BITS_20			4
#define TEGRA210_AUDIOCIF_BITS_24			5
#define TEGRA210_AUDIOCIF_BITS_28			6
#define TEGRA210_AUDIOCIF_BITS_32			7

#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT		12
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_MASK		(7                        << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_8		(TEGRA210_AUDIOCIF_BITS_8  << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_12		(TEGRA210_AUDIOCIF_BITS_12 << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_16		(TEGRA210_AUDIOCIF_BITS_16 << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_20		(TEGRA210_AUDIOCIF_BITS_20 << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_24		(TEGRA210_AUDIOCIF_BITS_24 << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_28		(TEGRA210_AUDIOCIF_BITS_28 << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_32		(TEGRA210_AUDIOCIF_BITS_32 << TEGRA210_AUDIOCIF_CTRL_AUDIO_BITS_SHIFT)

#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT	8
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_MASK		(7                        << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_8		(TEGRA210_AUDIOCIF_BITS_8  << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_12		(TEGRA210_AUDIOCIF_BITS_12 << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_16		(TEGRA210_AUDIOCIF_BITS_16 << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_20		(TEGRA210_AUDIOCIF_BITS_20 << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_24		(TEGRA210_AUDIOCIF_BITS_24 << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_28		(TEGRA210_AUDIOCIF_BITS_28 << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_32		(TEGRA210_AUDIOCIF_BITS_32 << TEGRA210_AUDIOCIF_CTRL_CLIENT_BITS_SHIFT)

#define TEGRA210_AUDIOCIF_EXPAND_ZERO			0
#define TEGRA210_AUDIOCIF_EXPAND_ONE			1
#define TEGRA210_AUDIOCIF_EXPAND_LFSR			2

#define TEGRA210_AUDIOCIF_CTRL_EXPAND_SHIFT		6
#define TEGRA210_AUDIOCIF_CTRL_EXPAND_MASK		(3                            << TEGRA210_AUDIOCIF_CTRL_EXPAND_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_EXPAND_ZERO		(TEGRA210_AUDIOCIF_EXPAND_ZERO << TEGRA210_AUDIOCIF_CTRL_EXPAND_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_EXPAND_ONE		(TEGRA210_AUDIOCIF_EXPAND_ONE  << TEGRA210_AUDIOCIF_CTRL_EXPAND_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_EXPAND_LFSR		(TEGRA210_AUDIOCIF_EXPAND_LFSR << TEGRA210_AUDIOCIF_CTRL_EXPAND_SHIFT)

#define TEGRA210_AUDIOCIF_STEREO_CONV_CH0		0
#define TEGRA210_AUDIOCIF_STEREO_CONV_CH1		1
#define TEGRA210_AUDIOCIF_STEREO_CONV_AVG		2

#define TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_SHIFT	4
#define TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_MASK		(3                                << TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_CH0		(TEGRA210_AUDIOCIF_STEREO_CONV_CH0 << TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_CH1		(TEGRA210_AUDIOCIF_STEREO_CONV_CH1 << TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_AVG		(TEGRA210_AUDIOCIF_STEREO_CONV_AVG << TEGRA210_AUDIOCIF_CTRL_STEREO_CONV_SHIFT)

#define TEGRA210_AUDIOCIF_CTRL_REPLICATE_SHIFT		3

#define TEGRA210_AUDIOCIF_TRUNCATE_ROUND		0
#define TEGRA210_AUDIOCIF_TRUNCATE_CHOP			1

#define TEGRA210_AUDIOCIF_CTRL_TRUNCATE_SHIFT		1
#define TEGRA210_AUDIOCIF_CTRL_TRUNCATE_MASK		(1                               << TEGRA210_AUDIOCIF_CTRL_TRUNCATE_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_TRUNCATE_ROUND		(TEGRA210_AUDIOCIF_TRUNCATE_ROUND << TEGRA210_AUDIOCIF_CTRL_TRUNCATE_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_TRUNCATE_CHOP		(TEGRA210_AUDIOCIF_TRUNCATE_CHOP  << TEGRA210_AUDIOCIF_CTRL_TRUNCATE_SHIFT)

#define TEGRA210_AUDIOCIF_MONO_CONV_ZERO		0
#define TEGRA210_AUDIOCIF_MONO_CONV_COPY		1

#define TEGRA210_AUDIOCIF_CTRL_MONO_CONV_SHIFT		0
#define TEGRA210_AUDIOCIF_CTRL_MONO_CONV_MASK		(1                               << TEGRA210_AUDIOCIF_CTRL_MONO_CONV_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_MONO_CONV_ZERO		(TEGRA210_AUDIOCIF_MONO_CONV_ZERO << TEGRA210_AUDIOCIF_CTRL_MONO_CONV_SHIFT)
#define TEGRA210_AUDIOCIF_CTRL_MONO_CONV_COPY		(TEGRA210_AUDIOCIF_MONO_CONV_COPY << TEGRA210_AUDIOCIF_CTRL_MONO_CONV_SHIFT)

/* maximum mux count in T210 */
#define TEGRA210_XBAR_UPDATE_MAX_REG	3

struct tegra210_xbar_cif_conf {
	unsigned int threshold;
	unsigned int audio_channels;
	unsigned int client_channels;
	unsigned int audio_bits;
	unsigned int client_bits;
	unsigned int expand;
	unsigned int stereo_conv;
	unsigned int replicate;
	unsigned int truncate;
	unsigned int mono_conv;
};

void tegra210_xbar_set_cif(struct regmap *regmap, unsigned int reg,
			  struct tegra210_xbar_cif_conf *conf);

int tegra210_xbar_read_reg (unsigned int reg, unsigned int *val);

struct tegra210_xbar_soc_data {
	const struct regmap_config *regmap_config;
	unsigned int mask[3];
	unsigned int reg_count;
	unsigned int reg_offset;
};

struct tegra210_xbar {
	struct clk *clk;
	struct clk *clk_parent;
	struct regmap *regmap;
	const struct tegra210_xbar_soc_data *soc_data;
};

#endif
