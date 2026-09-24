// SPDX-License-Identifier: GPL-2.0
/*
 * A V4L2 driver for OmniVision OS08E10 cameras
 *
 * Copyright (C) 2026, UAB Kurokesu
 * Copyright (C) 2021, Raspberry Pi (Trading) Ltd
 *
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* Paged 8-bit registers, one u32 packs page, address, width and byte order */
#define OS08E10_REG_ADDR_MASK GENMASK(7, 0)
#define OS08E10_REG_PAGE_MASK GENMASK(11, 8)
#define OS08E10_REG_WIDTH_MASK GENMASK(19, 16)
#define OS08E10_REG_LE BIT(20)

#define OS08E10_REG(p, a, w) (((w) << 16) | ((p) << 8) | (a))
#define OS08E10_REG8(p, a) OS08E10_REG(p, a, 1)
#define OS08E10_REG16(p, a) OS08E10_REG(p, a, 2)
#define OS08E10_REG24(p, a) OS08E10_REG(p, a, 3)
#define OS08E10_REG32(p, a) OS08E10_REG(p, a, 4)
#define OS08E10_REG16_LE(p, a) (OS08E10_REG16(p, a) | OS08E10_REG_LE)

#define OS08E10_REG_ADDR(reg) FIELD_GET(OS08E10_REG_ADDR_MASK, reg)
#define OS08E10_REG_PAGE(reg) FIELD_GET(OS08E10_REG_PAGE_MASK, reg)
#define OS08E10_REG_WIDTH(reg) FIELD_GET(OS08E10_REG_WIDTH_MASK, reg)

/* Page select */
#define OS08E10_WR_RD_CTRL 0xFD
#define OS08E10_PAGE_INVALID 0xFF

/* Registers */
#define OS08E10_REG_CHIP_ID OS08E10_REG32(0x00, 0x00)
#define OS08E10_REG_DPLL_CNT_CLK_VB_DIS OS08E10_REG8(0x00, 0x13)
#define OS08E10_REG_DPLL_PCLK_PRE_SEL OS08E10_REG8(0x00, 0x14)
#define OS08E10_REG_DPLL_CP_CLK_PRE_SEL OS08E10_REG8(0x00, 0x15)
#define OS08E10_REG_DPLL_BYP_SEL OS08E10_REG8(0x00, 0x18)
#define OS08E10_REG_DPLL_NC_SEL OS08E10_REG8(0x00, 0x1B)
#define OS08E10_REG_TIMER_CLK_CTRL OS08E10_REG8(0x00, 0x1D)
#define OS08E10_REG_MIPI_CLK_SEL OS08E10_REG8(0x00, 0x1E)
#define OS08E10_REG_DCLKIN_CISISP_GATING_EN OS08E10_REG8(0x00, 0x1F)
#define OS08E10_REG_SOFT_AUTO_RELEASE_EN OS08E10_REG8(0x00, 0x20)
#define OS08E10_REG_BCLK_GATING_SW_OFF OS08E10_REG8(0x00, 0x21)
#define OS08E10_REG_DCLK_MF_PD_GATING_EN OS08E10_REG8(0x00, 0x26)
#define OS08E10_REG_DAC_PLL_GATING OS08E10_REG8(0x00, 0x27)
#define OS08E10_REG_MIPI_RST_FIX_EN OS08E10_REG8(0x00, 0x28)
#define OS08E10_REG_TRIGGER OS08E10_REG8(0x01, 0x01)
#define OS08E10_REG_EXP1 OS08E10_REG24(0x01, 0x02)
#define OS08E10_REG_VBLANK_BUF OS08E10_REG16(0x01, 0x05)
#define OS08E10_REG_DIG_GAIN1 OS08E10_REG16(0x01, 0x21)
#define OS08E10_REG_ANA_GAIN1 OS08E10_REG16_LE(0x01, 0x24)
#define OS08E10_REG_MIRROR_FLIP OS08E10_REG8(0x01, 0x32)
#define OS08E10_REG_DATA_ID1 OS08E10_REG8(0x02, 0x75)
#define OS08E10_REG_DATA_ID2 OS08E10_REG8(0x02, 0x76)
#define OS08E10_REG_DATA_ID3 OS08E10_REG8(0x02, 0x77)
#define OS08E10_REG_R_INIT OS08E10_REG16_LE(0x02, 0x88)
#define OS08E10_REG_R_LPX_DAT OS08E10_REG8(0x02, 0x8B)
#define OS08E10_REG_R_HS_PREPARE OS08E10_REG8(0x02, 0x8C)
#define OS08E10_REG_R_HS_ZERO OS08E10_REG8(0x02, 0x8D)
#define OS08E10_REG_R_HS_TRAIL OS08E10_REG8(0x02, 0x8E)
#define OS08E10_REG_R_EXIT OS08E10_REG8(0x02, 0x8F)
#define OS08E10_REG_R_LPX_CK OS08E10_REG8(0x02, 0x90)
#define OS08E10_REG_R_CLK_PREPARE OS08E10_REG8(0x02, 0x91)
#define OS08E10_REG_R_CLK_ZERO OS08E10_REG8(0x02, 0x92)
#define OS08E10_REG_R_CLK_POST OS08E10_REG8(0x02, 0x93)
#define OS08E10_REG_R_CLK_TRAIL OS08E10_REG8(0x02, 0x94)
#define OS08E10_REG_ISP_MODE OS08E10_REG8(0x02, 0xAA)
#define OS08E10_REG_RAW_12_EN OS08E10_REG8(0x04, 0x00)
#define OS08E10_REG_PATTERN_SWITCH_EN OS08E10_REG8(0x04, 0x12)
#define OS08E10_REG_BLC_45 OS08E10_REG8(0x07, 0x45)
#define OS08E10_REG_BLC_47 OS08E10_REG8(0x07, 0x47)
#define OS08E10_REG_PSNC_RST_EN OS08E10_REG8(0x0A, 0x39)
#define OS08E10_REG_DAC_LOAD_HCG_6X OS08E10_REG8(0x0A, 0x45)
#define OS08E10_REG_P56 OS08E10_REG8(0x0A, 0x56)
#define OS08E10_REG_P57 OS08E10_REG8(0x0A, 0x57)
#define OS08E10_REG_P5E OS08E10_REG8(0x0A, 0x5E)
#define OS08E10_REG_P7B OS08E10_REG8(0x0A, 0x7B)
#define OS08E10_REG_P86_1X OS08E10_REG8(0x0A, 0x86)
#define OS08E10_REG_P88_1X OS08E10_REG8(0x0A, 0x88)
#define OS08E10_REG_P89 OS08E10_REG8(0x0A, 0x89)
#define OS08E10_REG_P8A OS08E10_REG8(0x0A, 0x8A)
#define OS08E10_REG_P91 OS08E10_REG8(0x0A, 0x91)
#define OS08E10_REG_P92 OS08E10_REG8(0x0A, 0x92)
#define OS08E10_REG_P95 OS08E10_REG8(0x0A, 0x95)
#define OS08E10_REG_PAA OS08E10_REG8(0x0A, 0xAA)
#define OS08E10_REG_PC3 OS08E10_REG8(0x0A, 0xC3)
#define OS08E10_REG_P86_2X OS08E10_REG8(0x0A, 0xD0)
#define OS08E10_REG_P86_3X OS08E10_REG8(0x0A, 0xD1)
#define OS08E10_REG_P88_2X OS08E10_REG8(0x0A, 0xD5)
#define OS08E10_REG_P88_3X OS08E10_REG8(0x0A, 0xD6)
#define OS08E10_REG_MPLL_PREDIVP_SEL OS08E10_REG8(0x0B, 0x10)
#define OS08E10_REG_MPLL_PHY_CLK_SEL OS08E10_REG8(0x0B, 0x12)
#define OS08E10_REG_MPLL_CP_SEL OS08E10_REG8(0x0B, 0x13)
#define OS08E10_REG_MPLL_NC_SEL OS08E10_REG8(0x0B, 0x14)
#define OS08E10_REG_DAC_BITS_SEL OS08E10_REG8(0x0B, 0x80)

#define OS08E10_CHIP_ID 0x10450853

#define OS08E10_FREQ_EXTCLK 24000000
#define OS08E10_FREQ_ROWCLK_50MHZ 50000000
#define OS08E10_FREQ_ROWCLK_25MHZ 25000000
#define OS08E10_FREQ_LINK_1452MBPS 726000000
#define OS08E10_FREQ_LINK_738MBPS 369000000

/*
 * Frame timing. VTS is dummy lines on top of the fixed array readout, row
 * clocks scale to pixel clocks by 12 so that HBLANK stays positive
 */
#define OS08E10_ARRAY_ROWS 2224
#define OS08E10_FRAME_OVERHEAD 2
#define OS08E10_VTS_BASE (OS08E10_ARRAY_ROWS + OS08E10_FRAME_OVERHEAD)
#define OS08E10_VTS_MAX (0xFFFF + OS08E10_VTS_BASE)
#define OS08E10_PIXCLK_PER_ROWCLK 12
#define OS08E10_ADD_DELAY_NUM 8
#define OS08E10_EXPOSURE_OFFSET (OS08E10_ADD_DELAY_NUM * 4 + 1)

/* OS08E10_REG_SOFT_AUTO_RELEASE_EN values, active low resets */
#define OS08E10_RESET_ALL 0x00
#define OS08E10_RESET_LOGIC 0x17
#define OS08E10_RESET_NONE 0x1F

