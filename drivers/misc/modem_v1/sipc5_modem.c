/* linux/drivers/modem/modem.c
 *
 * Copyright (C) 2010 Google, Inc.
 * Copyright (C) 2010 Samsung Electronics.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/if_arp.h>

#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/irq.h>
#include <linux/gpio.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif
#include <linux/delay.h>
#include <linux/wakelock.h>

#include "modem_prj.h"
#include "modem_variation.h"
#include "modem_utils.h"

#define FMT_WAKE_TIME   (HZ/2)
#define RAW_WAKE_TIME   (HZ*6)

static struct modem_shared *create_modem_shared_data(
				struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct modem_shared *msd;
	int size = MAX_MIF_BUFF_SIZE;

	msd = devm_kzalloc(dev, sizeof(struct modem_shared), GFP_KERNEL);
	if (!msd)
		return NULL;

	/* initialize link device list */
	INIT_LIST_HEAD(&msd->link_dev_list);

	/* initialize tree of io devices */
	msd->iodevs_tree_chan = RB_ROOT;
	msd->iodevs_tree_fmt = RB_ROOT;

	msd->storage.cnt = 0;
	msd->storage.addr = devm_kzalloc(dev, MAX_MIF_BUFF_SIZE +
		(MAX_MIF_SEPA_SIZE * 2), GFP_KERNEL);
	if (!msd->storage.addr) {
		mif_err("IPC logger buff alloc failed!!\n");
		return NULL;
	}
	memset(msd->storage.addr, 0, size + (MAX_MIF_SEPA_SIZE * 2));
	memcpy(msd->storage.addr, MIF_SEPARATOR, MAX_MIF_SEPA_SIZE);
	msd->storage.addr += MAX_MIF_SEPA_SIZE;
	memcpy(msd->storage.addr, &size, MAX_MIF_SEPA_SIZE);
	msd->storage.addr += MAX_MIF_SEPA_SIZE;
	spin_lock_init(&msd->lock);

	return msd;
}

static struct modem_ctl *create_modemctl_device(struct platform_device *pdev,
		struct modem_shared *msd)
{
	struct device *dev = &pdev->dev;
	struct modem_data *pdata = pdev->dev.platform_data;
	struct modem_ctl *modemctl;
	int ret;

	/* create modem control device */
	modemctl = devm_kzalloc(dev, sizeof(struct modem_ctl), GFP_KERNEL);
	if (!modemctl) {
		mif_err("%s: modemctl devm_kzalloc fail\n", pdata->name);
		mif_err("%s: xxx\n", pdata->name);
		return NULL;
	}

	modemctl->msd = msd;
	modemctl->dev = dev;
	modemctl->phone_state = STATE_OFFLINE;

	modemctl->mdm_data = pdata;
	modemctl->name = pdata->name;

	/* init modemctl device for getting modemctl operations */
	ret = call_modem_init_func(modemctl, pdata);
	if (ret) {
		mif_err("%s: call_modem_init_func fail (err %d)\n",
			pdata->name, ret);
		mif_err("%s: xxx\n", pdata->name);
		devm_kfree(dev, modemctl);
		return NULL;
	}

	mif_info("%s is created!!!\n", pdata->name);

	return modemctl;
}

static struct io_device *create_io_device(struct platform_device *pdev,
		struct modem_io_t *io_t, struct modem_shared *msd,
		struct modem_ctl *modemctl, struct modem_data *pdata)
{
	int ret;
	struct device *dev = &pdev->dev;
	struct io_device *iod;

	iod = devm_kzalloc(dev, sizeof(struct io_device), GFP_KERNEL);
	if (!iod) {
		mif_err("iod == NULL\n");
		return NULL;
	}

	RB_CLEAR_NODE(&iod->node_chan);
	RB_CLEAR_NODE(&iod->node_fmt);

	iod->name = io_t->name;
	iod->id = io_t->id;
	iod->format = io_t->format;
	iod->io_typ = io_t->io_type;
	iod->link_types = io_t->links;
	iod->app = io_t->app;
	iod->net_typ = pdata->modem_net;
	iod->use_handover = pdata->use_handover;
	iod->ipc_version = pdata->ipc_version;
	atomic_set(&iod->opened, 0);

