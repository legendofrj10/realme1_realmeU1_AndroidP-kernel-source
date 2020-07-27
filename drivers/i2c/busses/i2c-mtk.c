/*
 * Copyright (c) 2014 MediaTek Inc.
 * Author: Xudong.chen <xudong.chen@mediatek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/wait.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/scatterlist.h>
#include <linux/io.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include "mtk_secure_api.h"

#include <mtk_cpufreq_hybrid.h>
#ifdef CONFIG_MTK_GPU_SPM_DVFS_SUPPORT
#include <mtk_kbase_spm.h>
#endif
#ifdef CONFIG_MTK_TINYSYS_SCP_SUPPORT
#include <scp_helper.h>
#endif
#include "i2c-mtk.h"


static struct i2c_dma_info g_dma_regs[I2C_MAX_CHANNEL];
static struct mt_i2c *g_mt_i2c[I2C_MAX_CHANNEL];

static inline void _i2c_writew(u16 value, struct mt_i2c *i2c, u16 offset)
{
	writew(value, i2c->base + offset);
}

static inline u16 _i2c_readw(struct mt_i2c *i2c, u16 offset)
{
	return readw(i2c->base + offset);
}

#define raw_i2c_writew(val, i2c, ch_ofs, ofs) \
	do { \
		if (((i2c)->dev_comp->ver != 0x2) || (V2_##ofs != 0xfff)) \
			_i2c_writew(val, i2c, ch_ofs + \
				    (((i2c)->dev_comp->ver == 0x2) ? \
				    (V2_##ofs) : ofs)); \
	} while (0)

#define raw_i2c_readw(i2c, ch_ofs, ofs) \
	({ \
		u16 value = 0; \
		if (((i2c)->dev_comp->ver != 0x2) || (V2_##ofs != 0xfff)) \
			value = _i2c_readw(i2c, ch_ofs + \
					   (((i2c)->dev_comp->ver == 0x2) ? \
					   (V2_##ofs) : ofs)); \
		value; \
	})

#define i2c_writew(val, i2c, ofs) raw_i2c_writew(val, i2c, i2c->ch_offset, ofs)

#define i2c_readw(i2c, ofs) raw_i2c_readw(i2c, i2c->ch_offset, ofs)

#define i2c_writew_shadow(val, i2c, ofs) raw_i2c_writew(val, i2c, 0, ofs)

#define i2c_readw_shadow(i2c, ofs) raw_i2c_readw(i2c, 0, ofs)

void __iomem *cg_base;

s32 map_cg_regs(struct mt_i2c *i2c)
{
	struct device_node *cg_node;
	int ret = -1;

	if (!cg_base && i2c->dev_comp->clk_compatible[0]) {
		cg_node = of_find_compatible_node(NULL, NULL,
			i2c->dev_comp->clk_compatible);
		if (!cg_node) {
			pr_err("Cannot find cg_node\n");
			return -ENODEV;
		}
		cg_base = of_iomap(cg_node, 0);
		if (!cg_base) {
			pr_err("cg_base iomap failed\n");
			return -ENOMEM;
		}
		ret = 0;
	}

	return ret;
}

void dump_cg_regs(struct mt_i2c *i2c)
{
	u32 clk_sta_val, clk_sta_offs, cg_bit;

	if (!cg_base || i2c->id >= I2C_MAX_CHANNEL) {
		pr_err("cg_base %p, i2c id = %d\n", cg_base, i2c->id);
		return;
	}

	clk_sta_offs = i2c->dev_comp->clk_sta_offset[i2c->id];
	clk_sta_val = readl(cg_base + clk_sta_offs);
	cg_bit = i2c->dev_comp->cg_bit[i2c->id];

	pr_err("[I2C] cg regs dump:\n"
		"name %s, offset 0x%x: value = 0x%08x, bit %d, clock %s\n",
		i2c->dev_comp->clk_compatible,
		clk_sta_offs, clk_sta_val, cg_bit,
		clk_sta_val & (1 << cg_bit) ? "off":"on");
}

void __iomem *infra_base_i2c;
void __iomem *peri_base_i2c;

static s32 map_bus_regs(struct mt_i2c *i2c, u8 type)
{
	int ret = -1;

	if (type == I2C_DUMP_INFRA_DBG) {
		struct device_node *infra_node;

		if (!infra_base_i2c && i2c->dev_comp->infracfg_compatible[0]) {
			infra_node = of_find_compatible_node(NULL, NULL,
				i2c->dev_comp->infracfg_compatible);
			if (!infra_node) {
				dev_info(i2c->dev, "Cannot find infra_node\n");
				return -ENODEV;
			}
			infra_base_i2c = of_iomap(infra_node, 0);
			if (!infra_base_i2c) {
				dev_info(i2c->dev, "infra_base iomap failed\n");
				return -ENOMEM;
			}
			ret = 0;
		}
	} else if (type == I2C_DUMP_PERI_DBG) {
		struct device_node *peri_node;

		if (!peri_base_i2c && i2c->dev_comp->pericfg_compatible[0]) {
			peri_node = of_find_compatible_node(NULL, NULL,
				i2c->dev_comp->pericfg_compatible);
			if (!peri_node) {
				dev_info(i2c->dev, "Cannot find peri_node\n");
				return -ENODEV;
			}
			peri_base_i2c = of_iomap(peri_node, 0);
			if (!peri_base_i2c) {
				dev_info(i2c->dev, "peri_base iomap failed\n");
				return -ENOMEM;
			}
			ret = 0;
		}
	}

	return ret;
}

static void dump_bus_regs(struct mt_i2c *i2c, u8 type)
{
	u32 bus_mon_offs, bus_mon_lens, i;

	if (type == I2C_DUMP_INFRA_DBG) {
		if (!infra_base_i2c || i2c->id >= I2C_MAX_CHANNEL) {
			dev_info(i2c->dev, "infra_base %p, i2c id = %d\n",
				infra_base_i2c, i2c->id);
			return;
		}
		bus_mon_offs = i2c->dev_comp->infra_dbg_offset;
		bus_mon_lens = i2c->dev_comp->infra_dbg_length;
		dev_info(i2c->dev, "INFRA DBG RGs, i2c id = %d\n", i2c->id);

		for (i = bus_mon_offs;
		     i < bus_mon_offs + bus_mon_lens + 4; i += 4)
			pr_info_ratelimited("%p:%08x\n", infra_base_i2c + i,
					    readl(infra_base_i2c + i));
	} else if (type == I2C_DUMP_PERI_DBG) {
		if (!peri_base_i2c || i2c->id >= I2C_MAX_CHANNEL) {
			dev_info(i2c->dev, "peri_base %p, i2c id = %d\n",
				peri_base_i2c, i2c->id);
			return;
		}
		bus_mon_offs = i2c->dev_comp->peri_dbg_offset;
		bus_mon_lens = i2c->dev_comp->peri_dbg_length;
		dev_info(i2c->dev, "PERI DBG RGs, i2c id = %d\n", i2c->id);

		for (i = bus_mon_offs;
		     i < bus_mon_offs + bus_mon_lens + 4; i += 4)
			pr_info_ratelimited("%p:%08x\n", peri_base_i2c + i,
					    readl(peri_base_i2c + i));
	}
}

void __iomem *dma_base;

s32 map_dma_regs(void)
{
	struct device_node *dma_node;

	dma_node = of_find_compatible_node(NULL, NULL, "mediatek,ap_dma");
	if (!dma_node) {
		pr_err("Cannot find dma_node\n");
		return -ENODEV;
	}
	dma_base = of_iomap(dma_node, 0);
	if (!dma_base) {
		pr_err("dma_base iomap failed\n");
		return -ENOMEM;
	}
	return 0;
}

void dump_dma_regs(void)
{
	int status;
	int i;

	if (!dma_base) {
		pr_err("dma_base NULL\n");
		return;
	}

	status =  readl(dma_base + 8);
	pr_err("DMA RUNNING STATUS : 0x%x .\n", status);
	for	(i = 0; i < 21 ; i++) {
		if (status & (0x1 << i))
			pr_err("DMA[%d] CONTROL REG : 0x%x, DEBUG : 0x%x .\n", i,
				readl(dma_base + 0x80 + 0x80 * i + 0x18),
				readl(dma_base + 0x80 + 0x80 * i + 0x50));
	}

}

static inline void i2c_writel_dma(u32 value, struct mt_i2c *i2c, u8 offset)
{
	writel(value, i2c->pdmabase + i2c->dma_ch_offset + offset);
}

static inline u32 i2c_readl_dma(struct mt_i2c *i2c, u8 offset)
{
	return readl(i2c->pdmabase + i2c->dma_ch_offset + offset);
}

static void record_i2c_dma_info(struct mt_i2c *i2c)
{
	g_dma_regs[i2c->id].base = (unsigned long)i2c->pdmabase;
	g_dma_regs[i2c->id].int_flag = i2c_readl_dma(i2c, OFFSET_INT_FLAG);
	g_dma_regs[i2c->id].int_en = i2c_readl_dma(i2c, OFFSET_INT_EN);
	g_dma_regs[i2c->id].en = i2c_readl_dma(i2c, OFFSET_EN);
	g_dma_regs[i2c->id].rst = i2c_readl_dma(i2c, OFFSET_RST);
	g_dma_regs[i2c->id].stop = i2c_readl_dma(i2c, OFFSET_STOP);
	g_dma_regs[i2c->id].flush = i2c_readl_dma(i2c, OFFSET_FLUSH);
	g_dma_regs[i2c->id].con = i2c_readl_dma(i2c, OFFSET_CON);
	g_dma_regs[i2c->id].tx_mem_addr = i2c_readl_dma(i2c, OFFSET_TX_MEM_ADDR);
	g_dma_regs[i2c->id].rx_mem_addr = i2c_readl_dma(i2c, OFFSET_RX_MEM_ADDR);
	g_dma_regs[i2c->id].tx_len = i2c_readl_dma(i2c, OFFSET_TX_LEN);
	g_dma_regs[i2c->id].rx_len = i2c_readl_dma(i2c, OFFSET_RX_LEN);
	g_dma_regs[i2c->id].int_buf_size = i2c_readl_dma(i2c, OFFSET_INT_BUF_SIZE);
	g_dma_regs[i2c->id].debug_sta = i2c_readl_dma(i2c, OFFSET_DEBUG_STA);
	g_dma_regs[i2c->id].tx_mem_addr2 = i2c_readl_dma(i2c, OFFSET_TX_MEM_ADDR2);
	g_dma_regs[i2c->id].rx_mem_addr2 = i2c_readl_dma(i2c, OFFSET_RX_MEM_ADDR2);
}

static void record_i2c_info(struct mt_i2c *i2c, int tmo)
{
	int idx = i2c->rec_idx;

	i2c->rec_info[idx].slave_addr = i2c_readw(i2c, OFFSET_SLAVE_ADDR);
	i2c->rec_info[idx].intr_stat = i2c->irq_stat;
	i2c->rec_info[idx].control = i2c_readw(i2c, OFFSET_CONTROL);
	i2c->rec_info[idx].fifo_stat = i2c_readw(i2c, OFFSET_FIFO_STAT);
	i2c->rec_info[idx].debug_stat = i2c_readw(i2c, OFFSET_DEBUGSTAT);
	i2c->rec_info[idx].tmo = tmo;
	i2c->rec_info[idx].end_time = sched_clock();

	i2c->rec_idx++;
	if (i2c->rec_idx == I2C_RECORD_LEN)
		i2c->rec_idx = 0;
}

static void dump_i2c_info(struct mt_i2c *i2c)
{
	int i;
	int idx = i2c->rec_idx;
	long long endtime;
	unsigned long ns;

	if (i2c->buffermode) /* no i2c history @ buffermode */
		return;

	dev_err(i2c->dev, "last transfer info:\n");

	for (i = 0; i < I2C_RECORD_LEN; i++) {
		if (idx == 0)
			idx = I2C_RECORD_LEN;
		idx--;
		endtime = i2c->rec_info[idx].end_time;
		ns = do_div(endtime, 1000000000);
		dev_err(i2c->dev,
			"[%02d] [%5lu.%06lu] SLAVE_ADDR=%x,INTR_STAT=%x,CONTROL=%x,FIFO_STAT=%x,DEBUGSTAT=%x, tmo=%d\n",
			i,
			(unsigned long)endtime,
			ns/1000,
			i2c->rec_info[idx].slave_addr,
			i2c->rec_info[idx].intr_stat,
			i2c->rec_info[idx].control,
			i2c->rec_info[idx].fifo_stat,
			i2c->rec_info[idx].debug_stat,
			i2c->rec_info[idx].tmo
		);
	}
}