#define OS08E10_TRIGGER 0x01
#define OS08E10_TRIGGER_INIT 0x31
#define OS08E10_MIPI_RST_FIX 0x14

#define OS08E10_CLK_GATING_DEFAULT 0xB7
#define OS08E10_DCLKIN_TP_GATING_EN BIT(6)

#define OS08E10_ISP_MODE_DEFAULT 0x03
#define OS08E10_ISP_MODE_UPDOWN_AUTO_BAYER_EN BIT(4)
#define OS08E10_ISP_MODE_MIRROR_AUTO_BAYER_EN BIT(5)
#define OS08E10_ISP_MODE_AUTO_BAYER                                         \
	(OS08E10_ISP_MODE_DEFAULT | OS08E10_ISP_MODE_UPDOWN_AUTO_BAYER_EN | \
	 OS08E10_ISP_MODE_MIRROR_AUTO_BAYER_EN)

#define OS08E10_EXPOSURE_MIN 2
#define OS08E10_EXPOSURE_STEP 1

#define OS08E10_ANA_GAIN_MIN 0x010
#define OS08E10_ANA_GAIN_MAX 0x1FF
#define OS08E10_ANA_GAIN_STEP 1
#define OS08E10_ANA_GAIN_DEFAULT 0x010

#define OS08E10_DGTL_GAIN_MIN 0x0040
#define OS08E10_DGTL_GAIN_MAX 0x07FF
#define OS08E10_DGTL_GAIN_DEFAULT 0x0040
#define OS08E10_DGTL_GAIN_STEP 1

#define OS08E10_TEST_PATTERN_DISABLED 0x00
#define OS08E10_TEST_PATTERN_COLOR_BARS 0x01
#define OS08E10_TEST_PATTERN_GRADIENT 0x09

#define OS08E10_NATIVE_WIDTH 3856U
#define OS08E10_NATIVE_HEIGHT 2176U
#define OS08E10_PIXEL_ARRAY_LEFT 8U
#define OS08E10_PIXEL_ARRAY_TOP 8U
#define OS08E10_PIXEL_ARRAY_WIDTH 3840U
#define OS08E10_PIXEL_ARRAY_HEIGHT 2160U

/* Power sequencing */
#define OS08E10_XSHUTDOWN_DELAY_MIN_US 5000
#define OS08E10_SCCB_DELAY_MIN_US 8000
#define OS08E10_SOFT_RESET_DELAY_MIN_US 2000
#define OS08E10_DELAY_RANGE_US 1000

#define OS08E10_BURST_MAX 16

#define OS08E10_NUM_SUPPLIES ARRAY_SIZE(os08e10_supply_names)

enum pad_types {
	IMAGE_PAD,
	NUM_PADS,
};

struct os08e10_reg {
	u32 reg;
	u32 val;
};

struct os08e10_reg_sequence {
	unsigned int num_regs;
	const struct os08e10_reg *regs;
};

struct os08e10_mode {
	unsigned int width;
	unsigned int height;
	struct v4l2_rect crop;

	unsigned int hts;
	unsigned int vts;

	struct os08e10_reg_sequence reg_sequence;
};

static const struct os08e10_reg os08e10_preinit[] = {
	{ OS08E10_REG8(0x0F, 0x2E), 0x02 }, { OS08E10_REG8(0x01, 0x27), 0x00 },
	{ OS08E10_REG8(0x03, 0x84), 0x00 }, { OS08E10_REG8(0x03, 0xA0), 0x01 },
	{ OS08E10_REG8(0x03, 0x9E), 0x00 }, { OS08E10_REG8(0x03, 0x9F), 0x60 },
	{ OS08E10_REG8(0x03, 0x9D), 0x01 }, { OS08E10_REG8(0x03, 0xC0), 0x00 },
	{ OS08E10_REG8(0x03, 0xC1), 0x20 },
};

/*
 * PLL config for:
 * External clock - 24MHz
 * Link frequency - 726MHz
 */
static const struct os08e10_reg os08e10_pll_config_24_726[] = {
	{ OS08E10_REG_BCLK_GATING_SW_OFF, 0x16 },
	{ OS08E10_REG_DPLL_PCLK_PRE_SEL, 0x11 },
	{ OS08E10_REG_DPLL_CNT_CLK_VB_DIS, 0x38 },
	{ OS08E10_REG_DPLL_CP_CLK_PRE_SEL, 0x12 },
	{ OS08E10_REG_DPLL_BYP_SEL, 0x00 },
	{ OS08E10_REG_DPLL_NC_SEL, 0x96 },
	{ OS08E10_REG_TIMER_CLK_CTRL, 0x10 },
	{ OS08E10_REG_MIPI_CLK_SEL, 0x0C },
	{ OS08E10_REG_DCLKIN_CISISP_GATING_EN, OS08E10_CLK_GATING_DEFAULT },
	{ OS08E10_REG_DCLK_MF_PD_GATING_EN, 0xF7 },
	{ OS08E10_REG_DAC_PLL_GATING, 0x32 },
	{ OS08E10_REG_BCLK_GATING_SW_OFF, 0x12 },
	{ OS08E10_REG_MPLL_PREDIVP_SEL, 0x00 },
	{ OS08E10_REG_MPLL_PHY_CLK_SEL, 0x00 },
	{ OS08E10_REG_MPLL_CP_SEL, 0x0B },
	{ OS08E10_REG_MPLL_NC_SEL, 0xF2 },
};

/*
 * PLL config for:
 * External clock - 24MHz
 * Link frequency - 369MHz
 */
static const struct os08e10_reg os08e10_pll_config_24_369[] = {
	{ OS08E10_REG_BCLK_GATING_SW_OFF, 0x16 },
	{ OS08E10_REG_DPLL_PCLK_PRE_SEL, 0x11 },
	{ OS08E10_REG_DPLL_CNT_CLK_VB_DIS, 0x38 },
	{ OS08E10_REG_DPLL_CP_CLK_PRE_SEL, 0x11 },
	{ OS08E10_REG_DPLL_BYP_SEL, 0x01 },
	{ OS08E10_REG_DPLL_NC_SEL, 0x96 },
	{ OS08E10_REG_TIMER_CLK_CTRL, 0x10 },
	{ OS08E10_REG_MIPI_CLK_SEL, 0x0C },
	{ OS08E10_REG_DCLKIN_CISISP_GATING_EN, OS08E10_CLK_GATING_DEFAULT },
	{ OS08E10_REG_DCLK_MF_PD_GATING_EN, 0xF7 },
	{ OS08E10_REG_DAC_PLL_GATING, 0x32 },
	{ OS08E10_REG_BCLK_GATING_SW_OFF, 0x12 },
	{ OS08E10_REG_MPLL_PREDIVP_SEL, 0x00 },
	{ OS08E10_REG_MPLL_PHY_CLK_SEL, 0x04 },
	{ OS08E10_REG_MPLL_CP_SEL, 0x0B },
	{ OS08E10_REG_MPLL_NC_SEL, 0xF6 },
};