	/* link between io device and modem control */
	iod->mc = modemctl;
	if (iod->format == IPC_FMT)
		modemctl->iod = iod;
	if (iod->format == IPC_BOOT) {
		modemctl->bootd = iod;
		mif_err("BOOT device = %s\n", iod->name);
	}

	/* link between io device and modem shared */
	iod->msd = msd;

	/* add iod to rb_tree */
	if (iod->format != IPC_RAW)
		insert_iod_with_format(msd, iod->format, iod);

	if (sipc5_is_not_reserved_channel(iod->id))
		insert_iod_with_channel(msd, iod->id, iod);

	/* register misc device or net device */
	ret = sipc5_init_io_device(iod);
	if (ret) {
		devm_kfree(dev, iod);
		mif_err("sipc5_init_io_device fail (%d)\n", ret);
		return NULL;
	}

	mif_info("%s created\n", iod->name);
	return iod;
}

static int attach_devices(struct io_device *iod, enum modem_link tx_link)
{
	struct modem_shared *msd = iod->msd;
	struct link_device *ld;

	/* find link type for this io device */
	list_for_each_entry(ld, &msd->link_dev_list, list) {
		if (IS_CONNECTED(iod, ld)) {
			/* The count 1 bits of iod->link_types is count
			 * of link devices of this iod.
			 * If use one link device,
			 * or, 2+ link devices and this link is tx_link,
			 * set iod's link device with ld
			 */
			if ((countbits(iod->link_types) <= 1) ||
					(tx_link == ld->link_type)) {
				mif_debug("set %s->%s\n", iod->name, ld->name);
				set_current_link(iod, ld);
			}
		}
	}

	/* if use rx dynamic switch, set tx_link at modem_io_t of
	 * board-*-modems.c
	 */
	if (!get_current_link(iod)) {
		mif_err("%s->link == NULL\n", iod->name);
		BUG();
	}

	switch (iod->format) {
	case IPC_FMT:
		wake_lock_init(&iod->wakelock, WAKE_LOCK_SUSPEND, iod->name);
		iod->waketime = FMT_WAKE_TIME;
		break;

	case IPC_RAW:
		wake_lock_init(&iod->wakelock, WAKE_LOCK_SUSPEND, iod->name);
		iod->waketime = RAW_WAKE_TIME;
		break;

	case IPC_RFS:
		wake_lock_init(&iod->wakelock, WAKE_LOCK_SUSPEND, iod->name);
		iod->waketime = RAW_WAKE_TIME;
		break;

	case IPC_MULTI_RAW:
		wake_lock_init(&iod->wakelock, WAKE_LOCK_SUSPEND, iod->name);
		iod->waketime = RAW_WAKE_TIME;
		break;

	case IPC_BOOT:
		wake_lock_init(&iod->wakelock, WAKE_LOCK_SUSPEND, iod->name);
		iod->waketime = RAW_WAKE_TIME;
		break;

	default:
		break;
	}

	return 0;
}

#ifdef CONFIG_OF
static int parse_dt_common_pdata(struct device_node *np,
		struct modem_data *pdata)
{
	u32 val;

	mif_dt_read_string(np, "mif,name", pdata->name);

	mif_dt_read_enum(np, "mif,modem_net", pdata->modem_net);
	mif_dt_read_enum(np, "mif,modem_type", pdata->modem_type);
	mif_dt_read_bool(np, "mif,use_handover", pdata->use_handover);

	mif_dt_read_enum(np, "mif,link_types", pdata->link_types);
	mif_dt_read_string(np, "mif,link_name", pdata->link_name);

	mif_dt_read_enum(np, "mif,ipc_version", pdata->ipc_version);
	mif_dt_read_u32(np, "mif,num_iodevs", pdata->num_iodevs);

	return 0;
}

static int __parse_dt_mandatory_gpio_pdata(struct device_node *np,
					   struct modem_data *pdata)
{
	int ret = 0;

	/* GPIO_CP_ON */
	pdata->gpio_cp_on = of_get_named_gpio(np, "mif,gpio_cp_on", 0);
	if (!gpio_is_valid(pdata->gpio_cp_on)) {
		mif_err("cp_on: Invalied gpio pins\n");
		return -EINVAL;
	}