static int mt_i2c_clock_enable(struct mt_i2c *i2c)
{
#if !defined(CONFIG_MT_I2C_FPGA_ENABLE)
	int ret = 0;

	ret = clk_prepare_enable(i2c->clk_dma);
	if (ret)
		return ret;

	if (i2c->clk_arb != NULL) {
		ret = clk_prepare_enable(i2c->clk_arb);
		if (ret)
			return ret;
	}
	ret = clk_prepare_enable(i2c->clk_main);
	if (ret)
		goto err_main;

	if (i2c->have_pmic) {
		ret = clk_prepare_enable(i2c->clk_pmic);
		if (ret)
			goto err_pmic;
	}
	spin_lock(&i2c->cg_lock);
	if (i2c->suspended)
		ret = -EIO;
	else
		i2c->cg_cnt++;
	spin_unlock(&i2c->cg_lock);
	if (ret) {
		dev_info(i2c->dev, "err, access at suspend no irq stage\n");
		goto err_cg;
	}

	return 0;

err_cg:
	if (i2c->have_pmic)
		clk_disable_unprepare(i2c->clk_pmic);
err_pmic:
	clk_disable_unprepare(i2c->clk_main);
err_main:
	if (i2c->clk_arb)
		clk_disable_unprepare(i2c->clk_arb);
	clk_disable_unprepare(i2c->clk_dma);
	return ret;
#else
	return 0;
#endif
}

static void mt_i2c_clock_disable(struct mt_i2c *i2c)
{
#if !defined(CONFIG_MT_I2C_FPGA_ENABLE)
	if (i2c->have_pmic)
		clk_disable_unprepare(i2c->clk_pmic);

	clk_disable_unprepare(i2c->clk_main);
	if (i2c->clk_arb != NULL)
		clk_disable_unprepare(i2c->clk_arb);

	clk_disable_unprepare(i2c->clk_dma);
	spin_lock(&i2c->cg_lock);
	i2c->cg_cnt--;
	spin_unlock(&i2c->cg_lock);
#endif
}
#ifdef CONFIG_MTK_TINYSYS_SCP_SUPPORT

static int i2c_get_semaphore(struct mt_i2c *i2c)
{
#ifdef CONFIG_MTK_TINYSYS_SCP_SUPPORT
	int count = 100;
#endif

	if (i2c->appm) {
		if (cpuhvfs_get_dvfsp_semaphore(SEMA_I2C_DRV) != 0) {
			dev_err(i2c->dev, "sema time out 2ms\n");
			if (cpuhvfs_get_dvfsp_semaphore(SEMA_I2C_DRV) != 0) {
				dev_err(i2c->dev, "sema time out 4ms\n");
				i2c_dump_info(i2c);
				WARN_ON(1);
				return -EBUSY;
			}
		}
	}

#ifdef CONFIG_MTK_GPU_SPM_DVFS_SUPPORT
	if (i2c->gpupm) {
		if (dvfs_gpu_pm_spin_lock_for_vgpu() != 0) {
			dev_err(i2c->dev, "sema time out.\n");
			return -EBUSY;
		}
	}
#endif
	if (i2c->skip_scp_sema)
		return 0;

	switch (i2c->id) {
#ifdef CONFIG_MTK_TINYSYS_SCP_SUPPORT
	case 0:
		while ((get_scp_semaphore(SEMAPHORE_I2C0) != 1) && count > 0)
			count--;
		return count > 0 ? 0 : -EBUSY;
	case 1:
		while ((get_scp_semaphore(SEMAPHORE_I2C1) != 1) && count > 0)
			count--;
		return count > 0 ? 0 : -EBUSY;
#endif
	default:
		return 0;
	}
}

static int i2c_release_semaphore(struct mt_i2c *i2c)
{
	if (i2c->appm)
		cpuhvfs_release_dvfsp_semaphore(SEMA_I2C_DRV);
#ifdef CONFIG_MTK_GPU_SPM_DVFS_SUPPORT
	if (i2c->gpupm)
		dvfs_gpu_pm_spin_unlock_for_vgpu();
#endif
	if (i2c->skip_scp_sema)
		return 0;

	switch (i2c->id) {
#ifdef CONFIG_MTK_TINYSYS_SCP_SUPPORT
	case 0:
		return release_scp_semaphore(SEMAPHORE_I2C0) == 1 ? 0 : -EBUSY;
	case 1:
		return release_scp_semaphore(SEMAPHORE_I2C1) == 1 ? 0 : -EBUSY;
#endif
	default:
		return 0;
	}
}
#endif
static void free_i2c_dma_bufs(struct mt_i2c *i2c)
{
	dma_free_coherent(i2c->adap.dev.parent, PAGE_SIZE,
		i2c->dma_buf.vaddr, i2c->dma_buf.paddr);
}

static inline void mt_i2c_wait_done(struct mt_i2c *i2c, u16 ch_off)
{
	u16 start, tmo;

	start = raw_i2c_readw(i2c, ch_off, OFFSET_START) & I2C_TRANSAC_START;
	if (start) {
		dev_err(i2c->dev, "wait transfer done before cg off.\n");

		tmo = 100;
		do {
			msleep(20);
			start = raw_i2c_readw(i2c, ch_off, OFFSET_START) &
				I2C_TRANSAC_START;
			tmo--;
		} while (start && tmo);

		if (start && !tmo) {
			dev_err(i2c->dev, "wait transfer timeout.\n");
			i2c_dump_info(i2c);
		}
	}
}

static inline void mt_i2c_wait_dma_en_done(struct mt_i2c *i2c)
{
	u16 start, tmo;

	start = i2c_readl_dma(i2c, OFFSET_EN);
	if (start) {
		dev_info(i2c->dev, "wait AP DMA done.\n");

		tmo = 100;
		do {
			msleep(20);
			start = i2c_readl_dma(i2c, OFFSET_EN);
			tmo--;
		} while (start && tmo);

		if (start && !tmo) {
			dev_info(i2c->dev, "complete with DMA still active.\n");
			i2c_dump_info(i2c);
		}
	}
}

static inline void mt_i2c_init_hw(struct mt_i2c *i2c)
{
	/* clear interrupt status */
	i2c_writew_shadow(0, i2c, OFFSET_INTR_MASK);
	i2c->irq_stat = i2c_readw_shadow(i2c, OFFSET_INTR_STAT);
	i2c_writew_shadow(i2c->irq_stat, i2c, OFFSET_INTR_STAT);

	i2c_writew_shadow(I2C_SOFT_RST, i2c, OFFSET_SOFTRESET);
	/* Set ioconfig */
	if (i2c->use_push_pull)
		i2c_writew_shadow(I2C_IO_CONFIG_PUSH_PULL, i2c, OFFSET_IO_CONFIG);
	else
		i2c_writew_shadow(I2C_IO_CONFIG_OPEN_DRAIN, i2c, OFFSET_IO_CONFIG);
	if (i2c->have_dcm)
		i2c_writew_shadow(I2C_DCM_DISABLE, i2c, OFFSET_DCM_EN);

	if (i2c->dev_comp->ver != 0x2)
		i2c_writew_shadow(i2c->timing_reg, i2c, OFFSET_TIMING);
	else
		i2c_writew_shadow(i2c->timing_reg | I2C_TIMEOUT_EN, i2c,
			   OFFSET_TIMING);

	if (i2c->dev_comp->set_ltiming)
		i2c_writew_shadow(i2c->ltiming_reg, i2c, OFFSET_LTIMING);
	i2c_writew_shadow(i2c->high_speed_reg, i2c, OFFSET_HS);
	/* DMA warm reset, and waits for EN to become 0 */
	i2c_writel_dma(I2C_DMA_WARM_RST, i2c, OFFSET_RST);
	udelay(5);
	if (i2c_readl_dma(i2c, OFFSET_EN) != 0) {
		dev_err(i2c->dev, "DMA bus hang .\n");
		dump_dma_regs();
		WARN_ON(1);
	}
}

/* calculate i2c port speed */
static int mtk_i2c_calculate_speed(struct mt_i2c *i2c,
	unsigned int clk_src_in_hz,
	unsigned int speed_hz,
	unsigned int *timing_step_cnt,
	unsigned int *timing_sample_cnt)
{
	unsigned int khz;
	unsigned int step_cnt;
	unsigned int sample_cnt;
	unsigned int sclk;
	unsigned int hclk;
	unsigned int max_step_cnt;
	unsigned int sample_div = MAX_SAMPLE_CNT_DIV;
	unsigned int step_div;
	unsigned int min_div;
	unsigned int best_mul;
	unsigned int cnt_mul;

	if (speed_hz > MAX_HS_MODE_SPEED) {
		if (i2c->dev_comp->check_max_freq)
			return -EINVAL;
		max_step_cnt = MAX_HS_STEP_CNT_DIV;
	} else if (speed_hz > MAX_FS_MODE_SPEED) {
		max_step_cnt = MAX_HS_STEP_CNT_DIV;
	} else {
		max_step_cnt = MAX_STEP_CNT_DIV;
	}
	step_div = max_step_cnt;

	/* Find the best combination */
	khz = speed_hz / 1000;
	hclk = clk_src_in_hz / 1000;
	min_div = ((hclk >> 1) + khz - 1) / khz;
	best_mul = MAX_SAMPLE_CNT_DIV * max_step_cnt;
	for (sample_cnt = 1; sample_cnt <= MAX_SAMPLE_CNT_DIV; sample_cnt++) {
		step_cnt = (min_div + sample_cnt - 1) / sample_cnt;
		cnt_mul = step_cnt * sample_cnt;
		if (step_cnt > max_step_cnt)
			continue;
		if (cnt_mul < best_mul) {
			best_mul = cnt_mul;
			sample_div = sample_cnt;
			step_div = step_cnt;
			if (best_mul == min_div)
				break;
		}
	}
	sample_cnt = sample_div;
	step_cnt = step_div;
	sclk = hclk / (2 * sample_cnt * step_cnt);
	if (sclk > khz) {
		dev_dbg(i2c->dev, "%s mode: unsupported speed (%ldkhz)\n",
			(speed_hz > MAX_FS_MODE_SPEED) ? "HS" : "ST/FT", (long int)khz);
		return -ENOTSUPP;
	}

	step_cnt--;
	sample_cnt--;

	*timing_step_cnt = step_cnt;
	*timing_sample_cnt = sample_cnt;

	return 0;
}