static const struct os08e10_reg os08e10_common_init[] = {
	{ OS08E10_REG8(0x02, 0x51), 0x10 }, { OS08E10_REG8(0x00, 0x21), 0x10 },
	{ OS08E10_REG8(0x0F, 0x00), 0x50 }, { OS08E10_REG8(0x0F, 0x02), 0x10 },
	{ OS08E10_REG8(0x0F, 0x03), 0x03 }, { OS08E10_REG8(0x0F, 0x13), 0x44 },
	{ OS08E10_REG8(0x0F, 0x16), 0x44 }, { OS08E10_REG8(0x0F, 0x24), 0xF7 },
	{ OS08E10_REG8(0x0F, 0x2C), 0xE7 }, { OS08E10_REG8(0x0F, 0x2A), 0x02 },
	{ OS08E10_REG8(0x0F, 0x2E), 0x02 }, { OS08E10_REG8(0x0F, 0x2F), 0x8C },
	{ OS08E10_REG8(0x0F, 0x37), 0x6C }, { OS08E10_REG8(0x01, 0x07), 0x08 },
	{ OS08E10_REG8(0x01, 0x27), 0x00 }, { OS08E10_REG8(0x01, 0x30), 0x03 },
	{ OS08E10_REG8(0x01, 0x31), 0x00 }, { OS08E10_REG8(0x01, 0x3B), 0x14 },
	{ OS08E10_REG8(0x01, 0x3C), 0x00 }, { OS08E10_REG8(0x01, 0x4D), 0x00 },
	{ OS08E10_REG8(0x01, 0x4E), 0x00 }, { OS08E10_REG8(0x01, 0x4F), 0x46 },
	{ OS08E10_REG8(0x01, 0xE4), 0x0F }, { OS08E10_REG8(0x02, 0xA9), 0x77 },
	{ OS08E10_REG8(0x02, 0xF7), 0x01 }, { OS08E10_REG8(0x02, 0xC3), 0x24 },
	{ OS08E10_REG8(0x02, 0xC4), 0x12 }, { OS08E10_REG8(0x02, 0xC5), 0x01 },
	{ OS08E10_REG8(0x02, 0xCA), 0x03 }, { OS08E10_REG8(0x02, 0xCB), 0x0C },
	{ OS08E10_REG8(0x02, 0xCC), 0x0F }, { OS08E10_REG8(0x02, 0xCD), 0x0F },
	{ OS08E10_REG8(0x02, 0xCE), 0xFF }, { OS08E10_REG8(0x02, 0xCF), 0xFF },
	{ OS08E10_REG8(0x02, 0xD0), 0xFF }, { OS08E10_REG8(0x02, 0x36), 0xC1 },
	{ OS08E10_REG8(0x02, 0x02), 0x04 }, { OS08E10_REG8(0x02, 0x04), 0x0F },
	{ OS08E10_REG8(0x02, 0x05), 0x00 }, { OS08E10_REG8(0x02, 0x06), 0x08 },
	{ OS08E10_REG8(0x02, 0x07), 0x70 }, { OS08E10_REG8(0x02, 0x0A), 0x06 },
	{ OS08E10_REG8(0x02, 0x0B), 0x06 }, { OS08E10_REG8(0x02, 0x0C), 0x0E },
	{ OS08E10_REG8(0x02, 0x0D), 0x0E }, { OS08E10_REG8(0x02, 0x0E), 0x02 },
	{ OS08E10_REG8(0x02, 0x0F), 0x06 }, { OS08E10_REG8(0x02, 0x10), 0x10 },
	{ OS08E10_REG8(0x02, 0x11), 0x14 }, { OS08E10_REG8(0x02, 0x17), 0x00 },
	{ OS08E10_REG8(0x02, 0x19), 0x00 }, { OS08E10_REG8(0x02, 0x1A), 0x0F },
	{ OS08E10_REG8(0x02, 0x1B), 0x00 }, { OS08E10_REG8(0x02, 0x1C), 0x08 },
	{ OS08E10_REG8(0x02, 0x1D), 0x70 }, { OS08E10_REG8(0x02, 0x3B), 0x00 },
	{ OS08E10_REG8(0x02, 0x3D), 0x00 }, { OS08E10_REG8(0x02, 0x3E), 0x0F },
	{ OS08E10_REG8(0x02, 0x3F), 0x00 }, { OS08E10_REG8(0x02, 0x40), 0x08 },
	{ OS08E10_REG8(0x02, 0x41), 0x70 }, { OS08E10_REG8(0x02, 0xE4), 0x08 },
	{ OS08E10_REG8(0x02, 0xE5), 0x0F }, { OS08E10_REG8(0x02, 0xE6), 0x78 },
	{ OS08E10_REG8(0x02, 0xE7), 0x08 }, { OS08E10_REG8(0x05, 0x2F), 0x08 },
	{ OS08E10_REG8(0x05, 0x2E), 0x08 }, { OS08E10_REG8(0x0A, 0x0C), 0xAA },
	{ OS08E10_REG8(0x0A, 0x0D), 0x54 }, { OS08E10_REG8(0x0A, 0x0F), 0x20 },
	{ OS08E10_REG8(0x0A, 0x10), 0x33 }, { OS08E10_REG8(0x0A, 0x11), 0x44 },
	{ OS08E10_REG8(0x0A, 0x12), 0x77 }, { OS08E10_REG8(0x0A, 0x13), 0x43 },
	{ OS08E10_REG8(0x0A, 0x14), 0x42 }, { OS08E10_REG8(0x0A, 0x15), 0x42 },
	{ OS08E10_REG8(0x0A, 0x16), 0xC2 }, { OS08E10_REG8(0x0A, 0x17), 0xC2 },
	{ OS08E10_REG8(0x0A, 0x18), 0xC2 }, { OS08E10_REG8(0x0A, 0x2E), 0x30 },
	{ OS08E10_REG8(0x0A, 0x30), 0x08 }, { OS08E10_REG8(0x0A, 0x31), 0x18 },
	{ OS08E10_REG8(0x0A, 0x33), 0x50 }, { OS08E10_REG8(0x0A, 0x34), 0x31 },
	{ OS08E10_REG8(0x0A, 0x3A), 0xC0 }, { OS08E10_REG8(0x0A, 0x35), 0xC3 },
	{ OS08E10_REG8(0x0A, 0x36), 0xF6 }, { OS08E10_REG8(0x0A, 0x38), 0x0D },
	{ OS08E10_REG8(0x0A, 0x19), 0x46 }, { OS08E10_REG8(0x0A, 0x1A), 0x46 },
	{ OS08E10_REG8(0x0A, 0x1B), 0x4A }, { OS08E10_REG8(0x0A, 0x1C), 0x4C },
	{ OS08E10_REG8(0x0A, 0x1D), 0x96 }, { OS08E10_REG8(0x0A, 0x1E), 0x92 },
	{ OS08E10_REG8(0x0A, 0x40), 0x46 }, { OS08E10_REG8(0x0A, 0x41), 0x46 },
	{ OS08E10_REG8(0x0A, 0x42), 0x4A }, { OS08E10_REG8(0x0A, 0x43), 0x4C },
	{ OS08E10_REG8(0x0A, 0x44), 0x96 }, { OS08E10_REG8(0x0A, 0x50), 0x2F },
	{ OS08E10_REG8(0x0A, 0x53), 0x2F }, { OS08E10_REG8(0x0A, 0x51), 0x18 },
	{ OS08E10_REG8(0x0A, 0x52), 0x16 }, { OS08E10_REG8(0x0A, 0x59), 0x10 },
	{ OS08E10_REG8(0x0A, 0x5A), 0x10 }, { OS08E10_REG8(0x0A, 0xB8), 0x00 },
	{ OS08E10_REG8(0x0A, 0xB9), 0x00 }, { OS08E10_REG8(0x0A, 0x5C), 0x2D },
	{ OS08E10_REG8(0x0A, 0x5D), 0x3A }, { OS08E10_REG8(0x0A, 0x5F), 0x10 },
	{ OS08E10_REG8(0x0A, 0x67), 0x01 }, { OS08E10_REG8(0x0A, 0x7D), 0x1E },
	{ OS08E10_REG8(0x0A, 0x8E), 0x2F }, { OS08E10_REG8(0x0A, 0x8F), 0x02 },
	{ OS08E10_REG8(0x0A, 0x90), 0x60 }, { OS08E10_REG8(0x0A, 0x93), 0x42 },
	{ OS08E10_REG8(0x0A, 0x99), 0x2B }, { OS08E10_REG8(0x0A, 0x9A), 0x1B },
	{ OS08E10_REG8(0x0A, 0xA4), 0x00 }, { OS08E10_REG8(0x0A, 0xA8), 0x06 },
	{ OS08E10_REG8(0x0A, 0xB0), 0x2E }, { OS08E10_REG8(0x0A, 0xB7), 0x14 },
	{ OS08E10_REG8(0x0A, 0xD2), 0x51 }, { OS08E10_REG8(0x0A, 0xD3), 0x53 },
	{ OS08E10_REG8(0x0A, 0xD4), 0x53 }, { OS08E10_REG8(0x0A, 0xD7), 0x4F },
	{ OS08E10_REG8(0x0A, 0xD8), 0x50 }, { OS08E10_REG8(0x0A, 0xD9), 0x50 },
	{ OS08E10_REG8(0x0A, 0x8C), 0x50 }, { OS08E10_REG8(0x0A, 0xDF), 0x50 },
	{ OS08E10_REG8(0x0A, 0xE0), 0x50 }, { OS08E10_REG8(0x0A, 0xE1), 0x54 },
	{ OS08E10_REG8(0x0A, 0xE2), 0x54 }, { OS08E10_REG8(0x0A, 0xE3), 0x54 },
	{ OS08E10_REG8(0x0A, 0xEA), 0x10 }, { OS08E10_REG8(0x0A, 0x1F), 0x0B },
	{ OS08E10_REG8(0x0A, 0x20), 0x18 }, { OS08E10_REG8(0x0A, 0x21), 0x30 },
	{ OS08E10_REG8(0x0A, 0x22), 0x60 }, { OS08E10_REG8(0x0A, 0x23), 0xC0 },
	{ OS08E10_REG8(0x0A, 0x25), 0x0E }, { OS08E10_REG8(0x0A, 0x26), 0x11 },
	{ OS08E10_REG8(0x0A, 0x27), 0x43 }, { OS08E10_REG8(0x0A, 0x28), 0x8C },
	{ OS08E10_REG8(0x0A, 0x29), 0x93 }, { OS08E10_REG8(0x0A, 0x2A), 0xFE },
	{ OS08E10_REG8(0x0A, 0x2B), 0xAA }, { OS08E10_REG8(0x0A, 0x2C), 0xBB },
	{ OS08E10_REG8(0x0A, 0x2D), 0xAF }, { OS08E10_REG8(0x0B, 0x86), 0x08 },
	{ OS08E10_REG8(0x0B, 0x87), 0x0C }, { OS08E10_REG8(0x0B, 0x88), 0x47 },
	{ OS08E10_REG8(0x0B, 0x89), 0xB0 }, { OS08E10_REG8(0x0B, 0x8B), 0x69 },
	{ OS08E10_REG8(0x0B, 0x8C), 0x99 }, { OS08E10_REG8(0x0B, 0x1B), 0x00 },
	{ OS08E10_REG8(0x0B, 0x1C), 0x7C }, { OS08E10_REG8(0x0B, 0x85), 0x08 },
	{ OS08E10_REG8(0x0B, 0x83), 0xCC }, { OS08E10_REG8(0x0B, 0x84), 0x98 },
	{ OS08E10_REG8(0x07, 0x00), 0xFF }, { OS08E10_REG8(0x07, 0x01), 0x60 },
	{ OS08E10_REG8(0x07, 0x0F), 0x8A }, { OS08E10_REG8(0x07, 0x10), 0xF0 },
	{ OS08E10_REG8(0x07, 0x16), 0x00 }, { OS08E10_REG8(0x07, 0x17), 0x08 },
	{ OS08E10_REG8(0x07, 0x42), 0x00 }, { OS08E10_REG8(0x07, 0x43), 0x79 },
	{ OS08E10_REG8(0x07, 0x44), 0x00 }, { OS08E10_REG8(0x07, 0x46), 0x00 },
	{ OS08E10_REG8(0x07, 0x48), 0x00 }, { OS08E10_REG8(0x07, 0x49), 0x79 },
	{ OS08E10_REG8(0x07, 0x4C), 0x30 }, { OS08E10_REG8(0x07, 0x4D), 0x00 },
	{ OS08E10_REG8(0x07, 0x4E), 0x10 }, { OS08E10_REG8(0x07, 0xB0), 0x00 },
	{ OS08E10_REG8(0x07, 0xBB), 0x00 }, { OS08E10_REG8(0x07, 0xDB), 0x04 },
	{ OS08E10_REG8(0x04, 0x88), 0x80 }, { OS08E10_REG8(0x0C, 0x00), 0x04 },
	{ OS08E10_REG8(0x0C, 0x01), 0xAD }, { OS08E10_REG8(0x0C, 0x0C), 0x05 },
	{ OS08E10_REG8(0x0C, 0x02), 0xBC }, { OS08E10_REG8(0x0C, 0x03), 0xB8 },
	{ OS08E10_REG8(0x0C, 0x04), 0x1A }, { OS08E10_REG8(0x0C, 0x05), 0x18 },
	{ OS08E10_REG8(0x02, 0xB0), 0x00 }, { OS08E10_REG8(0x02, 0xB1), 0x04 },
	{ OS08E10_REG8(0x02, 0xB2), 0x08 }, { OS08E10_REG8(0x02, 0xB3), 0x70 },
	{ OS08E10_REG8(0x02, 0xB4), 0x00 }, { OS08E10_REG8(0x02, 0xB5), 0x04 },
	{ OS08E10_REG8(0x02, 0xB6), 0x0F }, { OS08E10_REG8(0x02, 0xB7), 0x00 },
};