	mif_err("gpio_cp_on: %d\n", pdata->gpio_cp_on);
	ret = gpio_request(pdata->gpio_cp_on, "CP_ON");
	if (ret)
		mif_err("fail to request gpio %s:%d\n", "CP_ON", ret);
	gpio_direction_output(pdata->gpio_cp_on, 0);

	/* GPIO_CP_RESET */
	pdata->gpio_cp_reset = of_get_named_gpio(np, "mif,gpio_cp_reset", 0);
	if (!gpio_is_valid(pdata->gpio_cp_reset)) {
		mif_err("cp_rst: Invalied gpio pins\n");
		return -EINVAL;
	}

	mif_err("gpio_cp_reset: %d\n", pdata->gpio_cp_reset);
	ret = gpio_request(pdata->gpio_cp_reset, "CP_RST");
	if (ret)
		mif_err("fail to request gpio %s:%d\n", "CP_RST", ret);
	gpio_direction_output(pdata->gpio_cp_reset, 0);

	/* GPIO_PDA_ACTIVE */
	pdata->gpio_pda_active = of_get_named_gpio(np,
						"mif,gpio_pda_active", 0);
	if (!gpio_is_valid(pdata->gpio_pda_active)) {
		mif_err("pda_active: Invalied gpio pins\n");
		return -EINVAL;
	}

	mif_err("gpio_pda_active: %d\n", pdata->gpio_pda_active);
	ret = gpio_request(pdata->gpio_pda_active, "PDA_ACTIVE");
	if (ret)
		mif_err("fail to request gpio %s:%d\n", "PDA_ACTIVE", ret);
	gpio_direction_output(pdata->gpio_pda_active, 0);

	/* GPIO_PHONE_ACTIVE */
	pdata->gpio_phone_active = of_get_named_gpio(np,
						"mif,gpio_phone_active", 0);
	if (!gpio_is_valid(pdata->gpio_phone_active)) {
		mif_err("phone_active: Invalied gpio pins\n");
		return -EINVAL;
	}

	mif_err("gpio_phone_active: %d\n", pdata->gpio_phone_active);
	ret = gpio_request(pdata->gpio_phone_active, "PHONE_ACTIVE");
	if (ret)
		mif_err("fail to request gpio %s:%d\n", "PHONE_ACTIVE", ret);
	gpio_direction_input(pdata->gpio_phone_active);

	return ret;
}

static void __parse_dt_optional_gpio_pdata(struct device_node *np,
					   struct modem_data *pdata)
{
	int ret;

	/* GPIO_CP_OFF */
	pdata->gpio_cp_off = of_get_named_gpio(np,
						"mif,gpio_cp_off", 0);
	if (gpio_is_valid(pdata->gpio_cp_off)) {
		mif_err("gpio_cp_off: %d\n", pdata->gpio_cp_off);
		ret = gpio_request(pdata->gpio_cp_off, "CP_OFF");
		if (ret)
			mif_err("fail to request gpio %s:%d\n",
				"CP_OFF", ret);
		else
			gpio_direction_output(pdata->gpio_cp_off, 0);
	}

	/* GPIO_CP_WAKEUP */
	pdata->gpio_cp_wakeup = of_get_named_gpio(np,
						"mif,gpio_cp_wakeup", 0);
	if (gpio_is_valid(pdata->gpio_cp_wakeup)) {
		mif_err("gpio_cp_wakeup: %d\n", pdata->gpio_cp_wakeup);
		ret = gpio_request(pdata->gpio_cp_wakeup, "CP_WAKEUP");
		if (ret)
			mif_err("fail to request gpio %s:%d\n",
				"CP_WAKEUP", ret);
		else
			gpio_direction_output(pdata->gpio_cp_wakeup, 0);
	}

	/* GPIO_AP_STATUS */
	pdata->gpio_ap_status = of_get_named_gpio(np,
						"mif,gpio_ap_status", 0);
	if (gpio_is_valid(pdata->gpio_ap_status)) {
		mif_err("gpio_ap_status: %d\n", pdata->gpio_ap_status);
		ret = gpio_request(pdata->gpio_ap_status, "AP_STATUS");
		if (ret)
			mif_err("fail to request gpio %s:%d\n",
				"AP_STATUS", ret);
		else
			gpio_direction_output(pdata->gpio_ap_status, 0);
	}

