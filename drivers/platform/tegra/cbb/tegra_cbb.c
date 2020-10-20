/*
 * Copyright (c) 2017-2020, NVIDIA CORPORATION.  All rights reserved.
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
 * The driver handles Error's from Control Backbone(CBB) generated due to
 * illegal accesses. When an error is reported from a NOC within CBB,
 * the driver prints error type and debug information about failed transaction.
 */

#include <asm/traps.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/of_irq.h>
#include <linux/of_address.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/version.h>
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
#include <soc/tegra/chip-id.h>
#else
#include <soc/tegra/fuse.h>
#endif
#include <linux/platform/tegra/tegra_cbb.h>

static struct tegra_cbberr_ops *cbberr_ops;

void print_cbb_err(struct seq_file *file, const char *fmt, ...)
{
	va_list args;
	struct va_format vaf;

	va_start(args, fmt);

	if (file) {
		seq_vprintf(file, fmt, args);
	} else {
		vaf.fmt = fmt;
		vaf.va = &args;
		pr_crit("%pV", &vaf);
	}

	va_end(args);
}

void print_cache(struct seq_file *file, u32 cache)
{
	if ((cache & 0x3) == 0x0) {
		print_cbb_err(file, "\t  Cache\t\t\t: 0x%x -- "
			"Non-cacheable/Non-Bufferable)\n", cache);
		return;
	}
	if ((cache & 0x3) == 0x1) {
		print_cbb_err(file, "\t  Cache\t\t\t: 0x%x -- Device\n", cache);
		return;
	}

	switch (cache) {
	case 0x2:
		print_cbb_err(file,
		"\t  Cache\t\t\t: 0x%x -- Cacheable/Non-Bufferable\n", cache);
		break;

	case 0x3:
		print_cbb_err(file,
		"\t  Cache\t\t\t: 0x%x -- Cacheable/Bufferable\n", cache);
		break;

	default:
		print_cbb_err(file,
		"\t  Cache\t\t\t: 0x%x -- Cacheable\n", cache);
	}
}

void print_prot(struct seq_file *file, u32 prot)
{
	char *data_str;
	char *secure_str;
	char *priv_str;

	data_str = (prot & 0x4) ? "Instruction" : "Data";
	secure_str = (prot & 0x2) ? "Non-Secure" : "Secure";
	priv_str = (prot & 0x1) ? "Privileged" : "Unprivileged";

	print_cbb_err(file, "\t  Protection\t\t: 0x%x -- %s, %s, %s Access\n",
			prot, priv_str, secure_str, data_str);
}

#ifdef CONFIG_DEBUG_FS
static int created_root;

static int cbb_err_show(struct seq_file *file, void *data)
{
	return cbberr_ops->cbb_err_debugfs_show(file, data);
}

static int cbb_err_open(struct inode *inode, struct file *file)
{
	return single_open(file, cbb_err_show, inode->i_private);
}

static const struct file_operations cbb_err_fops = {
	.open = cbb_err_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release
};

static int tegra_cbb_noc_dbgfs_init(void)
{
	struct dentry *d;
	if (!created_root) {
		d = debugfs_create_file("tegra_cbb_err",
				S_IRUGO, NULL, NULL, &cbb_err_fops);
		if (IS_ERR_OR_NULL(d)) {
			pr_err("%s: could not create 'tegra_cbb_err' node\n",
					__func__);
			return PTR_ERR(d);
		}
		created_root = true;
	}
	return 0;
}

#else
static int tegra_cbb_noc_dbgfs_init(void) { return 0; }
#endif

void tegra_cbb_stallen(void __iomem *addr)
{
	if (cbberr_ops->stallen)
		cbberr_ops->stallen(addr);
}

void tegra_cbb_faulten(void __iomem *addr)
{
	if (cbberr_ops->faulten)
		cbberr_ops->faulten(addr);
}

void tegra_cbb_errclr(void __iomem *addr)
{
	if (cbberr_ops->errclr)
		cbberr_ops->errclr(addr);
}

unsigned int tegra_cbb_errvld(void __iomem *addr)
{
	if (cbberr_ops->errvld)
		return cbberr_ops->errvld(addr);
	else
		return 0;
}

void tegra_cbberr_set_ops(struct tegra_cbberr_ops *tegra_cbb_err_ops)
{
	cbberr_ops = tegra_cbb_err_ops;
}

int tegra_cbb_err_getirq(struct platform_device *pdev,
		int *nonsecure_irq, int *secure_irq, int *num_intr)
{
	int err = 0;
	int intr_indx = 0;

	*nonsecure_irq = 0x0;
	*secure_irq = 0x0;

	*num_intr = platform_irq_count(pdev);
	if (!*num_intr)
		return -EINVAL;

	if (*num_intr == 2) {
		*nonsecure_irq = platform_get_irq(pdev, intr_indx);
		if (*nonsecure_irq <= 0) {
			dev_err(&pdev->dev, "can't get irq (%d)\n",
					*nonsecure_irq);
			return -ENOENT;
		}
		intr_indx++;
	}

	*secure_irq = platform_get_irq(pdev, intr_indx);
	if (*secure_irq <= 0) {
		dev_err(&pdev->dev, "can't get irq (%d)\n", *secure_irq);
		return -ENOENT;
	}

	if (*num_intr == 1)
		dev_info(&pdev->dev, "secure_irq = %d\n", *secure_irq);
	if (*num_intr == 2)
		dev_info(&pdev->dev, "secure_irq = %d, nonsecure_irq = %d>\n",
						*secure_irq, *nonsecure_irq);
	return err;
}

int tegra_cbberr_register_hook_en(struct platform_device *pdev,
			const struct tegra_cbb_noc_data *bdata,
			struct serr_hook *callback,
			struct tegra_cbb_init_data cbb_init_data)
{
	int ret = 0;

	ret = tegra_cbb_noc_dbgfs_init();
	if (ret) {
		dev_err(&pdev->dev, "failed to create debugfs\n");
		return ret;
	}

	if (bdata->erd_mask_inband_err) {
		/* set Error Response Disable to mask SError/inband errors */
		ret = bdata->tegra_cbb_noc_set_erd(cbb_init_data.addr_mask_erd);
		if (ret) {
			dev_err(&pdev->dev, "couldn't mask inband errors\n");
			return ret;
		}
	}

	/* register SError handler for CBB errors due to CCPLEX master */
	if (callback)
		register_serr_hook(callback);

	/* register interrupt handler for CBB errors due to different masters.
	 * If ERD bit is set then CBB NOC error will not generate SErrors for
	 * CCPLEX. It will only trigger LIC interrupts to print error info.
	 */
	ret = cbberr_ops->cbb_enable_interrupt(pdev, cbb_init_data.secure_irq,
				cbb_init_data.nonsecure_irq);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to register CBB Interrupt ISR");
		return ret;
	}

	cbberr_ops->cbb_error_enable(cbb_init_data.vaddr);
	dsb(sy);

	return ret;
}