/* RAW10 at 726MHz link, 60fps */
static const struct os08e10_reg os08e10_4k_raw10_726_config[] = {
	{ OS08E10_REG_PSNC_RST_EN, 0x60 },
	{ OS08E10_REG_DAC_LOAD_HCG_6X, 0x92 },
	{ OS08E10_REG_P56, 0x00 },
	{ OS08E10_REG_P57, 0xD5 },
	{ OS08E10_REG_P5E, 0x16 },
	{ OS08E10_REG_P7B, 0x27 },
	{ OS08E10_REG_P89, 0x0E },
	{ OS08E10_REG_P8A, 0x50 },
	{ OS08E10_REG_P91, 0x1A },
	{ OS08E10_REG_P92, 0x30 },
	{ OS08E10_REG_P95, 0x50 },
	{ OS08E10_REG_PAA, 0x0B },
	{ OS08E10_REG_PC3, 0x0B },
	{ OS08E10_REG_P86_1X, 0x48 },
	{ OS08E10_REG_P86_2X, 0x4B },
	{ OS08E10_REG_P86_3X, 0x4B },
	{ OS08E10_REG_P88_1X, 0x47 },
	{ OS08E10_REG_P88_2X, 0x4A },
	{ OS08E10_REG_P88_3X, 0x4A },
	{ OS08E10_REG_DAC_BITS_SEL, 0x40 },
	{ OS08E10_REG_BLC_45, 0x79 },
	{ OS08E10_REG_BLC_47, 0x79 },
	{ OS08E10_REG_RAW_12_EN, 0x00 },
	{ OS08E10_REG_DATA_ID1, 0x2B },
	{ OS08E10_REG_DATA_ID2, 0x2B },
	{ OS08E10_REG_DATA_ID3, 0x2B },
	{ OS08E10_REG_R_INIT, 0x4E20 },
	{ OS08E10_REG_R_LPX_DAT, 0x0C },
	{ OS08E10_REG_R_HS_PREPARE, 0x0B },
	{ OS08E10_REG_R_HS_ZERO, 0x15 },
	{ OS08E10_REG_R_HS_TRAIL, 0x0F },
	{ OS08E10_REG_R_EXIT, 0x0D },
	{ OS08E10_REG_R_LPX_CK, 0x0C },
	{ OS08E10_REG_R_CLK_PREPARE, 0x0C },
	{ OS08E10_REG_R_CLK_ZERO, 0x36 },
	{ OS08E10_REG_R_CLK_POST, 0x14 },
	{ OS08E10_REG_R_CLK_TRAIL, 0x0F },
};

/* RAW12 at 726MHz link, 30fps */
static const struct os08e10_reg os08e10_4k_raw12_726_config[] = {
	{ OS08E10_REG_PSNC_RST_EN, 0x61 },
	{ OS08E10_REG_DAC_LOAD_HCG_6X, 0x93 },
	{ OS08E10_REG_P56, 0x01 },
	{ OS08E10_REG_P57, 0xDD },
	{ OS08E10_REG_P5E, 0x14 },
	{ OS08E10_REG_P7B, 0x0F },
	{ OS08E10_REG_P89, 0x00 },
	{ OS08E10_REG_P8A, 0x00 },
	{ OS08E10_REG_P91, 0x1E },
	{ OS08E10_REG_P92, 0x84 },
	{ OS08E10_REG_P95, 0xF6 },
	{ OS08E10_REG_PAA, 0x18 },
	{ OS08E10_REG_PC3, 0x18 },
	{ OS08E10_REG_P86_1X, 0x48 },
	{ OS08E10_REG_P86_2X, 0x4D },
	{ OS08E10_REG_P86_3X, 0x4D },
	{ OS08E10_REG_P88_1X, 0x47 },
	{ OS08E10_REG_P88_2X, 0x4C },
	{ OS08E10_REG_P88_3X, 0x4C },
	{ OS08E10_REG_DAC_BITS_SEL, 0x80 },
	{ OS08E10_REG_BLC_45, 0x7A },
	{ OS08E10_REG_BLC_47, 0x7A },
	{ OS08E10_REG_RAW_12_EN, 0x02 },
	{ OS08E10_REG_DATA_ID1, 0x2C },
	{ OS08E10_REG_DATA_ID2, 0x2C },
	{ OS08E10_REG_DATA_ID3, 0x2C },
	{ OS08E10_REG_R_INIT, 0x4E20 },
	{ OS08E10_REG_R_LPX_DAT, 0x0C },
	{ OS08E10_REG_R_HS_PREPARE, 0x0B },
	{ OS08E10_REG_R_HS_ZERO, 0x15 },
	{ OS08E10_REG_R_HS_TRAIL, 0x0F },
	{ OS08E10_REG_R_EXIT, 0x0D },
	{ OS08E10_REG_R_LPX_CK, 0x0C },
	{ OS08E10_REG_R_CLK_PREPARE, 0x0C },
	{ OS08E10_REG_R_CLK_ZERO, 0x36 },
	{ OS08E10_REG_R_CLK_POST, 0x14 },
	{ OS08E10_REG_R_CLK_TRAIL, 0x0F },
};

/* RAW10 at 369MHz link, 30fps */
static const struct os08e10_reg os08e10_4k_raw10_369_config[] = {
	{ OS08E10_REG_PSNC_RST_EN, 0x60 },
	{ OS08E10_REG_DAC_LOAD_HCG_6X, 0x92 },
	{ OS08E10_REG_P56, 0x00 },
	{ OS08E10_REG_P57, 0xD5 },
	{ OS08E10_REG_P5E, 0x16 },
	{ OS08E10_REG_P7B, 0x20 },
	{ OS08E10_REG_P89, 0x0E },
	{ OS08E10_REG_P8A, 0x50 },
	{ OS08E10_REG_P91, 0x1A },
	{ OS08E10_REG_P92, 0x30 },
	{ OS08E10_REG_P95, 0x50 },
	{ OS08E10_REG_PAA, 0x0B },
	{ OS08E10_REG_PC3, 0x0B },
	{ OS08E10_REG_P86_1X, 0x44 },
	{ OS08E10_REG_P86_2X, 0x48 },
	{ OS08E10_REG_P86_3X, 0x48 },
	{ OS08E10_REG_P88_1X, 0x43 },
	{ OS08E10_REG_P88_2X, 0x47 },
	{ OS08E10_REG_P88_3X, 0x47 },
	{ OS08E10_REG_DAC_BITS_SEL, 0x40 },
	{ OS08E10_REG_BLC_45, 0x79 },
	{ OS08E10_REG_BLC_47, 0x79 },
	{ OS08E10_REG_RAW_12_EN, 0x00 },
	{ OS08E10_REG_DATA_ID1, 0x2B },
	{ OS08E10_REG_DATA_ID2, 0x2B },
	{ OS08E10_REG_DATA_ID3, 0x2B },
	{ OS08E10_REG_R_INIT, 0x2710 },
	{ OS08E10_REG_R_LPX_DAT, 0x07 },
	{ OS08E10_REG_R_HS_PREPARE, 0x07 },
	{ OS08E10_REG_R_HS_ZERO, 0x0B },
	{ OS08E10_REG_R_HS_TRAIL, 0x09 },
	{ OS08E10_REG_R_EXIT, 0x07 },
	{ OS08E10_REG_R_LPX_CK, 0x06 },
	{ OS08E10_REG_R_CLK_PREPARE, 0x06 },
	{ OS08E10_REG_R_CLK_ZERO, 0x1B },
	{ OS08E10_REG_R_CLK_POST, 0x0F },
	{ OS08E10_REG_R_CLK_TRAIL, 0x08 },
};