	/* GPIO_AP_WAKEUP */
	pdata->gpio_ap_wakeup = of_get_named_gpio(np,
						"mif,gpio_ap_wakeup", 0);
	if (gpio_is_valid(pdata->gpio_ap_wakeup)) {
		mif_err("gpio_ap_wakeup: %d\n", pdata->gpio_ap_wakeup);
		ret = gpio_request(pdata->gpio_ap_wakeup, "AP_WAKEUP");
		if (ret)
			mif_err("fail to request gpio %s:%d\n",
				"AP_WAKEUP", ret);
		else
			gpio_direction_input(pdata->gpio_ap_wakeup);
	}

	/* GPIO_CP_STATUS */
	pdata->gpio_cp_status = of_get_named_gpio(np,
						"mif,gpio_cp_status", 0);
	if (gpio_is_valid(pdata->gpio_cp_status)) {
		mif_err("gpio_cp_status: %d\n", pdata->gpio_cp_status);
		ret = gpio_request(pdata->gpio_cp_status, "CP_STATUS");
		if (ret)
			mif_err("fail to request gpio %s:%d\n",
				"CP_STATUS", ret);
		else
			gpio_direction_input(pdata->gpio_cp_status);
	}
}

static int parse_dt_gpio_pdata(struct device_node *np, struct modem_data *pdata)
{
	int err = 0;

	err = __parse_dt_mandatory_gpio_pdata(np, pdata);
	if (err)
		return err;

	__parse_dt_optional_gpio_pdata(np, pdata);
	return err;
}

static int parse_dt_iodevs_pdata(struct device *dev, struct device_node *np,
				 struct modem_data *pdata)
{
	struct device_node *child = NULL;
	struct modem_io_t *iod = NULL;
	size_t size = sizeof(struct modem_io_t) * pdata->num_iodevs;
	int idx = 0;
	u32 val;

	pdata->iodevs = devm_kzalloc(dev, size, GFP_KERNEL);
	if (!pdata->iodevs) {
		mif_err("iodevs: failed to alloc memory\n");
		return -ENOMEM;
	}

	for_each_child_of_node(np, child) {
		iod = &pdata->iodevs[idx];

		mif_dt_read_string(child, "iod,name", iod->name);
		mif_dt_read_u32(child, "iod,id", iod->id);
		mif_dt_read_enum(child, "iod,format", iod->format);
		mif_dt_read_enum(child, "iod,io_type", iod->io_type);
		mif_dt_read_enum(child, "iod,links", iod->links);
		idx++;
	}

	return 0;
}

static struct modem_data *modem_if_parse_dt_pdata(struct device *dev)
{
	struct modem_data *pdata;
	struct device_node *iodevs = NULL;

	pdata = devm_kzalloc(dev, sizeof(struct modem_data), GFP_KERNEL);
	if (!pdata) {
		mif_err("modem_data: alloc fail\n");
		return ERR_PTR(-ENOMEM);
	}

	if (parse_dt_common_pdata(dev->of_node, pdata)) {
		mif_err("DT error: failed to parse common\n");
		goto error;
	}

	if (parse_dt_gpio_pdata(dev->of_node, pdata)) {
		mif_err("DT error: failed to parse gpio\n");
		goto error;
	}

	iodevs = of_get_child_by_name(dev->of_node, "iodevs");
	if (!iodevs) {
		mif_err("DT error: failed to get child node\n");
		goto error;
	}

	if (parse_dt_iodevs_pdata(dev, iodevs, pdata)) {
		mif_err("DT error: failed to parse iodevs\n");
		goto error;
	}

	dev->platform_data = pdata;
	mif_info("DT parse complete!\n");
	return pdata;

error:
	if (!pdata) {
		if (!pdata->iodevs)
			devm_kfree(dev, pdata->iodevs);

		devm_kfree(dev, pdata);
	}
	return ERR_PTR(-EINVAL);
}

static const struct of_device_id sec_modem_match[] = {
	{ .compatible = "sec_modem,modem_pdata", },
	{},
};
MODULE_DEVICE_TABLE(of, sec_modem_match);
#else
static struct modem_data *modem_if_parse_dt_pdata(struct device *dev)
{
	return ERR_PTR(-ENODEV);
}
#endif