static int i2c_set_speed(struct mt_i2c *i2c, unsigned int clk_src_in_hz)
{
	int ret;
	unsigned int step_cnt;
	unsigned int sample_cnt;
	unsigned int l_step_cnt;
	unsigned int l_sample_cnt;
	unsigned int speed_hz;
	unsigned int duty = HALF_DUTY_CYCLE;

	if (i2c->ext_data.isEnable && i2c->ext_data.timing)
		speed_hz = i2c->ext_data.timing;
	else
		speed_hz = i2c->speed_hz;

	if (speed_hz > MAX_FS_MODE_SPEED && !i2c->hs_only) {
		/* Set the hign speed mode register */
		ret = mtk_i2c_calculate_speed(i2c, clk_src_in_hz,
			MAX_FS_MODE_SPEED, &l_step_cnt, &l_sample_cnt);
		if (ret < 0)
			return ret;

		ret = mtk_i2c_calculate_speed(i2c, clk_src_in_hz,
			speed_hz, &step_cnt, &sample_cnt);
		if (ret < 0)
			return ret;

		i2c->high_speed_reg = I2C_TIME_DEFAULT_VALUE |
			(sample_cnt & I2C_TIMING_SAMPLE_COUNT_MASK) << 12 |
			(step_cnt & I2C_TIMING_SAMPLE_COUNT_MASK) << 8;

		i2c->timing_reg =
			(l_sample_cnt & I2C_TIMING_SAMPLE_COUNT_MASK) << 8 |
			(l_step_cnt & I2C_TIMING_STEP_DIV_MASK) << 0;

		if (i2c->dev_comp->set_ltiming) {
			i2c->ltiming_reg = (l_sample_cnt << 6) | (l_step_cnt << 0) |
				(sample_cnt & I2C_TIMING_SAMPLE_COUNT_MASK) << 12 |
				(step_cnt & I2C_TIMING_SAMPLE_COUNT_MASK) << 9;
		}
	} else {
		if (speed_hz > I2C_DEFAUT_SPEED
			&& speed_hz <= MAX_FS_MODE_SPEED
			&& i2c->dev_comp->set_ltiming)
			duty = DUTY_CYCLE;

		ret = mtk_i2c_calculate_speed(i2c, clk_src_in_hz,
			(speed_hz * 50 / duty), &step_cnt, &sample_cnt);
		if (ret < 0)
			return ret;

		i2c->timing_reg =
			(sample_cnt & I2C_TIMING_SAMPLE_COUNT_MASK) << 8 |
			(step_cnt & I2C_TIMING_STEP_DIV_MASK) << 0;

		if (i2c->dev_comp->set_ltiming) {
			ret = mtk_i2c_calculate_speed(i2c, clk_src_in_hz,
				(speed_hz * 50 / (100 - duty)), &l_step_cnt, &l_sample_cnt);
			if (ret < 0)
				return ret;

			i2c->ltiming_reg =
				(l_sample_cnt & I2C_TIMING_SAMPLE_COUNT_MASK) << 6 |
				(l_step_cnt & I2C_TIMING_STEP_DIV_MASK) << 0;
		}
		/* Disable the high speed transaction */
		i2c->high_speed_reg = I2C_TIME_CLR_VALUE;
	}

	return 0;
}

#ifdef I2C_DEBUG_FS

void i2c_dump_info1(struct mt_i2c *i2c)
{
	if (i2c->ext_data.isEnable && i2c->ext_data.timing)
		dev_err(i2c->dev, "I2C structure:\nspeed %d\n",
			i2c->ext_data.timing);
	else
		dev_err(i2c->dev, "I2C structure:\nspeed %d\n",
			i2c->speed_hz);
	dev_err(i2c->dev, "I2C structure:\nOp %x\n", i2c->op);
	dev_err(i2c->dev,
		"I2C structure:\nData_size %x\nIrq_stat %x\nTrans_stop %d\n",
		i2c->msg_len, i2c->irq_stat, i2c->trans_stop);
	dev_err(i2c->dev, "base address %p\n", i2c->base);
	dev_err(i2c->dev,
		"I2C register:\nSLAVE_ADDR %x\nINTR_MASK %x\n",
		(i2c_readw(i2c, OFFSET_SLAVE_ADDR)),
		(i2c_readw(i2c, OFFSET_INTR_MASK)));
	dev_err(i2c->dev,
		"I2C register:\nINTR_STAT %x\nCONTROL %x\n",
		(i2c_readw(i2c, OFFSET_INTR_STAT)),
		(i2c_readw(i2c, OFFSET_CONTROL)));
	dev_err(i2c->dev,
		"I2C register:\nTRANSFER_LEN %x\nTRANSAC_LEN %x\n",
		(i2c_readw(i2c, OFFSET_TRANSFER_LEN)),
		(i2c_readw(i2c, OFFSET_TRANSAC_LEN)));
	dev_err(i2c->dev,
		"I2C register:\nDELAY_LEN %x\nTIMING %x\n",
		(i2c_readw(i2c, OFFSET_DELAY_LEN)),
		(i2c_readw(i2c, OFFSET_TIMING)));
	dev_err(i2c->dev,
		"I2C register:\nSTART %x\nFIFO_STAT %x\n",
		(i2c_readw(i2c, OFFSET_START)),
		(i2c_readw(i2c, OFFSET_FIFO_STAT)));
	dev_err(i2c->dev,
		"I2C register:\nIO_CONFIG %x\nHS %x\n",
		(i2c_readw(i2c, OFFSET_IO_CONFIG)),
		(i2c_readw(i2c, OFFSET_HS)));
	dev_err(i2c->dev,
		"I2C register:\nDEBUGSTAT %x\nEXT_CONF %x\nPATH_DIR %x\n",
		(i2c_readw(i2c, OFFSET_DEBUGSTAT)),
		(i2c_readw(i2c, OFFSET_EXT_CONF)),
		(i2c_readw(i2c, OFFSET_PATH_DIR)));
}

void i2c_dump_info(struct mt_i2c *i2c)
{
	/* I2CFUC(); */
	/* int val=0; */
	pr_info_ratelimited("i2c_dump_info ++++++++++++++++++++++++++++++++++++++++++\n");
	pr_err("I2C structure:\n"
	       I2CTAG "Clk=%ld,Id=%d,Op=%x,Irq_stat=%x,Total_len=%x\n"
	       I2CTAG "Trans_len=%x,Trans_num=%x,Trans_auxlen=%x,speed=%d\n"
	       I2CTAG "Trans_stop=%u,cg_cnt=%d,hs_only=%d, ch_offset=%d,ch_offset_default=%d\n",
	       i2c->main_clk, i2c->id, i2c->op, i2c->irq_stat, i2c->total_len,
			i2c->msg_len, 1, i2c->msg_aux_len, i2c->speed_hz, i2c->trans_stop, i2c->cg_cnt,
			i2c->hs_only, i2c->ch_offset, i2c->ch_offset_default);

	pr_info_ratelimited("base address 0x%p\n", i2c->base);
	pr_info_ratelimited("I2C register:\n"
	       I2CTAG "SLAVE_ADDR=%x,INTR_MASK=%x,INTR_STAT=%x,CONTROL=%x,TRANSFER_LEN=%x\n"
	       I2CTAG "TRANSAC_LEN=%x,DELAY_LEN=%x,TIMING=%x,LTIMING=%x,START=%x,FIFO_STAT=%x\n"
	       I2CTAG "IO_CONFIG=%x,HS=%x,DCM_EN=%x,DEBUGSTAT=%x,EXT_CONF=%x,TRANSFER_LEN_AUX=%x\n",
	       (i2c_readw(i2c, OFFSET_SLAVE_ADDR)),
	       (i2c_readw(i2c, OFFSET_INTR_MASK)),
	       (i2c_readw(i2c, OFFSET_INTR_STAT)),
	       (i2c_readw(i2c, OFFSET_CONTROL)),
	       (i2c_readw(i2c, OFFSET_TRANSFER_LEN)),
	       (i2c_readw(i2c, OFFSET_TRANSAC_LEN)),
	       (i2c_readw(i2c, OFFSET_DELAY_LEN)),
	       (i2c_readw(i2c, OFFSET_TIMING)),
	       (i2c_readw(i2c, OFFSET_LTIMING)),
	       (i2c_readw(i2c, OFFSET_START)),
	       (i2c_readw(i2c, OFFSET_FIFO_STAT)),
	       (i2c_readw(i2c, OFFSET_IO_CONFIG)),
	       (i2c_readw(i2c, OFFSET_HS)),
	       (i2c_readw(i2c, OFFSET_DCM_EN)),
	       (i2c_readw(i2c, OFFSET_DEBUGSTAT)),
	       (i2c_readw(i2c, OFFSET_EXT_CONF)), (i2c_readw(i2c, OFFSET_TRANSFER_LEN_AUX)));

	pr_info_ratelimited("before enable DMA register(0x%lx):\n"
	       I2CTAG "INT_FLAG=%x,INT_EN=%x,EN=%x,RST=%x,\n"
	       I2CTAG "STOP=%x,FLUSH=%x,CON=%x,TX_MEM_ADDR=%x, RX_MEM_ADDR=%x\n"
	       I2CTAG "TX_LEN=%x,RX_LEN=%x,INT_BUF_SIZE=%x,DEBUG_STATUS=%x\n"
	       I2CTAG "TX_MEM_ADDR2=%x, RX_MEM_ADDR2=%x\n",
	       g_dma_regs[i2c->id].base,
	       g_dma_regs[i2c->id].int_flag,
	       g_dma_regs[i2c->id].int_en,
	       g_dma_regs[i2c->id].en,
	       g_dma_regs[i2c->id].rst,
	       g_dma_regs[i2c->id].stop,
	       g_dma_regs[i2c->id].flush,
	       g_dma_regs[i2c->id].con,
	       g_dma_regs[i2c->id].tx_mem_addr,
	       g_dma_regs[i2c->id].tx_mem_addr,
	       g_dma_regs[i2c->id].tx_len,
	       g_dma_regs[i2c->id].rx_len,
	       g_dma_regs[i2c->id].int_buf_size, g_dma_regs[i2c->id].debug_sta,
	       g_dma_regs[i2c->id].tx_mem_addr2,
	       g_dma_regs[i2c->id].tx_mem_addr2);
	pr_info_ratelimited("DMA register(0x%p):\n"
	       I2CTAG "INT_FLAG=%x,INT_EN=%x,EN=%x,RST=%x,\n"
	       I2CTAG "STOP=%x,FLUSH=%x,CON=%x,TX_MEM_ADDR=%x, RX_MEM_ADDR=%x\n"
	       I2CTAG "TX_LEN=%x,RX_LEN=%x,INT_BUF_SIZE=%x,DEBUG_STATUS=%x\n"
	       I2CTAG "TX_MEM_ADDR2=%x, RX_MEM_ADDR2=%x\n",
	       i2c->pdmabase,
	       (i2c_readl_dma(i2c, OFFSET_INT_FLAG)),
	       (i2c_readl_dma(i2c, OFFSET_INT_EN)),
	       (i2c_readl_dma(i2c, OFFSET_EN)),
	       (i2c_readl_dma(i2c, OFFSET_RST)),
	       (i2c_readl_dma(i2c, OFFSET_STOP)),
	       (i2c_readl_dma(i2c, OFFSET_FLUSH)),
	       (i2c_readl_dma(i2c, OFFSET_CON)),
	       (i2c_readl_dma(i2c, OFFSET_TX_MEM_ADDR)),
	       (i2c_readl_dma(i2c, OFFSET_RX_MEM_ADDR)),
	       (i2c_readl_dma(i2c, OFFSET_TX_LEN)),
	       (i2c_readl_dma(i2c, OFFSET_RX_LEN)),
	       (i2c_readl_dma(i2c, OFFSET_INT_BUF_SIZE)),
	       (i2c_readl_dma(i2c, OFFSET_DEBUG_STA)),
	       (i2c_readl_dma(i2c, OFFSET_TX_MEM_ADDR2)),
	       (i2c_readl_dma(i2c, OFFSET_RX_MEM_ADDR2)));
	pr_info_ratelimited("i2c_dump_info ------------------------------------------\n");

	dump_i2c_info(i2c);

	if (i2c->ccu_offset) {
		dev_info(i2c->dev, "I2C CCU register:\n"
		I2CTAG "SLAVE_ADDR=%x,INTR_MASK=%x,INTR_STAT=%x,CONTROL=%x,"
		I2CTAG "TRANSFER_LEN=%x, TRANSAC_LEN=%x,DELAY_LEN=%x\n"
		I2CTAG "TIMING=%x,LTIMING=%x,START=%x,FIFO_STAT=%x,"
		I2CTAG "IO_CONFIG=%x,HS=%x,DCM_EN=%x,DEBUGSTAT=%x,EXT_CONF=%x,"
		I2CTAG "TRANSFER_LEN_AUX=%x\n",
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_SLAVE_ADDR)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_INTR_MASK)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_INTR_STAT)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_CONTROL)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_TRANSFER_LEN)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_TRANSAC_LEN)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_DELAY_LEN)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_TIMING)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_LTIMING)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_START)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_FIFO_STAT)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_IO_CONFIG)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_HS)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_DCM_EN)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_DEBUGSTAT)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_EXT_CONF)),
		(raw_i2c_readw(i2c, i2c->ccu_offset, OFFSET_TRANSFER_LEN_AUX)));
	}
}
#else
void i2c_dump_info(struct mt_i2c *i2c)
{
}
#endif