static const struct os08e10_reg os08e10_mipi_init[] = {
	{ OS08E10_REG8(0x02, 0x99), 0x00 }, { OS08E10_REG8(0x02, 0x9A), 0x0F },
	{ OS08E10_REG8(0x02, 0x9B), 0x00 }, { OS08E10_REG8(0x02, 0x9C), 0x0F },
	{ OS08E10_REG8(0x02, 0x9D), 0x00 }, { OS08E10_REG8(0x02, 0x9E), 0x0F },
	{ OS08E10_REG8(0x02, 0xA1), 0x70 }, { OS08E10_REG8(0x02, 0xA2), 0x08 },
	{ OS08E10_REG8(0x02, 0x66), 0xCC }, { OS08E10_REG8(0x02, 0x6D), 0x07 },
	{ OS08E10_REG8(0x02, 0x79), 0x1B }, { OS08E10_REG8(0x02, 0x7D), 0x07 },
	{ OS08E10_REG8(0x02, 0x6B), 0x00 }, { OS08E10_REG8(0x02, 0xA3), 0x01 },
};

static const char *const os08e10_test_pattern_menu[] = {
	"Disabled",
	"Color Bars",
	"Gradient Color Bars",
};

static const unsigned int os08e10_test_pattern_val[] = {
	OS08E10_TEST_PATTERN_DISABLED,
	OS08E10_TEST_PATTERN_COLOR_BARS,
	OS08E10_TEST_PATTERN_GRADIENT,
};

/* regulator supplies */
static const char *const os08e10_supply_names[] = {
	/* Supplies must be enabled in this order */
	"vdig", /* Digital I/O DOVDD (1.8V) supply */
	"vana", /* Analog AVDD (2.8V) supply */
	"vddl", /* Digital Core DVDD (1.2V) supply */
};

static const struct os08e10_mode os08e10_modes_raw10_726[] = {
	{
		.width = 3840,
		.height = 2160,
		.crop = {
			.left = OS08E10_PIXEL_ARRAY_LEFT,
			.top = OS08E10_PIXEL_ARRAY_TOP,
			.width = 3840,
			.height = 2160,
		},
		.hts = 357,
		.vts = 2334,
		.reg_sequence = {
			.regs = os08e10_4k_raw10_726_config,
			.num_regs = ARRAY_SIZE(os08e10_4k_raw10_726_config),
		},
	},
};

static const struct os08e10_mode os08e10_modes_raw12_726[] = {
	{
		.width = 3840,
		.height = 2160,
		.crop = {
			.left = OS08E10_PIXEL_ARRAY_LEFT,
			.top = OS08E10_PIXEL_ARRAY_TOP,
			.width = 3840,
			.height = 2160,
		},
		.hts = 635,
		.vts = 2624,
		.reg_sequence = {
			.regs = os08e10_4k_raw12_726_config,
			.num_regs = ARRAY_SIZE(os08e10_4k_raw12_726_config),
		},
	},
};

static const struct os08e10_mode os08e10_modes_raw10_369[] = {
	{
		.width = 3840,
		.height = 2160,
		.crop = {
			.left = OS08E10_PIXEL_ARRAY_LEFT,
			.top = OS08E10_PIXEL_ARRAY_TOP,
			.width = 3840,
			.height = 2160,
		},
		.hts = 350,
		.vts = 2380,
		.reg_sequence = {
			.regs = os08e10_4k_raw10_369_config,
			.num_regs = ARRAY_SIZE(os08e10_4k_raw10_369_config),
		},
	},
};

struct os08e10_format {
	u32 code;
	const struct os08e10_mode *modes;
	unsigned int num_modes;
};

static const struct os08e10_format os08e10_formats_726[] = {
	{
		.code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.modes = os08e10_modes_raw10_726,
		.num_modes = ARRAY_SIZE(os08e10_modes_raw10_726),
	},
	{
		.code = MEDIA_BUS_FMT_SBGGR12_1X12,
		.modes = os08e10_modes_raw12_726,
		.num_modes = ARRAY_SIZE(os08e10_modes_raw12_726),
	},
};

static const struct os08e10_format os08e10_formats_369[] = {
	{
		.code = MEDIA_BUS_FMT_SBGGR10_1X10,
		.modes = os08e10_modes_raw10_369,
		.num_modes = ARRAY_SIZE(os08e10_modes_raw10_369),
	},
};

struct os08e10_pll_config {
	s64 freq_link;
	u32 freq_extclk;
	u32 freq_rowclk;
	struct os08e10_reg_sequence regs_pll;
	const struct os08e10_format *formats;
	unsigned int num_formats;
};

static const struct os08e10_pll_config os08e10_pll_configs[] = {
	{
		.freq_link = OS08E10_FREQ_LINK_1452MBPS,
		.freq_extclk = OS08E10_FREQ_EXTCLK,
		.freq_rowclk = OS08E10_FREQ_ROWCLK_50MHZ,
		.regs_pll = {
			.regs = os08e10_pll_config_24_726,
			.num_regs = ARRAY_SIZE(os08e10_pll_config_24_726),
		},
		.formats = os08e10_formats_726,
		.num_formats = ARRAY_SIZE(os08e10_formats_726),
	},
	{
		.freq_link = OS08E10_FREQ_LINK_738MBPS,
		.freq_extclk = OS08E10_FREQ_EXTCLK,
		.freq_rowclk = OS08E10_FREQ_ROWCLK_25MHZ,
		.regs_pll = {
			.regs = os08e10_pll_config_24_369,
			.num_regs = ARRAY_SIZE(os08e10_pll_config_24_369),
		},
		.formats = os08e10_formats_369,
		.num_formats = ARRAY_SIZE(os08e10_formats_369),
	},
};

struct os08e10_hw_config {
	struct clk *extclk;
	struct regulator_bulk_data supplies[OS08E10_NUM_SUPPLIES];
	struct gpio_desc *gpio_reset;
	unsigned int num_data_lanes;
};

struct os08e10 {
	struct device *dev;
	struct os08e10_hw_config hw_config;
	struct os08e10_pll_config const *pll_config;

	struct regmap *regmap;
	u8 page;

	struct v4l2_subdev sd;
	struct media_pad pad[NUM_PADS];

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
};

static inline struct os08e10 *to_os08e10(struct v4l2_subdev *_sd)
{
	return container_of(_sd, struct os08e10, sd);
}

static int os08e10_set_page(struct os08e10 *os08e10, u8 page)
{
	int ret;

	if (os08e10->page == page)
		return 0;

	ret = regmap_write(os08e10->regmap, OS08E10_WR_RD_CTRL, page);
	os08e10->page = ret ? OS08E10_PAGE_INVALID : page;

	return ret;
}

static void os08e10_reg_to_bytes(u32 reg, u32 val, u8 *buf)
{
	unsigned int width = OS08E10_REG_WIDTH(reg);
	unsigned int i;

	for (i = 0; i < width; i++) {
		unsigned int shift = reg & OS08E10_REG_LE ? i : width - i - 1;

		buf[i] = val >> (8 * shift);
	}
}

static int os08e10_write(struct os08e10 *os08e10, u32 reg, u32 val, int *err)
{
	u8 buf[sizeof(u32)];
	int ret;

	if (err && *err)
		return *err;

	ret = os08e10_set_page(os08e10, OS08E10_REG_PAGE(reg));
	if (!ret) {
		os08e10_reg_to_bytes(reg, val, buf);
		ret = regmap_bulk_write(os08e10->regmap, OS08E10_REG_ADDR(reg),
					buf, OS08E10_REG_WIDTH(reg));
	}

	/* Page select does not survive register file reset */
	if (reg == OS08E10_REG_SOFT_AUTO_RELEASE_EN)
		os08e10->page = OS08E10_PAGE_INVALID;

	if (err)
		*err = ret;

	return ret;
}

static int os08e10_read(struct os08e10 *os08e10, u32 reg, u32 *val)
{
	unsigned int width = OS08E10_REG_WIDTH(reg);
	u8 buf[sizeof(u32)];
	unsigned int i;
	int ret;

	ret = os08e10_set_page(os08e10, OS08E10_REG_PAGE(reg));
	if (ret)
		return ret;

	ret = regmap_bulk_read(os08e10->regmap, OS08E10_REG_ADDR(reg), buf,
			       width);
	if (ret)
		return ret;

	*val = 0;
	for (i = 0; i < width; i++) {
		unsigned int shift = reg & OS08E10_REG_LE ? i : width - i - 1;

		*val |= (u32)buf[i] << (8 * shift);
	}

	return 0;
}