static int modem_probe(struct platform_device *pdev)
{
	int i;
	struct device *dev = &pdev->dev;
	struct modem_data *pdata = dev->platform_data;
	struct modem_shared *msd;
	struct modem_ctl *modemctl;
	struct io_device **iod;
	unsigned size;
	struct link_device *ld;
	mif_err("%s: +++\n", pdev->name);

	if (dev->of_node) {
		pdata = modem_if_parse_dt_pdata(dev);
		if (IS_ERR(pdata)) {
			mif_err("MIF DT parse error!\n");
			return PTR_ERR(pdata);
		}
	} else {
		if (!pdata) {
			mif_err("Non-DT, incorrect pdata!\n");
			return -EINVAL;
		}
	}

	msd = create_modem_shared_data(pdev);
	if (!msd) {
		mif_err("%s: msd == NULL\n", pdata->name);
		return -ENOMEM;
	}

	modemctl = create_modemctl_device(pdev, msd);
	if (!modemctl) {
		mif_err("%s: modemctl == NULL\n", pdata->name);
		devm_kfree(dev, msd);
		return -ENOMEM;
	}

	/* create link device */
	/* support multi-link device */
	for (i = 0; i < LINKDEV_MAX ; i++) {
		/* find matching link type */
		if (pdata->link_types & LINKTYPE(i)) {
			ld = call_link_init_func(pdev, i);
			if (!ld)
				goto free_mc;

			mif_err("%s: %s link created\n", pdata->name, ld->name);
			ld->link_type = i;
			ld->mc = modemctl;
			ld->msd = msd;
			list_add(&ld->list, &msd->link_dev_list);
		}
	}

	/* create io deivces and connect to modemctl device */
	size = sizeof(struct io_device *) * pdata->num_iodevs;
	iod = (struct io_device **)kzalloc(size, GFP_KERNEL);
	for (i = 0; i < pdata->num_iodevs; i++) {
		iod[i] = create_io_device(pdev, &pdata->iodevs[i], msd,
					  modemctl, pdata);
		if (!iod[i]) {
			mif_err("%s: iod[%d] == NULL\n", pdata->name, i);
			goto free_iod;
		}

		attach_devices(iod[i], pdata->iodevs[i].tx_link);
	}

	platform_set_drvdata(pdev, modemctl);

	kfree(iod);

	mif_err("%s: ---\n", pdev->name);
	return 0;

free_iod:
	for (i = 0; i < pdata->num_iodevs; i++) {
		if (iod[i])
			devm_kfree(dev, iod[i]);
	}
	kfree(iod);

free_mc:
	if (modemctl)
		devm_kfree(dev, modemctl);

	if (msd)
		devm_kfree(dev, msd);

	mif_err("%s: xxx\n", pdev->name);
	return -ENOMEM;
}

static void modem_shutdown(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct modem_ctl *mc = dev_get_drvdata(dev);
	struct utc_time utc;

	mc->ops.modem_off(mc);
	mc->phone_state = STATE_OFFLINE;

	get_utc_time(&utc);
	mif_info("%s: at [%02d:%02d:%02d.%03d]\n",
		mc->name, utc.hour, utc.min, utc.sec, utc.msec);
}

static int modem_suspend(struct device *pdev)
{
#if !defined(CONFIG_LINK_DEVICE_HSIC)
	struct modem_ctl *mc = dev_get_drvdata(pdev);

	if (mc->gpio_pda_active)
		gpio_set_value(mc->gpio_pda_active, 0);
#endif

	return 0;
}

static int modem_resume(struct device *pdev)
{
#if !defined(CONFIG_LINK_DEVICE_HSIC)
	struct modem_ctl *mc = dev_get_drvdata(pdev);

	if (mc->gpio_pda_active)
		gpio_set_value(mc->gpio_pda_active, 1);
#endif

	return 0;
}

static const struct dev_pm_ops modem_pm_ops = {
	.suspend = modem_suspend,
	.resume = modem_resume,
};

static struct platform_driver modem_driver = {
	.probe = modem_probe,
	.shutdown = modem_shutdown,
	.driver = {
		.name = "mif_sipc5",
		.pm = &modem_pm_ops,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(sec_modem_match),
#endif
	},
};

static int __init modem_init(void)
{
	return platform_driver_register(&modem_driver);
}

module_init(modem_init);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Samsung Modem Interface Driver");