void dump_i2c_status(int id)
{
	if (id >= I2C_MAX_CHANNEL) {
		pr_err("error %s, id = %d\n", __func__, id);
		return;
	}

	if (!g_mt_i2c[id]) {
		pr_err("error %s, g_mt_i2c[%d] == NULL\n", __func__, id);
		return;
	}

	dump_cg_regs(g_mt_i2c[id]);
	(void)mt_i2c_clock_enable(g_mt_i2c[id]);
	i2c_dump_info(g_mt_i2c[id]);
	mt_i2c_clock_disable(g_mt_i2c[id]);
}
EXPORT_SYMBOL(dump_i2c_status);

static int mt_i2c_do_transfer(struct mt_i2c *i2c)
{
	u16 addr_reg;
	u16 control_reg;
	u16 ioconfig_reg;
	u16 int_reg;
	u16 start_reg;
	int tmo = i2c->adap.timeout;
	unsigned int speed_hz;
	bool isDMA = false;
	int data_size;
	u8 *ptr;
	int ret;

	i2c->trans_stop = false;
	i2c->irq_stat = 0;
	if (i2c->total_len > 8 || i2c->msg_aux_len > 8) {
		isDMA = true;
		if (i2c_readl_dma(i2c, OFFSET_EN) != 0) {
			dev_info(i2c->dev, "DMA hard reset before start\n");
			i2c_writel_dma(I2C_DMA_HARD_RST, i2c, OFFSET_RST);
			udelay(50);
			i2c_writel_dma(I2C_DMA_CLR_FLAG, i2c, OFFSET_RST);
		}
	}
	if (i2c->ext_data.isEnable && i2c->ext_data.timing)
		speed_hz = i2c->ext_data.timing;
	else
		speed_hz = i2c->speed_hz;
	if (i2c->ext_data.is_ch_offset) {
		i2c->ch_offset = i2c->ext_data.ch_offset;
		i2c->dma_ch_offset = i2c->ext_data.dma_ch_offset;

		if (i2c->ext_data.ch_offset == 0) {
			dev_info(i2c->dev, "Wrong channel offset for multi-channel\n");
			i2c->ch_offset = i2c->ccu_offset;
		}
	} else {
		i2c->ch_offset = i2c->ch_offset_default;
		i2c->dma_ch_offset = i2c->dma_ch_offset_default;
	}

#if !defined(CONFIG_MT_I2C_FPGA_ENABLE)
	ret = i2c_set_speed(i2c, clk_get_rate(i2c->clk_main) / i2c->clk_src_div);
#else
	ret = i2c_set_speed(i2c, I2C_CLK_RATE);
#endif
	if (ret) {
		dev_err(i2c->dev, "Failed to set the speed\n");
		return -EINVAL;
	}

	if ((i2c->dev_comp->ver == 0x2) && i2c->ltiming_reg) {
		u32 tv1, tv2, tv;
#if !defined(CONFIG_MT_I2C_FPGA_ENABLE)
		tv1 = (clk_get_rate(i2c->clk_main) / i2c->clk_src_div) / 1000;
#else
		tv1 = I2C_CLK_RATE / 1000;
#endif
		tv1 = tv1 * MAX_SCL_LOW_TIME;
		tv2 = (((i2c->ltiming_reg & LSTEP_MSK) + 1) *
		      (((i2c->ltiming_reg & LSAMPLE_MSK) >> 6) + 1));
		tv = DIV_ROUND_UP(tv1, tv2);
		i2c_writew(tv & 0xFFFF, i2c, OFFSET_HW_TIMEOUT);
		/* dev_info(i2c->dev, "scl time out value %04X\n", */
		/*	    (u16)(tv & 0xFFFF));		   */
	}

	if (i2c->dev_comp->set_dt_div) {
		if (i2c->clk_src_div > MAX_CLOCK_DIV) {
			dev_err(i2c->dev, "Clock div error\n");
			return -EINVAL;
		}
		i2c_writew(((i2c->clk_src_div - 1) << 8) + (i2c->clk_src_div - 1),
			i2c, OFFSET_CLOCK_DIV);
	}

	/* If use i2c pin from PMIC mt6397 side, need set PATH_DIR first */
	if (i2c->have_pmic)
		i2c_writew(I2C_CONTROL_WRAPPER, i2c, OFFSET_PATH_DIR);
	control_reg = I2C_CONTROL_ACKERR_DET_EN | I2C_CONTROL_CLK_EXT_EN;
	if (isDMA == true) /* DMA */
		control_reg |= I2C_CONTROL_DMA_EN | I2C_CONTROL_DMAACK_EN | I2C_CONTROL_ASYNC_MODE;

	if (speed_hz > 400000)
		control_reg |= I2C_CONTROL_RS;
	if (i2c->op == I2C_MASTER_WRRD)
		control_reg |= I2C_CONTROL_DIR_CHANGE | I2C_CONTROL_RS;
	i2c_writew(control_reg, i2c, OFFSET_CONTROL);

	/* set start condition */
	if (speed_hz <= 100000)
		i2c_writew(I2C_ST_START_CON, i2c, OFFSET_EXT_CONF);
	else {
		if (i2c->dev_comp->ext_time_config != 0)
			i2c_writew(i2c->dev_comp->ext_time_config, i2c, OFFSET_EXT_CONF);
		else
			i2c_writew(I2C_FS_START_CON, i2c, OFFSET_EXT_CONF);
	}
	if (~control_reg & I2C_CONTROL_RS)
		i2c_writew(I2C_DELAY_LEN, i2c, OFFSET_DELAY_LEN);

	/* Set ioconfig */
	if (i2c->use_push_pull) {
		ioconfig_reg = I2C_IO_CONFIG_PUSH_PULL;
	} else {
		ioconfig_reg = I2C_IO_CONFIG_OPEN_DRAIN;
		if (i2c->dev_comp->set_aed)
			ioconfig_reg |= ((i2c->aed<<4) & I2C_IO_CONFIG_AED_MASK);
	}
	i2c_writew(ioconfig_reg, i2c, OFFSET_IO_CONFIG);

	if (i2c->dev_comp->ver != 0x2)
		i2c_writew(i2c->timing_reg, i2c, OFFSET_TIMING);
	else
		i2c_writew(i2c->timing_reg | I2C_TIMEOUT_EN, i2c, OFFSET_TIMING);

	if (i2c->dev_comp->set_ltiming)
		i2c_writew(i2c->ltiming_reg, i2c, OFFSET_LTIMING);
	i2c_writew(i2c->high_speed_reg, i2c, OFFSET_HS);

	if (i2c->have_dcm)
		i2c_writew(I2C_DCM_ENABLE, i2c, OFFSET_DCM_EN);

	addr_reg = i2c->addr << 1;
	if (i2c->op == I2C_MASTER_RD)
		addr_reg |= 0x1;
	i2c_writew(addr_reg, i2c, OFFSET_SLAVE_ADDR);

	int_reg = I2C_HS_NACKERR | I2C_ACKERR |
		  I2C_TRANSAC_COMP | I2C_ARB_LOST;
	if (i2c->dev_comp->ver == 0x2)
		int_reg |= I2C_MAS_ERR | I2C_TIMEOUT;
	if (i2c->ch_offset)
		int_reg &= ~(I2C_HS_NACKERR | I2C_ACKERR);

	/* Clear interrupt status */
	i2c_writew(I2C_INTR_ALL, i2c, OFFSET_INTR_STAT);

	if (i2c->ch_offset != 0)
		i2c_writew(I2C_FIFO_ADDR_CLR_MCH | I2C_FIFO_ADDR_CLR,
			   i2c, OFFSET_FIFO_ADDR_CLR);
	else
		i2c_writew(I2C_FIFO_ADDR_CLR, i2c, OFFSET_FIFO_ADDR_CLR);
	/* Enable interrupt */
	i2c_writew(int_reg, i2c, OFFSET_INTR_MASK);

	/* Set transfer and transaction len */
	if (i2c->op == I2C_MASTER_WRRD) {
		if ((i2c->appm) && (i2c->dev_comp->idvfs_i2c)) {
			i2c_writew((i2c->msg_len & 0xFF) | ((i2c->msg_aux_len<<8) & 0x1F00),
				i2c, OFFSET_TRANSFER_LEN);
		} else {
			i2c_writew(i2c->msg_len, i2c, OFFSET_TRANSFER_LEN);
			i2c_writew(i2c->msg_aux_len, i2c, OFFSET_TRANSFER_LEN_AUX);
		}
		i2c_writew(0x02, i2c, OFFSET_TRANSAC_LEN);
	} else if (i2c->op == I2C_MASTER_MULTI_WR) {
		i2c_writew(i2c->msg_len, i2c, OFFSET_TRANSFER_LEN);
		i2c_writew(i2c->total_len / i2c->msg_len, i2c, OFFSET_TRANSAC_LEN);
	} else {
		i2c_writew(i2c->msg_len, i2c, OFFSET_TRANSFER_LEN);
		i2c_writew(0x01, i2c, OFFSET_TRANSAC_LEN);
	}

	/* Prepare buffer data to start transfer */
	if (isDMA == true && (!i2c->is_ccu_trig)) {

#ifdef CONFIG_MTK_LM_MODE
		if ((i2c->dev_comp->dma_support == 1) && (enable_4G())) {
			i2c_writel_dma(0x1, i2c, OFFSET_TX_MEM_ADDR2);
			i2c_writel_dma(0x1, i2c, OFFSET_RX_MEM_ADDR2);
		}
#endif
		if (i2c->op == I2C_MASTER_RD) {
			i2c_writel_dma(I2C_DMA_INT_FLAG_NONE, i2c, OFFSET_INT_FLAG);
			i2c_writel_dma(I2C_DMA_CON_RX, i2c, OFFSET_CON);
			i2c_writel_dma(lower_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_RX_MEM_ADDR);
			if ((i2c->dev_comp->dma_support >= 2))
				i2c_writel_dma(upper_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_RX_MEM_ADDR2);

			i2c_writel_dma(i2c->msg_len, i2c, OFFSET_RX_LEN);
		} else if (i2c->op == I2C_MASTER_WR || i2c->op == I2C_MASTER_MULTI_WR) {
			i2c_writel_dma(I2C_DMA_INT_FLAG_NONE, i2c, OFFSET_INT_FLAG);
			i2c_writel_dma(I2C_DMA_CON_TX, i2c, OFFSET_CON);
			i2c_writel_dma(lower_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_TX_MEM_ADDR);
			if ((i2c->dev_comp->dma_support >= 2))
				i2c_writel_dma(upper_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_TX_MEM_ADDR2);

			i2c_writel_dma(i2c->total_len, i2c, OFFSET_TX_LEN);
		} else {
			i2c_writel_dma(0x0000, i2c, OFFSET_INT_FLAG);
			i2c_writel_dma(0x0000, i2c, OFFSET_CON);
			i2c_writel_dma(lower_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_TX_MEM_ADDR);
			i2c_writel_dma(lower_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_RX_MEM_ADDR);
			if ((i2c->dev_comp->dma_support >= 2)) {
				i2c_writel_dma(upper_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_TX_MEM_ADDR2);
				i2c_writel_dma(upper_32_bits(i2c->dma_buf.paddr), i2c, OFFSET_RX_MEM_ADDR2);
			}
			i2c_writel_dma(i2c->msg_len, i2c, OFFSET_TX_LEN);
			i2c_writel_dma(i2c->msg_aux_len, i2c, OFFSET_RX_LEN);
		}
		record_i2c_dma_info(i2c);
		/* flush before sending DMA start */
		mb();
		i2c_writel_dma(I2C_DMA_START_EN, i2c, OFFSET_EN);
	} else {
		if (i2c->op != I2C_MASTER_RD && (!i2c->is_ccu_trig)) {
			data_size = i2c->total_len;
			ptr = i2c->dma_buf.vaddr;
			while (data_size--) {
				i2c_writew(*ptr, i2c, OFFSET_DATA_PORT);
				ptr++;
			}
		}
	}
	if (i2c->dev_comp->ver == 0x2) {
		if (!i2c->is_ccu_trig)
			i2c_writew(I2C_MCU_INTR_EN, i2c, OFFSET_MCU_INTR);
		else {
			dev_info(i2c->dev, "I2C CCU trig.\n");
			return 0;
		}
	}
	/* flush before sending start */
	mb();
	if (!i2c->is_hw_trig)
		i2c_writew(I2C_TRANSAC_START, i2c, OFFSET_START);
	else {
		dev_err(i2c->dev, "I2C hw trig.\n");
		return 0;
	}

	tmo = wait_event_timeout(i2c->wait, i2c->trans_stop, tmo);

	record_i2c_info(i2c, tmo);

	if (tmo == 0) {
		dev_err(i2c->dev, "addr: %x, transfer timeout\n",
			i2c->addr);
		start_reg = i2c_readw(i2c, OFFSET_START);
		i2c_dump_info(i2c);
		#if defined(CONFIG_MTK_GIC_EXT)
		mt_irq_dump_status(i2c->irqnr);
		#endif
		dump_cg_regs(i2c);
		dump_bus_regs(i2c, I2C_DUMP_INFRA_DBG);
		dump_bus_regs(i2c, I2C_DUMP_PERI_DBG);

		if (i2c->ch_offset != 0)
			i2c_writew(I2C_FIFO_ADDR_CLR_MCH | I2C_FIFO_ADDR_CLR,
				   i2c, OFFSET_FIFO_ADDR_CLR);
		else
			i2c_writew(I2C_FIFO_ADDR_CLR, i2c,
				   OFFSET_FIFO_ADDR_CLR);

		/* This slave addr is used to check whether the shadow RG is */
		/* mapped normally or not */
		pr_info("SLAVE_ADDR=%x (shadow RG)",
			i2c_readw_shadow(i2c, OFFSET_SLAVE_ADDR));

		mt_i2c_init_hw(i2c);
		if (i2c->ch_offset)
			i2c_writew_shadow(I2C_RESUME_ARBIT, i2c, OFFSET_START);

		if (start_reg & I2C_TRANSAC_START) {
			dev_err(i2c->dev, "bus tied low/high\n");
			return -EIO;
		}
		return -ETIMEDOUT;
	}
	if (i2c->irq_stat & (I2C_HS_NACKERR | I2C_ACKERR |
	    I2C_TIMEOUT | I2C_MAS_ERR)) {
		static u16 tmp_addr;

		if (i2c->irq_stat & I2C_TIMEOUT)
			dev_info(i2c->dev, "addr: %x, scl timeout error\n",
				i2c->addr);
		else {
			if (tmp_addr != i2c->addr) {
				dev_info(i2c->dev,
					  "addr: %x, transfer ACK error\n",
					  i2c->addr);
				tmp_addr = i2c->addr;
			} else
				pr_info_ratelimited("addr: %x, transfer ACK error\n",
						      i2c->addr);
		}

		if (i2c->ch_offset != 0)
			i2c_writew(I2C_FIFO_ADDR_CLR_MCH | I2C_FIFO_ADDR_CLR,
				   i2c, OFFSET_FIFO_ADDR_CLR);
		else
			i2c_writew(I2C_FIFO_ADDR_CLR, i2c,
				   OFFSET_FIFO_ADDR_CLR);

		if (i2c->ext_data.isEnable ==  false || i2c->ext_data.isFilterMsg == false)
			i2c_dump_info(i2c);

		if ((i2c->irq_stat & I2C_TRANSAC_COMP) && i2c->ch_offset &&
		    (!(i2c->irq_stat & I2C_MAS_ERR))) {
			pr_info_ratelimited("addr: %x, trans done with err %x",
					    i2c->addr, i2c->irq_stat);
			return -EREMOTEIO;
		}

		mt_i2c_init_hw(i2c);
		if (i2c->ch_offset)
			i2c_writew_shadow(I2C_RESUME_ARBIT, i2c, OFFSET_START);
		return -EREMOTEIO;
	}
	if (i2c->op != I2C_MASTER_WR && isDMA == false) {
		if (i2c->ch_offset != 0) {
			if (i2c->op == I2C_MASTER_WRRD)
				data_size = i2c->msg_aux_len;
			else
				data_size = i2c->msg_len;
		} else
			data_size = (i2c_readw(i2c, OFFSET_FIFO_STAT) >> 4) & 0x000F;
		ptr = i2c->dma_buf.vaddr;
		while (data_size--) {
			*ptr = i2c_readw(i2c, OFFSET_DATA_PORT);
			/* I2CLOG("addr %x read byte = 0x%x\n", i2c->addr, *ptr); */
			ptr++;
		}
	}

	if (isDMA == true)
		mt_i2c_wait_dma_en_done(i2c);

	dev_dbg(i2c->dev, "i2c transfer done.\n");

	return 0;
}