static int os08e10_write_regs(struct os08e10 *os08e10,
			      const struct os08e10_reg *regs,
			      unsigned int num_regs, int *err)
{
	unsigned int i = 0;
	int ret = 0;

	if (err && *err)
		return *err;

	while (i < num_regs && !ret) {
		u8 page = OS08E10_REG_PAGE(regs[i].reg);
		u8 addr = OS08E10_REG_ADDR(regs[i].reg);
		u8 buf[OS08E10_BURST_MAX];
		unsigned int len;

		os08e10_reg_to_bytes(regs[i].reg, regs[i].val, buf);
		len = OS08E10_REG_WIDTH(regs[i].reg);
		i++;

		/* Burst runs of consecutive addresses below the page select */
		while (i < num_regs && OS08E10_REG_PAGE(regs[i].reg) == page &&
		       OS08E10_REG_ADDR(regs[i].reg) == addr + len &&
		       addr + len + OS08E10_REG_WIDTH(regs[i].reg) <=
			       OS08E10_WR_RD_CTRL &&
		       len + OS08E10_REG_WIDTH(regs[i].reg) <= sizeof(buf)) {
			os08e10_reg_to_bytes(regs[i].reg, regs[i].val,
					     &buf[len]);
			len += OS08E10_REG_WIDTH(regs[i].reg);
			i++;
		}

		ret = os08e10_set_page(os08e10, page);
		if (!ret)
			ret = regmap_bulk_write(os08e10->regmap, addr, buf,
						len);
	}

	if (err)
		*err = ret;

	return ret;
}

static const struct os08e10_format *os08e10_get_format(struct os08e10 *os08e10,
						       u32 code)
{
	const struct os08e10_pll_config *pll_config = os08e10->pll_config;
	unsigned int i;

	for (i = 0; i < pll_config->num_formats; i++)
		if (pll_config->formats[i].code == code)
			return &pll_config->formats[i];

	return &pll_config->formats[0];
}

static const struct os08e10_mode *
os08e10_state_get_mode(struct os08e10 *os08e10, struct v4l2_subdev_state *state)
{
	const struct os08e10_format *format;
	struct v4l2_mbus_framefmt *fmt;

	fmt = v4l2_subdev_state_get_format(state, IMAGE_PAD);
	format = os08e10_get_format(os08e10, fmt->code);

	return v4l2_find_nearest_size(format->modes, format->num_modes, width,
				      height, fmt->width, fmt->height);
}

static void os08e10_adjust_exposure_range(struct os08e10 *os08e10,
					  const struct os08e10_mode *mode)
{
	int exp_max =
		mode->height + os08e10->vblank->val - OS08E10_EXPOSURE_OFFSET;

	__v4l2_ctrl_modify_range(os08e10->exposure, os08e10->exposure->minimum,
				 exp_max, os08e10->exposure->step, exp_max);
}

static int os08e10_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct os08e10 *os08e10 =
		container_of(ctrl->handler, struct os08e10, ctrl_handler);
	struct i2c_client *client = v4l2_get_subdevdata(&os08e10->sd);
	struct v4l2_subdev_state *state;
	const struct os08e10_mode *mode;
	u32 val;
	int ret = 0;

	state = v4l2_subdev_get_locked_active_state(&os08e10->sd);
	mode = os08e10_state_get_mode(os08e10, state);

	if (ctrl->id == V4L2_CID_VBLANK)
		os08e10_adjust_exposure_range(os08e10, mode);

	/*
	 * Applying V4L2 control value only happens
	 * when power is up for streaming
	 */
	if (pm_runtime_get_if_in_use(&client->dev) == 0)
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = os08e10_write(os08e10, OS08E10_REG_ANA_GAIN1, ctrl->val,
				    NULL);
		ret = os08e10_write(os08e10, OS08E10_REG_TRIGGER,
				    OS08E10_TRIGGER, &ret);
		break;
	case V4L2_CID_EXPOSURE:
		ret = os08e10_write(os08e10, OS08E10_REG_EXP1, ctrl->val, NULL);
		ret = os08e10_write(os08e10, OS08E10_REG_TRIGGER,
				    OS08E10_TRIGGER, &ret);
		break;
	case V4L2_CID_DIGITAL_GAIN:
		ret = os08e10_write(os08e10, OS08E10_REG_DIG_GAIN1, ctrl->val,
				    NULL);
		ret = os08e10_write(os08e10, OS08E10_REG_TRIGGER,
				    OS08E10_TRIGGER, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		/* Pattern generator clock is gated unless a pattern is on */
		val = OS08E10_CLK_GATING_DEFAULT;
		if (ctrl->val)
			val |= OS08E10_DCLKIN_TP_GATING_EN;
		ret = os08e10_write(os08e10,
				    OS08E10_REG_DCLKIN_CISISP_GATING_EN, val,
				    NULL);
		ret = os08e10_write(os08e10, OS08E10_REG_PATTERN_SWITCH_EN,
				    os08e10_test_pattern_val[ctrl->val], &ret);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = os08e10_write(os08e10, OS08E10_REG_ISP_MODE,
				    OS08E10_ISP_MODE_AUTO_BAYER, NULL);
		val = (os08e10->vflip->val << 1) | os08e10->hflip->val;
		ret = os08e10_write(os08e10, OS08E10_REG_MIRROR_FLIP, val,
				    &ret);
		break;
	case V4L2_CID_VBLANK:
		ret = os08e10_write(os08e10, OS08E10_REG_VBLANK_BUF,
				    mode->height + ctrl->val - OS08E10_VTS_BASE,
				    NULL);
		ret = os08e10_write(os08e10, OS08E10_REG_TRIGGER,
				    OS08E10_TRIGGER, &ret);
		break;
	case V4L2_CID_HBLANK:
		/* Read only, line length is set with mode registers */
		break;
	default:
		dev_info(&client->dev,
			 "ctrl(id:0x%x,val:0x%x) is not handled\n", ctrl->id,
			 ctrl->val);
		ret = -EINVAL;
		break;
	}

	pm_runtime_mark_last_busy(&client->dev);
	pm_runtime_put_autosuspend(&client->dev);

	return ret;
}

static const struct v4l2_ctrl_ops os08e10_ctrl_ops = {
	.s_ctrl = os08e10_set_ctrl,
};

static int os08e10_enum_mbus_code(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_mbus_code_enum *code)
{
	struct os08e10 *os08e10 = to_os08e10(sd);
	const struct os08e10_pll_config *pll_config = os08e10->pll_config;

	if (code->pad >= NUM_PADS)
		return -EINVAL;

	if (code->index >= pll_config->num_formats)
		return -EINVAL;

	code->code = pll_config->formats[code->index].code;

	return 0;
}

static int os08e10_enum_frame_size(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *sd_state,
				   struct v4l2_subdev_frame_size_enum *fse)
{
	struct os08e10 *os08e10 = to_os08e10(sd);
	const struct os08e10_format *format;
	const struct os08e10_mode *mode;

	if (fse->pad >= NUM_PADS)
		return -EINVAL;

	format = os08e10_get_format(os08e10, fse->code);
	if (format->code != fse->code || fse->index >= format->num_modes)
		return -EINVAL;

	mode = &format->modes[fse->index];
	fse->min_width = mode->width;
	fse->max_width = fse->min_width;
	fse->min_height = mode->height;
	fse->max_height = fse->min_height;

	return 0;
}

static void os08e10_reset_colorspace(struct v4l2_mbus_framefmt *fmt)
{
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_MAP_QUANTIZATION_DEFAULT(true, fmt->colorspace,
							  fmt->ycbcr_enc);
	fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
}

static void os08e10_update_image_pad_format(const struct os08e10_mode *mode,
					    struct v4l2_subdev_format *fmt)
{
	fmt->format.width = mode->width;
	fmt->format.height = mode->height;
	fmt->format.field = V4L2_FIELD_NONE;
	os08e10_reset_colorspace(&fmt->format);
}

static void os08e10_set_framing_limits(struct os08e10 *os08e10,
				       const struct os08e10_mode *mode)
{
	int vblank_min = mode->vts - mode->height;
	int hblank;

	/* Update limits and set FPS to default */
	__v4l2_ctrl_modify_range(os08e10->vblank, vblank_min,
				 OS08E10_VTS_MAX - mode->height,
				 os08e10->vblank->step, vblank_min);

	/* Setting this will adjust the exposure limits as well */
	__v4l2_ctrl_s_ctrl(os08e10->vblank, vblank_min);

	hblank = mode->hts * OS08E10_PIXCLK_PER_ROWCLK - mode->width;
	__v4l2_ctrl_modify_range(os08e10->hblank, hblank, hblank, 1, hblank);
}

static int os08e10_set_pad_format(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_format *fmt)
{
	struct os08e10 *os08e10 = to_os08e10(sd);
	const struct os08e10_format *format;
	const struct os08e10_mode *mode;

	if (fmt->pad >= NUM_PADS)
		return -EINVAL;

	format = os08e10_get_format(os08e10, fmt->format.code);
	fmt->format.code = format->code;

	mode = v4l2_find_nearest_size(format->modes, format->num_modes, width,
				      height, fmt->format.width,
				      fmt->format.height);
	os08e10_update_image_pad_format(mode, fmt);

	*v4l2_subdev_state_get_format(sd_state, IMAGE_PAD) = fmt->format;

	*v4l2_subdev_state_get_crop(sd_state, IMAGE_PAD) = mode->crop;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		os08e10_set_framing_limits(os08e10, mode);

	return 0;
}