static inline void mt_i2c_copy_to_dma(struct mt_i2c *i2c, struct i2c_msg *msg)
{
	/*
	 * if the operate is write, write-read, multi-write, need to copy the data
	 * to DMA memory.
	 */
	if (!(msg->flags & I2C_M_RD))
		memcpy(i2c->dma_buf.vaddr + i2c->total_len - msg->len,
			msg->buf, msg->len);
}

static inline void mt_i2c_copy_from_dma(struct mt_i2c *i2c,
	struct i2c_msg *msg)
{
	/* if the operate is read, need to copy the data from DMA memory */
	if (msg->flags & I2C_M_RD)
		memcpy(msg->buf, i2c->dma_buf.vaddr, msg->len);
}

/*
 * In MTK platform the STOP will be issued after each
 * message was transferred which is not flow the clarify
 * for i2c_transfer(), several I2C devices tolerate the STOP,
 * but some device need Repeat-Start and do not compatible with STOP
 * MTK platform has WRRD mode which can write then read with
 * Repeat-Start between two message, so we combined two
 * messages into one transaction.
 * The max read length is 4096
 */
static bool mt_i2c_should_combine(struct i2c_msg *msg)
{
	struct i2c_msg *next_msg = msg + 1;

	if ((next_msg->len < 4096) &&
			msg->addr == next_msg->addr &&
			!(msg->flags & I2C_M_RD) &&
			(next_msg->flags & I2C_M_RD) == I2C_M_RD) {
		return true;
	}
	return false;
}