static int os08e10_get_selection(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_selection *sel)
{
	if (sel->pad != IMAGE_PAD)
		return -EINVAL;

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(sd_state, IMAGE_PAD);

		return 0;

	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = OS08E10_NATIVE_WIDTH;
		sel->r.height = OS08E10_NATIVE_HEIGHT;

		return 0;

	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.top = OS08E10_PIXEL_ARRAY_TOP;
		sel->r.left = OS08E10_PIXEL_ARRAY_LEFT;
		sel->r.width = OS08E10_PIXEL_ARRAY_WIDTH;
		sel->r.height = OS08E10_PIXEL_ARRAY_HEIGHT;

		return 0;
	}

	return -EINVAL;
}

static int os08e10_soft_reset(struct os08e10 *os08e10)
{
	int ret;

	ret = os08e10_write(os08e10, OS08E10_REG_SOFT_AUTO_RELEASE_EN,
			    OS08E10_RESET_ALL, NULL);
	ret = os08e10_write_regs(os08e10, os08e10_preinit,
				 ARRAY_SIZE(os08e10_preinit), &ret);
	ret = os08e10_write(os08e10, OS08E10_REG_SOFT_AUTO_RELEASE_EN,
			    OS08E10_RESET_LOGIC, &ret);
	if (ret)
		return ret;

	usleep_range(OS08E10_SOFT_RESET_DELAY_MIN_US,
		     OS08E10_SOFT_RESET_DELAY_MIN_US + OS08E10_DELAY_RANGE_US);

	return 0;
}

static inline int
os08e10_reg_seq_write(struct os08e10 *os08e10,
		      struct os08e10_reg_sequence const *reg_sequence)
{
	return os08e10_write_regs(os08e10, reg_sequence->regs,
				  reg_sequence->num_regs, NULL);
}

static int os08e10_stream_on(struct os08e10 *os08e10)
{
	int ret;

	ret = os08e10_write(os08e10, OS08E10_REG_TRIGGER, OS08E10_TRIGGER_INIT,
			    NULL);
	ret = os08e10_write(os08e10, OS08E10_REG_MIPI_RST_FIX_EN,
			    OS08E10_MIPI_RST_FIX, &ret);

	return os08e10_write(os08e10, OS08E10_REG_SOFT_AUTO_RELEASE_EN,
			     OS08E10_RESET_NONE, &ret);
}

static int os08e10_enable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct os08e10 *os08e10 = to_os08e10(sd);
	struct device *dev = os08e10->dev;
	const struct os08e10_mode *mode;
	int ret;

	ret = pm_runtime_resume_and_get(dev);
	if (ret < 0)
		return ret;

	ret = os08e10_soft_reset(os08e10);
	if (ret < 0) {
		dev_err(dev, "%s failed to reset\n", __func__);
		goto err_rpm_put;
	}

	/* PLL config */
	ret = os08e10_reg_seq_write(os08e10, &os08e10->pll_config->regs_pll);
	if (ret < 0) {
		dev_err(dev, "%s failed to configure pll settings\n", __func__);
		goto err_rpm_put;
	}

	/* Common */
	ret = os08e10_write_regs(os08e10, os08e10_common_init,
				 ARRAY_SIZE(os08e10_common_init), NULL);
	if (ret < 0) {
		dev_err(dev, "%s failed to set common settings\n", __func__);
		goto err_rpm_put;
	}

	/* Apply default values of current frame format */
	mode = os08e10_state_get_mode(os08e10, state);
	ret = os08e10_reg_seq_write(os08e10, &mode->reg_sequence);
	if (ret < 0) {
		dev_err(dev, "%s failed to set frame format\n", __func__);
		goto err_rpm_put;
	}

	/* MIPI config */
	ret = os08e10_write_regs(os08e10, os08e10_mipi_init,
				 ARRAY_SIZE(os08e10_mipi_init), NULL);
	if (ret < 0) {
		dev_err(dev, "%s failed to configure mipi settings\n",
			__func__);
		goto err_rpm_put;
	}

	/* Apply customized values from user */
	ret = __v4l2_ctrl_handler_setup(os08e10->sd.ctrl_handler);
	if (ret)
		goto err_rpm_put;

	ret = os08e10_stream_on(os08e10);
	if (ret)
		goto err_rpm_put;

	/* vflip and hflip cannot change during streaming */
	__v4l2_ctrl_grab(os08e10->vflip, true);
	__v4l2_ctrl_grab(os08e10->hflip, true);

	return 0;

err_rpm_put:
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static int os08e10_disable_streams(struct v4l2_subdev *sd,
				   struct v4l2_subdev_state *state, u32 pad,
				   u64 streams_mask)
{
	struct os08e10 *os08e10 = to_os08e10(sd);
	struct device *dev = os08e10->dev;
	int ret;

	ret = os08e10_write(os08e10, OS08E10_REG_SOFT_AUTO_RELEASE_EN,
			    OS08E10_RESET_ALL, NULL);
	if (ret < 0)
		dev_err(dev, "%s failed to stop streaming\n", __func__);

	__v4l2_ctrl_grab(os08e10->vflip, false);
	__v4l2_ctrl_grab(os08e10->hflip, false);

	pm_runtime_mark_last_busy(dev);
	pm_runtime_put_autosuspend(dev);

	return ret;
}

static int os08e10_power_on(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct os08e10 *os08e10 = to_os08e10(sd);
	unsigned int i;
	int ret;

	/* regulator_bulk_enable is unordered */
	for (i = 0; i < OS08E10_NUM_SUPPLIES; i++) {
		ret = regulator_enable(os08e10->hw_config.supplies[i].consumer);
		if (ret) {
			dev_err(dev, "%s: failed to enable regulators\n",
				__func__);
			goto reg_off;
		}
	}

	ret = clk_prepare_enable(os08e10->hw_config.extclk);
	if (ret) {
		dev_err(dev, "%s: failed to enable clock\n", __func__);
		goto reg_off;
	}

	usleep_range(OS08E10_XSHUTDOWN_DELAY_MIN_US,
		     OS08E10_XSHUTDOWN_DELAY_MIN_US + OS08E10_DELAY_RANGE_US);
	gpiod_set_value_cansleep(os08e10->hw_config.gpio_reset, 0);
	usleep_range(OS08E10_SCCB_DELAY_MIN_US,
		     OS08E10_SCCB_DELAY_MIN_US + OS08E10_DELAY_RANGE_US);

	os08e10->page = OS08E10_PAGE_INVALID;

	return 0;

reg_off:
	regulator_bulk_disable(i, os08e10->hw_config.supplies);

	return ret;
}

static int os08e10_power_off(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct os08e10 *os08e10 = to_os08e10(sd);

	gpiod_set_value_cansleep(os08e10->hw_config.gpio_reset, 1);
	clk_disable_unprepare(os08e10->hw_config.extclk);
	regulator_bulk_disable(OS08E10_NUM_SUPPLIES,
			       os08e10->hw_config.supplies);

	return 0;
}

static int os08e10_identify_module(struct os08e10 *os08e10)
{
	int ret;
	u32 reg_val;

	ret = os08e10_read(os08e10, OS08E10_REG_CHIP_ID, &reg_val);
	if (ret < 0)
		return dev_err_probe(os08e10->dev, ret,
				     "failed to read chip id\n");

	if (reg_val != OS08E10_CHIP_ID)
		return dev_err_probe(os08e10->dev, -EIO,
				     "Invalid chip id: 0x%x\n", reg_val);

	dev_info(os08e10->dev, "Success reading chip id: 0x%x\n", reg_val);

	return ret;
}

static int os08e10_init_state(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state)
{
	struct os08e10 *os08e10 = to_os08e10(sd);
	const struct os08e10_format *format = &os08e10->pll_config->formats[0];
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = IMAGE_PAD,
		.format = {
			.code = format->code,
			.width = format->modes[0].width,
			.height = format->modes[0].height,
		},
	};

	os08e10_set_pad_format(sd, state, &fmt);

	return 0;
}