static bool mt_i2c_should_batch(struct i2c_msg *prev, struct i2c_msg *next)
{
	if ((prev == NULL) || (next == NULL) ||
	    (prev->flags & I2C_M_RD) || (next->flags & I2C_M_RD))
		return false;
	if ((next != NULL) && (prev != NULL) &&
	    (prev->len == next->len && prev->addr == next->addr))
		return true;
	return false;
}

#if 0
static int __mt_i2c_transfer(struct mt_i2c *i2c,
	struct i2c_msg msgs[], int num)
{
	int ret;
	int left_num = num;

	ret = mt_i2c_clock_enable(i2c);
	if (ret)
		return ret;
	while (left_num--) {
		/* In MTK platform the max transfer number is 4096 */
		if (msgs->len > MAX_DMA_TRANS_SIZE) {
			dev_dbg(i2c->dev,
				" message data length is more than 255\n");
			ret = -EINVAL;
			goto err_exit;
		}
		if (msgs->addr == 0) {
			dev_dbg(i2c->dev, " addr is invalid.\n");
			ret = -EINVAL;
			goto err_exit;
		}
		if (msgs->buf == NULL) {
			dev_dbg(i2c->dev, " data buffer is NULL.\n");
			ret = -EINVAL;
			goto err_exit;
		}

		i2c->addr = msgs->addr;
		i2c->msg_len = msgs->len;
		i2c->msg_buf = msgs->buf;
		i2c->msg_aux_len = 0;
		if (msgs->flags & I2C_M_RD)
			i2c->op = I2C_MASTER_RD;
		else
			i2c->op = I2C_MASTER_WR;
		/* combined two messages into one transaction */
		if (left_num >= 1 && mt_i2c_should_combine(msgs)) {
			i2c->msg_aux_len = (msgs + 1)->len;
			i2c->op = I2C_MASTER_WRRD;
			left_num--;
		}
		/*
		 * always use DMA mode.
		 * 1st when write need copy the data of message to dma memory
		 * 2nd when read need copy the DMA data to the message buffer.
		 * The length should be less than 255.
		 */
		mt_i2c_copy_to_dma(i2c, msgs);
		i2c->msg_buf = (u8 *)i2c->dma_buf.paddr;

		/* Use HW semaphore to protect mt6313 access between AP and SPM */
		if (i2c_get_semaphore(i2c) != 0)
			return -EBUSY;
		ret = mt_i2c_do_transfer(i2c);
		/* Use HW semaphore to protect mt6313 access between AP and SPM */
		if (i2c_release_semaphore(i2c) != 0)
			ret = -EBUSY;
		if (ret < 0)
			goto err_exit;
		if (i2c->op == I2C_MASTER_WRRD)
			mt_i2c_copy_from_dma(i2c, msgs + 1);
		else
			mt_i2c_copy_from_dma(i2c, msgs);
		msgs++;
		/* after combined two messages so we need ignore one */
		if (left_num > 0 && i2c->op == I2C_MASTER_WRRD)
			msgs++;
	}
	/* the return value is number of executed messages */
	ret = num;
err_exit:
	mt_i2c_clock_disable(i2c);
	return ret;
}
#else
static int __mt_i2c_transfer(struct mt_i2c *i2c,
	struct i2c_msg msgs[], int num)
{
	int ret;
	int left_num = num;

	while (left_num--) {
		/* In MTK platform the max transfer number is 4096 */
		if (msgs->len > MAX_DMA_TRANS_SIZE) {
			dev_dbg(i2c->dev,
				" message data length is more than 255\n");
			ret = -EINVAL;
			goto err_exit;
		}
		if (msgs->addr == 0) {
			dev_dbg(i2c->dev, " addr is invalid.\n");
			ret = -EINVAL;
			goto err_exit;
		}
		if (msgs->buf == NULL) {
			dev_dbg(i2c->dev, " data buffer is NULL.\n");
			ret = -EINVAL;
			goto err_exit;
		}

		i2c->addr = msgs->addr;
		i2c->msg_len = msgs->len;
		i2c->msg_aux_len = 0;

		if ((left_num + 1 == num) || !mt_i2c_should_batch(msgs - 1, msgs)) {
			i2c->total_len = msgs->len;
			if (msgs->flags & I2C_M_RD)
				i2c->op = I2C_MASTER_RD;
			else
				i2c->op = I2C_MASTER_WR;
		} else {
			i2c->total_len += msgs->len;
		}

		/*
		 * always use DMA mode.
		 * 1st when write need copy the data of message to dma memory
		 * 2nd when read need copy the DMA data to the message buffer.
		 * The length should be less than 255.
		 */
		mt_i2c_copy_to_dma(i2c, msgs);

		if (left_num >= 1) {
			if (mt_i2c_should_batch(msgs, msgs + 1)) {
				i2c->op = I2C_MASTER_MULTI_WR;
				msgs++;
				continue;
			}
			if (mt_i2c_should_combine(msgs)) {
				i2c->msg_aux_len = (msgs + 1)->len;
				i2c->op = I2C_MASTER_WRRD;
				left_num--;
			}
		}
		#ifdef CONFIG_MTK_TINYSYS_SCP_SUPPORT

		/* Use HW semaphore to protect device access between AP and SPM, or SCP */
		if (i2c_get_semaphore(i2c) != 0) {
			dev_err(i2c->dev, "get hw semaphore failed.\n");
			return -EBUSY;
		}
		#endif
		ret = mt_i2c_do_transfer(i2c);
		#ifdef CONFIG_MTK_TINYSYS_SCP_SUPPORT
		/* Use HW semaphore to protect device access between AP and SPM, or SCP */
		if (i2c_release_semaphore(i2c) != 0) {
			dev_err(i2c->dev, "release hw semaphore failed.\n");
			ret = -EBUSY;
		}
		#endif
		if (ret < 0)
			goto err_exit;
		if (i2c->op == I2C_MASTER_WRRD)
			mt_i2c_copy_from_dma(i2c, msgs + 1);
		else
			mt_i2c_copy_from_dma(i2c, msgs);

		msgs++;
		/* after combined two messages so we need ignore one */
		if (left_num > 0 && i2c->op == I2C_MASTER_WRRD)
			msgs++;
	}
	/* the return value is number of executed messages */
	ret = num;
err_exit:
	return ret;
}
#endif

#ifdef CONFIG_TRUSTONIC_TEE_SUPPORT
int i2c_tui_enable_clock(int id)
{
	struct i2c_adapter *adap;
	struct mt_i2c *i2c;
	int ret;

	adap = i2c_get_adapter(id);
	if (!adap) {
		pr_err("Cannot get adapter\n");
		return -1;
	}

	i2c = i2c_get_adapdata(adap);
	ret = clk_prepare_enable(i2c->clk_main);
	if (ret) {
		pr_err("Cannot enable main clk\n");
		return ret;
	}
	ret = clk_prepare_enable(i2c->clk_dma);
	if (ret) {
		pr_err("Cannot enable dma clk\n");
		clk_disable_unprepare(i2c->clk_main);
		return ret;
	}

	return 0;
}

int i2c_tui_disable_clock(int id)
{
	struct i2c_adapter *adap;
	struct mt_i2c *i2c;

	adap = i2c_get_adapter(id);
	if (!adap) {
		pr_err("Cannot get adapter\n");
		return -1;
	}

	i2c = i2c_get_adapdata(adap);
	clk_disable_unprepare(i2c->clk_dma);
	clk_disable_unprepare(i2c->clk_main);

	return 0;
}
#endif

static int mt_i2c_transfer(struct i2c_adapter *adap,
	struct i2c_msg msgs[], int num)
{
	int ret;
	struct mt_i2c *i2c = i2c_get_adapdata(adap);

	ret = mt_i2c_clock_enable(i2c);
	if (ret)
		return -EBUSY;

	mutex_lock(&i2c->i2c_mutex);
	ret = __mt_i2c_transfer(i2c, msgs, num);
	mutex_unlock(&i2c->i2c_mutex);

	mt_i2c_clock_disable(i2c);
	return ret;
}


static void mt_i2c_parse_extension(struct mt_i2c_ext *pext, u32 ext_flag, u32 timing)
{
	if (ext_flag & I2C_A_FILTER_MSG)
		pext->isFilterMsg = true;
	if (timing)
		pext->timing = timing;
}

int mtk_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num,
					u32 ext_flag, u32 timing)
{
	int ret;
	struct mt_i2c *i2c = i2c_get_adapdata(adap);

	ret = mt_i2c_clock_enable(i2c);
	if (ret)
		return -EBUSY;

	mutex_lock(&i2c->i2c_mutex);
	i2c->ext_data.isEnable = true;

	mt_i2c_parse_extension(&i2c->ext_data, ext_flag, timing);
	ret = __mt_i2c_transfer(i2c, msgs, num);

	i2c->ext_data.isEnable = false;
	mutex_unlock(&i2c->i2c_mutex);

	mt_i2c_clock_disable(i2c);
	return ret;
}
EXPORT_SYMBOL(mtk_i2c_transfer);

int hw_trig_i2c_enable(struct i2c_adapter *adap)
{
	struct mt_i2c *i2c = i2c_get_adapdata(adap);

	if (!i2c->buffermode)
		return -1;
	if (mt_i2c_clock_enable(i2c))
		return -EBUSY;

	mutex_lock(&i2c->i2c_mutex);
	i2c->is_hw_trig = true;
	mutex_unlock(&i2c->i2c_mutex);
	return 0;
}
EXPORT_SYMBOL(hw_trig_i2c_enable);

int hw_trig_i2c_disable(struct i2c_adapter *adap)
{
	struct mt_i2c *i2c = i2c_get_adapdata(adap);

	if (!i2c->buffermode)
		return -1;
	mutex_lock(&i2c->i2c_mutex);
	i2c->is_hw_trig = false;
	mutex_unlock(&i2c->i2c_mutex);
	mt_i2c_wait_done(i2c, 0);
	mt_i2c_clock_disable(i2c);
	return 0;
}
EXPORT_SYMBOL(hw_trig_i2c_disable);

int hw_trig_i2c_transfer(struct i2c_adapter *adap, struct i2c_msg *msgs,
		int num)
{
	int ret;
	struct mt_i2c *i2c = i2c_get_adapdata(adap);

	if (!i2c->buffermode)
		return -1;
	mutex_lock(&i2c->i2c_mutex);
	ret = __mt_i2c_transfer(i2c, msgs, num);
	mutex_unlock(&i2c->i2c_mutex);
	return ret;
}
EXPORT_SYMBOL(hw_trig_i2c_transfer);