static const struct v4l2_subdev_core_ops os08e10_core_ops = {
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops os08e10_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops os08e10_pad_ops = {
	.enum_mbus_code = os08e10_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = os08e10_set_pad_format,
	.get_selection = os08e10_get_selection,
	.enum_frame_size = os08e10_enum_frame_size,
	.enable_streams = os08e10_enable_streams,
	.disable_streams = os08e10_disable_streams,
};

static const struct v4l2_subdev_ops os08e10_subdev_ops = {
	.core = &os08e10_core_ops,
	.video = &os08e10_video_ops,
	.pad = &os08e10_pad_ops,
};

static const struct v4l2_subdev_internal_ops os08e10_internal_ops = {
	.init_state = os08e10_init_state,
};

static int os08e10_init_controls(struct os08e10 *os08e10)
{
	struct i2c_client *client = v4l2_get_subdevdata(&os08e10->sd);
	const struct os08e10_mode *mode =
		&os08e10->pll_config->formats[0].modes[0];
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *ctrl_hdlr;
	struct v4l2_ctrl *ctrl;
	unsigned int pixel_rate;
	int exp_max, vblank_min, hblank;
	int ret;

	ctrl_hdlr = &os08e10->ctrl_handler;
	ret = v4l2_ctrl_handler_init(ctrl_hdlr, 12);
	if (ret)
		return ret;

	pixel_rate =
		os08e10->pll_config->freq_rowclk * OS08E10_PIXCLK_PER_ROWCLK;
	ctrl = v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops,
				 V4L2_CID_PIXEL_RATE, pixel_rate, pixel_rate, 1,
				 pixel_rate);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	/* Seed limits from default mode, subdev state does not exist yet */
	vblank_min = mode->vts - mode->height;
	os08e10->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops,
					    V4L2_CID_VBLANK, vblank_min,
					    OS08E10_VTS_MAX - mode->height, 1,
					    vblank_min);

	hblank = mode->hts * OS08E10_PIXCLK_PER_ROWCLK - mode->width;
	os08e10->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops,
					    V4L2_CID_HBLANK, hblank, hblank, 1,
					    hblank);
	if (os08e10->hblank)
		os08e10->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	exp_max = mode->vts - OS08E10_EXPOSURE_OFFSET;
	os08e10->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops,
					      V4L2_CID_EXPOSURE,
					      OS08E10_EXPOSURE_MIN, exp_max,
					      OS08E10_EXPOSURE_STEP, exp_max);

	v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  OS08E10_ANA_GAIN_MIN, OS08E10_ANA_GAIN_MAX,
			  OS08E10_ANA_GAIN_STEP, OS08E10_ANA_GAIN_DEFAULT);

	v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops, V4L2_CID_DIGITAL_GAIN,
			  OS08E10_DGTL_GAIN_MIN, OS08E10_DGTL_GAIN_MAX,
			  OS08E10_DGTL_GAIN_STEP, OS08E10_DGTL_GAIN_DEFAULT);

	os08e10->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops,
					   V4L2_CID_HFLIP, 0, 1, 1, 0);

	os08e10->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &os08e10_ctrl_ops,
					   V4L2_CID_VFLIP, 0, 1, 1, 0);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &os08e10_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(os08e10_test_pattern_menu) - 1,
				     0, 0, os08e10_test_pattern_menu);

	ctrl = v4l2_ctrl_new_int_menu(ctrl_hdlr, &os08e10_ctrl_ops,
				      V4L2_CID_LINK_FREQ, 0, 0,
				      &os08e10->pll_config->freq_link);
	if (ctrl)
		ctrl->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	ret = v4l2_fwnode_device_parse(&client->dev, &props);
	if (!ret)
		v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &os08e10_ctrl_ops,
						&props);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		dev_err(&client->dev, "%s control init failed (%d)\n", __func__,
			ret);
		goto error;
	}

	os08e10->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static void os08e10_free_controls(struct os08e10 *os08e10)
{
	v4l2_ctrl_handler_free(os08e10->sd.ctrl_handler);
}

static int os08e10_parse_hw_config(struct os08e10 *os08e10)
{
	struct device *dev = os08e10->dev;
	struct v4l2_fwnode_endpoint ep_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct fwnode_handle *endpoint;
	struct os08e10_hw_config *hw_config = &os08e10->hw_config;
	unsigned long extclk_frequency;
	int ret = -EINVAL;
	unsigned int i;

	for (i = 0; i < OS08E10_NUM_SUPPLIES; i++)
		hw_config->supplies[i].supply = os08e10_supply_names[i];

	ret = devm_regulator_bulk_get(dev, OS08E10_NUM_SUPPLIES,
				      hw_config->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	hw_config->gpio_reset =
		devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(hw_config->gpio_reset))
		return dev_err_probe(dev, PTR_ERR(hw_config->gpio_reset),
				     "failed to get reset gpio\n");

	hw_config->extclk = devm_clk_get(dev, "extclk");
	if (IS_ERR(hw_config->extclk))
		return dev_err_probe(dev, PTR_ERR(hw_config->extclk),
				     "failed to get extclk\n");

	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return dev_err_probe(dev, -ENXIO, "endpoint node not found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &ep_cfg);
	fwnode_handle_put(endpoint);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse endpoint\n");

	if (ep_cfg.bus.mipi_csi2.num_data_lanes != 4) {
		ret = dev_err_probe(dev, -EINVAL,
				    "invalid number of CSI2 data lanes %d\n",
				    ep_cfg.bus.mipi_csi2.num_data_lanes);
		goto error_out;
	}

	hw_config->num_data_lanes = ep_cfg.bus.mipi_csi2.num_data_lanes;

	if (!ep_cfg.nr_of_link_frequencies) {
		ret = dev_err_probe(dev, -EINVAL,
				    "link-frequency not found in DT\n");
		goto error_out;
	}

	extclk_frequency = clk_get_rate(hw_config->extclk);

	for (i = 0; i < ARRAY_SIZE(os08e10_pll_configs); i++) {
		if (os08e10_pll_configs[i].freq_extclk == extclk_frequency &&
		    os08e10_pll_configs[i].freq_link ==
			    ep_cfg.link_frequencies[0])
			break;
	}

	if (i == ARRAY_SIZE(os08e10_pll_configs)) {
		ret = dev_err_probe(dev, -EINVAL,
				    "no PLL config for %lu/%llu Hz\n",
				    extclk_frequency,
				    ep_cfg.link_frequencies[0]);
		goto error_out;
	}

	os08e10->pll_config = &os08e10_pll_configs[i];

	dev_info(dev, "extclk: %luHz, link: %lluHz, lanes: %d\n",
		 extclk_frequency, ep_cfg.link_frequencies[0],
		 hw_config->num_data_lanes);

	ret = 0;

error_out:
	v4l2_fwnode_endpoint_free(&ep_cfg);

	return ret;
}

static const struct regmap_config os08e10_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
};

static int os08e10_probe(struct i2c_client *client)
{
	struct os08e10 *os08e10;
	int ret;

	os08e10 = devm_kzalloc(&client->dev, sizeof(*os08e10), GFP_KERNEL);
	if (!os08e10)
		return -ENOMEM;

	os08e10->dev = &client->dev;

	v4l2_i2c_subdev_init(&os08e10->sd, client, &os08e10_subdev_ops);

	ret = os08e10_parse_hw_config(os08e10);
	if (ret)
		return ret;

	os08e10->regmap = devm_regmap_init_i2c(client, &os08e10_regmap_config);
	if (IS_ERR(os08e10->regmap))
		return PTR_ERR(os08e10->regmap);

	/*
	 * Enable power management. The driver supports runtime PM, but needs to
	 * work when runtime PM is disabled in the kernel. To that end, power
	 * the sensor on manually here and identify it
	 */
	ret = os08e10_power_on(os08e10->dev);
	if (ret)
		return ret;

	pm_runtime_set_active(os08e10->dev);
	pm_runtime_get_noresume(os08e10->dev);
	pm_runtime_enable(os08e10->dev);
	pm_runtime_set_autosuspend_delay(os08e10->dev, 1000);
	pm_runtime_use_autosuspend(os08e10->dev);

	ret = os08e10_identify_module(os08e10);
	if (ret)
		goto error_power_off;

	ret = os08e10_init_controls(os08e10);
	if (ret)
		goto error_power_off;

	os08e10->sd.internal_ops = &os08e10_internal_ops;
	os08e10->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
			     V4L2_SUBDEV_FL_HAS_EVENTS;
	os08e10->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;

	os08e10->pad[IMAGE_PAD].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&os08e10->sd.entity, NUM_PADS,
				     os08e10->pad);
	if (ret) {
		dev_err(os08e10->dev, "failed to init entity pads: %d\n", ret);
		goto error_handler_free;
	}

	os08e10->sd.state_lock = os08e10->ctrl_handler.lock;
	ret = v4l2_subdev_init_finalize(&os08e10->sd);
	if (ret) {
		dev_err(os08e10->dev, "failed to finalize subdev init: %d\n",
			ret);
		goto error_media_entity;
	}

	ret = v4l2_async_register_subdev_sensor(&os08e10->sd);
	if (ret < 0) {
		dev_err(os08e10->dev,
			"failed to register sensor sub-device: %d\n", ret);
		goto error_subdev_cleanup;
	}

	/* Drop the usage count, autosuspend then powers the sensor off */
	pm_runtime_mark_last_busy(os08e10->dev);
	pm_runtime_put_autosuspend(os08e10->dev);

	return 0;

error_subdev_cleanup:
	v4l2_subdev_cleanup(&os08e10->sd);

error_media_entity:
	media_entity_cleanup(&os08e10->sd.entity);

error_handler_free:
	os08e10_free_controls(os08e10);

error_power_off:
	pm_runtime_disable(os08e10->dev);
	pm_runtime_put_noidle(os08e10->dev);
	os08e10_power_off(os08e10->dev);

	return ret;
}

static void os08e10_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct os08e10 *os08e10 = to_os08e10(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	os08e10_free_controls(os08e10);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		os08e10_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id os08e10_dt_ids[] = {
	{ .compatible = "ovti,os08e10" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, os08e10_dt_ids);

static DEFINE_RUNTIME_DEV_PM_OPS(os08e10_pm_ops, os08e10_power_off,
				 os08e10_power_on, NULL);

static struct i2c_driver os08e10_i2c_driver = {
	.driver = {
		.name = "os08e10",
		.of_match_table	= os08e10_dt_ids,
		.pm = pm_ptr(&os08e10_pm_ops),
	},
	.probe = os08e10_probe,
	.remove = os08e10_remove,
};

module_i2c_driver(os08e10_i2c_driver);

MODULE_AUTHOR("Danius Kalvaitis <danius@kurokesu.com>");
MODULE_DESCRIPTION("OmniVision OS08E10 sensor driver");
MODULE_LICENSE("GPL");