int i2c_ccu_enable(struct i2c_adapter *adap, u16 ch_offset)
{
	int ret;
	struct mt_i2c *i2c = i2c_get_adapdata(adap);
	char buf[1];
	/*This is just a dummy msg which is meaningless since these parameter
	 * is actually not used.
	 */
	struct i2c_msg dummy_msg = {
		.addr = 0x1,
		.flags = I2C_MASTER_RD,
		.len = 1,
		.buf = (char *)buf,
	};
	if (mt_i2c_clock_enable(i2c))
		return -EBUSY;
	mutex_lock(&i2c->i2c_mutex);
	i2c->is_ccu_trig = true;
	i2c->ext_data.ch_offset = ch_offset;
	i2c->ext_data.is_ch_offset = true;
	ret = __mt_i2c_transfer(i2c, &dummy_msg, 1);
	i2c->is_ccu_trig = false;
	i2c->ext_data.is_ch_offset = false;
	mutex_unlock(&i2c->i2c_mutex);
	return ret;
}
EXPORT_SYMBOL(i2c_ccu_enable);

int i2c_ccu_disable(struct i2c_adapter *adap)
{
	struct mt_i2c *i2c = i2c_get_adapdata(adap);
	mt_i2c_wait_done(i2c, i2c->ccu_offset);
	mt_i2c_clock_disable(i2c);
	return 0;
}
EXPORT_SYMBOL(i2c_ccu_disable);

static irqreturn_t mt_i2c_irq(int irqno, void *dev_id)
{
	struct mt_i2c *i2c = dev_id;
	u16 int_reg = I2C_HS_NACKERR | I2C_ACKERR |
		      I2C_TRANSAC_COMP | I2C_ARB_LOST;

	i2c_writew(0, i2c, OFFSET_INTR_MASK);
	i2c->irq_stat = i2c_readw(i2c, OFFSET_INTR_STAT);
	if (i2c->dev_comp->ver == 0x2)
		int_reg = I2C_INTR_ALL;
	i2c_writew(int_reg, i2c, OFFSET_INTR_STAT);

	i2c->trans_stop = true;
	if (!i2c->is_hw_trig) {
		wake_up(&i2c->wait);
		if (!i2c->irq_stat) {
			dev_info(i2c->dev, "addr: %x, irq stat 0\n", i2c->addr);
			i2c_dump_info(i2c);
			#if defined(CONFIG_MTK_GIC_EXT)
			mt_irq_dump_status(i2c->irqnr);
			#endif
		}
	}
	else {	/* dump regs info for hw trig i2c if ACK err */
		if (i2c->irq_stat & (I2C_HS_NACKERR | I2C_ACKERR)) {
			dev_err(i2c->dev, "addr: %x, transfer ACK error\n", i2c->addr);
			mt_i2c_init_hw(i2c);
		}
	}
	return IRQ_HANDLED;
}

static u32 mt_i2c_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_10BIT_ADDR | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm mt_i2c_algorithm = {
	.master_xfer = mt_i2c_transfer,
	.functionality = mt_i2c_functionality,
};

static int mt_i2c_parse_dt(struct device_node *np, struct mt_i2c *i2c)
{
	int ret = 0;
	i2c->speed_hz = I2C_DEFAUT_SPEED;
	of_property_read_u32(np, "clock-frequency", &i2c->speed_hz);
	of_property_read_u32(np, "clock-div", &i2c->clk_src_div);
	of_property_read_u32(np, "id", (u32 *)&i2c->id);
	of_property_read_u32(np, "ch_offset_default", &i2c->ch_offset_default);
	of_property_read_u32(np, "dma_ch_offset_default", &i2c->dma_ch_offset_default);
	of_property_read_u32(np, "aed", &i2c->aed);
	ret = of_property_read_u32(np, "ccu-ch-offset", &i2c->ccu_offset);
	if (ret >= 0)
		i2c->have_ccu = true;
	else
		i2c->have_ccu = false;

	i2c->have_pmic = of_property_read_bool(np, "mediatek,have-pmic");
	i2c->have_dcm = of_property_read_bool(np, "mediatek,have-dcm");
	i2c->use_push_pull = of_property_read_bool(np, "mediatek,use-push-pull");
	i2c->appm = of_property_read_bool(np, "mediatek,appm_used");
	i2c->gpupm = of_property_read_bool(np, "mediatek,gpupm_used");
	i2c->buffermode = of_property_read_bool(np, "mediatek,buffermode_used");
	i2c->hs_only = of_property_read_bool(np, "mediatek,hs_only");
	i2c->skip_scp_sema = of_property_read_bool(np, "mediatek,skip_scp_sema");
	pr_info("[I2C] id : %d, freq : %d, div : %d, ch_offset: %d, dma_ch_offset: %d\n",
		i2c->id, i2c->speed_hz, i2c->clk_src_div, i2c->ch_offset_default,
		i2c->dma_ch_offset_default);
	if (i2c->clk_src_div == 0)
		return -EINVAL;
	return 0;
}


static const struct mtk_i2c_compatible mt6775_compat = {
	.dma_support = 2,
	.idvfs_i2c = 1,
	.set_dt_div = 1,
	.set_ltiming = 1,
	.set_aed = 1,
	.check_max_freq = 1,
	.ext_time_config = 0,
	.ver = 0x2,
};

static const struct mtk_i2c_compatible mt6771_compat = {
	.dma_support = 2,
	.idvfs_i2c = 1,
	.set_dt_div = 1,
	.set_ltiming = 1,
	.set_aed = 0,
	.check_max_freq = 1,
	.ext_time_config = 0x1801,
	.ver = 0x2,
#if 0
	.clk_compatible = "mediatek,infracfg_ao",
	.clk_sta_offset[0] = 0x90,
	.cg_bit[0] = 11,
	.cg_bit[1] = 12,
	.cg_bit[2] = 13,
	.cg_bit[3] = 14,
	.clk_sta_offset[1] = 0xAC,
	.cg_bit[4] = 7,
	.cg_bit[5] = 18,
	.cg_bit[6] = 20,
	.cg_bit[7] = 22,
	.cg_bit[8] = 24,
	.clk_sta_offset[2] = 0xC8,
	.cg_bit[9] = 6,
#endif
	.pericfg_compatible = "mediatek,pericfg",
	.infracfg_compatible = "mediatek,infracfg_ao",
	.peri_dbg_offset = 0x500,
	.peri_dbg_length = 0x4c,
	.infra_dbg_offset = 0xD00,
	.infra_dbg_length = 0x64,
};

static const struct mtk_i2c_compatible mt6735_compat = {
	.dma_support = 0,
	.idvfs_i2c = 0,
	.set_dt_div = 0,
	.set_ltiming = 0,
	.check_max_freq = 1,
	.ext_time_config = 0,
};

static const struct mtk_i2c_compatible mt6739_compat = {
	.dma_support = 1,
	.idvfs_i2c = 0,
	.set_dt_div = 1,
	.set_ltiming = 1,
	.set_aed = 1,
	.check_max_freq = 1,
	.ext_time_config = 0x1801,
};

static const struct mtk_i2c_compatible mt6797_compat = {
	.dma_support = 1,
	.idvfs_i2c = 1,
	.set_dt_div = 0,
	.set_ltiming = 0,
	.check_max_freq = 1,
	.ext_time_config = 0,
};

static const struct mtk_i2c_compatible mt6757_compat = {
	.dma_support = 2,
	.idvfs_i2c = 0,
	.set_dt_div = 0,
	.set_ltiming = 0,
	.check_max_freq = 1,
	.ext_time_config = 0x201,
};

static const struct mtk_i2c_compatible mt6758_compat = {
	.dma_support = 2,
	.idvfs_i2c = 1,
	.set_dt_div = 1,
	.set_ltiming = 0,
	.check_max_freq = 1,
	.ext_time_config = 0,
	.clk_compatible = "mediatek,pericfg",
	.clk_sta_offset[0] = 0x278,
	.cg_bit[0] = 16,
	.cg_bit[1] = 17,
	.cg_bit[2] = 24,
	.cg_bit[3] = 25,
	.cg_bit[4] = 20,
	.cg_bit[5] = 21,
	.cg_bit[6] = 22,
	.cg_bit[7] = 23,
	.cg_bit[8] = 18,
	.cg_bit[9] = 19,
};

static const struct mtk_i2c_compatible mt6759_compat = {
	.dma_support = 2,
	.idvfs_i2c = 1,
	.set_dt_div = 1,
	.set_ltiming = 0,
	.check_max_freq = 1,
	.ext_time_config = 0,
	.clk_compatible = "mediatek,pericfg",
	.clk_sta_offset[0] = 0x278,
	.cg_bit[0] = 16,
	.cg_bit[1] = 17,
	.cg_bit[2] = 24,
	.cg_bit[3] = 25,
	.cg_bit[4] = 20,
	.cg_bit[5] = 21,
	.cg_bit[6] = 22,
	.cg_bit[7] = 23,
	.cg_bit[8] = 18,
	.cg_bit[9] = 19,
};

static const struct mtk_i2c_compatible mt6799_compat = {
	.dma_support = 3,
	.idvfs_i2c = 1,
	.set_dt_div = 1,
	.set_ltiming = 0,
	.check_max_freq = 0,
	.ext_time_config = 0,
};

static const struct mtk_i2c_compatible mt6763_compat = {
	.dma_support = 2,
	.idvfs_i2c = 1,
	.set_dt_div = 1,
	.set_ltiming = 1,
	.set_aed = 1,
	.check_max_freq = 1,
	.ext_time_config = 0x1801,
	.clk_compatible = "mediatek,infracfg_ao",
	.clk_sta_offset[0] = 0x90,
	.cg_bit[0] = 11,
	.clk_sta_offset[1] = 0xac,
	.cg_bit[1] = 22,
	.clk_sta_offset[2] = 0xac,
	.cg_bit[2] = 24,
	.clk_sta_offset[3] = 0xac,
	.cg_bit[3] = 7,
	.clk_sta_offset[4] = 0xac,
	.cg_bit[4] = 20,
	.clk_sta_offset[5] = 0x90,
	.cg_bit[5] = 14,
	.clk_sta_offset[6] = 0xc8,
	.cg_bit[6] = 6,
	.clk_sta_offset[7] = 0x90,
	.cg_bit[7] = 12,
	.clk_sta_offset[8] = 0x90,
	.cg_bit[8] = 13,
	.clk_sta_offset[9] = 0xac,
	.cg_bit[9] = 18,
};

static const struct mtk_i2c_compatible elbrus_compat = {
	.dma_support = 2,
	.idvfs_i2c = 1,
	.set_dt_div = 0,
	.set_ltiming = 0,
	.check_max_freq = 1,
	.ext_time_config = 0,
};

static const struct of_device_id mtk_i2c_of_match[] = {
	{ .compatible = "mediatek,mt6775-i2c", .data = &mt6775_compat },
	{ .compatible = "mediatek,mt6771-i2c", .data = &mt6771_compat },
	{ .compatible = "mediatek,mt6735-i2c", .data = &mt6735_compat },
	{ .compatible = "mediatek,mt6739-i2c", .data = &mt6739_compat },
	{ .compatible = "mediatek,mt6797-i2c", .data = &mt6797_compat },
	{ .compatible = "mediatek,mt6757-i2c", .data = &mt6757_compat },
	{ .compatible = "mediatek,mt6758-i2c", .data = &mt6758_compat },
	{ .compatible = "mediatek,mt6759-i2c", .data = &mt6759_compat },
	{ .compatible = "mediatek,mt6799-i2c", .data = &mt6799_compat },
	{ .compatible = "mediatek,mt6763-i2c", .data = &mt6763_compat },
	{ .compatible = "mediatek,elbrus-i2c", .data = &elbrus_compat },
	{},
};

MODULE_DEVICE_TABLE(of, mtk_i2c_of_match);

static int mt_i2c_probe(struct platform_device *pdev)
{
	int ret = 0;
	struct mt_i2c *i2c;
	unsigned int clk_src_in_hz;
	struct resource *res;
	const struct of_device_id *of_id;

	i2c = devm_kzalloc(&pdev->dev, sizeof(struct mt_i2c), GFP_KERNEL);
	if (i2c == NULL)
		return -ENOMEM;

	ret = mt_i2c_parse_dt(pdev->dev.of_node, i2c);
	if (ret)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);

	i2c->base = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c->base))
		return PTR_ERR(i2c->base);

	if (i2c->id < I2C_MAX_CHANNEL)
		g_mt_i2c[i2c->id] = i2c;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 1);

	i2c->pdmabase = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2c->pdmabase))
		return PTR_ERR(i2c->pdmabase);

	i2c->irqnr = platform_get_irq(pdev, 0);
	if (i2c->irqnr <= 0)
		return -EINVAL;
	init_waitqueue_head(&i2c->wait);

	ret = devm_request_irq(&pdev->dev, i2c->irqnr, mt_i2c_irq,
		IRQF_NO_SUSPEND | IRQF_TRIGGER_NONE, I2C_DRV_NAME, i2c);
	if (ret < 0) {
		dev_err(&pdev->dev,
			"Request I2C IRQ %d fail\n", i2c->irqnr);
		return ret;
	}
	of_id = of_match_node(mtk_i2c_of_match, pdev->dev.of_node);
	if (!of_id)
		return -EINVAL;

	i2c->dev_comp = of_id->data;
	i2c->adap.dev.of_node = pdev->dev.of_node;
	i2c->dev = &i2c->adap.dev;
	i2c->adap.dev.parent = &pdev->dev;
	i2c->adap.owner = THIS_MODULE;
	i2c->adap.algo = &mt_i2c_algorithm;
	i2c->adap.algo_data = NULL;
	i2c->adap.timeout = 2 * HZ;
	i2c->adap.retries = 1;
	i2c->adap.nr = i2c->id;
	spin_lock_init(&i2c->cg_lock);

	if (i2c->dev_comp->dma_support == 2) {
		if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(33))) {
			dev_err(&pdev->dev, "dma_set_mask return error.\n");
			return -EINVAL;
		}
	} else if (i2c->dev_comp->dma_support == 3) {
		if (dma_set_mask(&pdev->dev, DMA_BIT_MASK(36))) {
			dev_err(&pdev->dev, "dma_set_mask return error.\n");
			return -EINVAL;
		}
	}

#if !defined(CONFIG_MT_I2C_FPGA_ENABLE)
	i2c->clk_main = devm_clk_get(&pdev->dev, "main");
	if (IS_ERR(i2c->clk_main)) {
		dev_err(&pdev->dev, "cannot get main clock\n");
		return PTR_ERR(i2c->clk_main);
	}
	i2c->clk_dma = devm_clk_get(&pdev->dev, "dma");
	if (IS_ERR(i2c->clk_dma)) {
		dev_err(&pdev->dev, "cannot get dma clock\n");
		return PTR_ERR(i2c->clk_dma);
	}
	i2c->clk_arb = devm_clk_get(&pdev->dev, "arb");
	if (IS_ERR(i2c->clk_arb))
		i2c->clk_arb = NULL;
	else
		dev_dbg(&pdev->dev, "i2c%d has the relevant arbitrator clk.\n", i2c->id);
#endif

	if (i2c->have_pmic) {
		i2c->clk_pmic = devm_clk_get(&pdev->dev, "pmic");
		if (IS_ERR(i2c->clk_pmic)) {
			dev_err(&pdev->dev, "cannot get pmic clock\n");
			return PTR_ERR(i2c->clk_pmic);
		}
		clk_src_in_hz = clk_get_rate(i2c->clk_pmic) / i2c->clk_src_div;
		i2c->main_clk = clk_src_in_hz;
	} else {
		clk_src_in_hz = clk_get_rate(i2c->clk_main) / i2c->clk_src_div;
		i2c->main_clk = clk_src_in_hz;
	}
	dev_warn(&pdev->dev, "i2c%d clock source %p,clock src frequency %d\n", i2c->id,
		i2c->clk_main, clk_src_in_hz);

	strlcpy(i2c->adap.name, I2C_DRV_NAME, sizeof(i2c->adap.name));
	mutex_init(&i2c->i2c_mutex);
	ret = i2c_set_speed(i2c, clk_src_in_hz);
	if (ret) {
		dev_err(&pdev->dev, "Failed to set the speed\n");
		return -EINVAL;
	}
	ret = mt_i2c_clock_enable(i2c);
	if (ret) {
		dev_err(&pdev->dev, "clock enable failed!\n");
		return ret;
	}
	mt_i2c_init_hw(i2c);
	mt_i2c_clock_disable(i2c);
	if (i2c->ch_offset_default)
		i2c->dma_buf.vaddr = dma_alloc_coherent(&pdev->dev,
			PAGE_SIZE * 2, &i2c->dma_buf.paddr, GFP_KERNEL);
	else
		i2c->dma_buf.vaddr = dma_alloc_coherent(&pdev->dev,
			PAGE_SIZE, &i2c->dma_buf.paddr, GFP_KERNEL);
	if (i2c->dma_buf.vaddr == NULL) {
		dev_err(&pdev->dev, "dma_alloc_coherent fail\n");
		return -ENOMEM;
	}
	i2c_set_adapdata(&i2c->adap, i2c);
	/* ret = i2c_add_adapter(&i2c->adap); */
	ret = i2c_add_numbered_adapter(&i2c->adap);
	if (ret) {
		dev_err(&pdev->dev, "Failed to add i2c bus to i2c core\n");
		free_i2c_dma_bufs(i2c);
		return ret;
	}
	platform_set_drvdata(pdev, i2c);

	if (!map_cg_regs(i2c))
		pr_warn("Map cg regs successfully.\n");

	if (!map_bus_regs(i2c, I2C_DUMP_INFRA_DBG))
		dev_info(i2c->dev, "Map infra regs successfully.\n");

	if (!map_bus_regs(i2c, I2C_DUMP_PERI_DBG))
		dev_info(i2c->dev, "Map peri regs successfully.\n");

	return 0;
}

static int mt_i2c_remove(struct platform_device *pdev)
{
	struct mt_i2c *i2c = platform_get_drvdata(pdev);

	i2c_del_adapter(&i2c->adap);
	free_i2c_dma_bufs(i2c);
	platform_set_drvdata(pdev, NULL);
	return 0;
}


MODULE_DEVICE_TABLE(of, mt_i2c_match);

#ifdef CONFIG_PM_SLEEP
static int mt_i2c_suspend_noirq(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mt_i2c *i2c = platform_get_drvdata(pdev);
	int ret = 0;

	spin_lock(&i2c->cg_lock);
	if (i2c->cg_cnt > 0) {
		ret = -EBUSY;
		dev_info(i2c->dev, "%s(%d) busy\n", __func__, i2c->cg_cnt);
	} else
		i2c->suspended = true;
	spin_unlock(&i2c->cg_lock);

	return ret;
}

static int mt_i2c_resume_noirq(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	struct mt_i2c *i2c = platform_get_drvdata(pdev);

	spin_lock(&i2c->cg_lock);
	i2c->suspended = false;
	spin_unlock(&i2c->cg_lock);

	if (i2c->ch_offset_default) {
		if (mt_i2c_clock_enable(i2c))
			dev_info(i2c->dev, "%s enable clock failed\n",
				 __func__);

		/* Disable rollback mode for multi-channel */
		mt_secure_call(MTK_SIP_KERNEL_I2C_SEC_WRITE,
			       i2c->id, V2_OFFSET_ROLLBACK, 0);

		/* Enable multi-channel DMA mode */
		mt_secure_call(MTK_SIP_KERNEL_I2C_SEC_WRITE, i2c->id,
			       V2_OFFSET_MULTI_DMA, I2C_SHADOW_REG_MODE);

		mt_i2c_clock_disable(i2c);
	}

	return 0;
}
#endif

static const struct dev_pm_ops mt_i2c_dev_pm_ops = {
#ifdef CONFIG_PM_SLEEP
	.suspend_noirq = mt_i2c_suspend_noirq,
	.resume_noirq = mt_i2c_resume_noirq,
#endif
};

static struct platform_driver mt_i2c_driver = {
	.probe = mt_i2c_probe,
	.remove = mt_i2c_remove,
	.driver = {
		.name = I2C_DRV_NAME,
		.owner = THIS_MODULE,
		.pm = &mt_i2c_dev_pm_ops,
		.of_match_table = of_match_ptr(mtk_i2c_of_match),
	},
};

#ifdef CONFIG_MTK_I2C_ARBITRATION
static s32 enable_arbitration(void)
{
	struct device_node *pericfg_node;
	void __iomem *pericfg_base;

	pericfg_node = of_find_compatible_node(NULL, NULL, "mediatek,pericfg");
	if (!pericfg_node) {
		pr_err("Cannot find pericfg node\n");
		return -ENODEV;
	}
	pericfg_base = of_iomap(pericfg_node, 0);
	if (!pericfg_base) {
		pr_err("pericfg iomap failed\n");
		return -ENOMEM;
	}
	/* Enable the I2C arbitration */
	writew(0x3, pericfg_base + OFFSET_PERI_I2C_MODE_ENABLE);
	return 0;
}
#endif

static s32 __init mt_i2c_init(void)
{
#ifdef CONFIG_MTK_I2C_ARBITRATION
	int ret;

	ret = enable_arbitration();
	if (ret) {
		pr_err("Cannot enalbe arbitration.\n");
		return ret;
	}
#endif

	if (!map_dma_regs())
		pr_warn("Mapp dma regs successfully.\n");
	pr_err(" mt_i2c_init driver as platform device\n");
	return platform_driver_register(&mt_i2c_driver);
}

static void __exit mt_i2c_exit(void)
{
	platform_driver_unregister(&mt_i2c_driver);
}

module_init(mt_i2c_init);
module_exit(mt_i2c_exit);

/* module_platform_driver(mt_i2c_driver); */

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MediaTek I2C Bus Driver");
MODULE_AUTHOR("Xudong Chen <xudong.chen@mediatek.com>");
