/*
 * virtual GPIO IRQ#9 workaround for whacky Cherrytrail devices
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/dmi.h>

#define CHT_VGPIO_DRIVER_NAME		"cht-vgpio"
#define CHT_VGPIO_IRQ			9

#define GPE0A_PME_STS_BIT               0x2000
#define GPE0A_PME_EN_BIT                0x2000
#define GPE0A_STS_PORT			0x420
#define GPE0A_EN_PORT			0x428

struct cht_vgpio_data {
	struct work_struct work;
	struct mutex lock;
};

static irqreturn_t cht_vgpio_irq_handler(int irq, void *data)
{
	struct cht_vgpio_data *gd = data;

	schedule_work(&gd->work);
	return IRQ_HANDLED;
}

static void cht_vgpio_work(struct work_struct *work)
{
	struct cht_vgpio_data *gd =
		container_of(work, struct cht_vgpio_data, work);
	u32 gpe_sts_reg;
	u32 gpe_en_reg;

	mutex_lock(&gd->lock);
	gpe_sts_reg = inl(GPE0A_STS_PORT);
	gpe_en_reg = inl(GPE0A_EN_PORT);
	/* Clear the STS Bit */
	if (gpe_en_reg & GPE0A_PME_EN_BIT)
		outl(gpe_en_reg & ~GPE0A_PME_EN_BIT, GPE0A_EN_PORT);
	if (gpe_sts_reg & GPE0A_PME_STS_BIT)
		outl(GPE0A_PME_STS_BIT, GPE0A_STS_PORT);
	if (gpe_en_reg & GPE0A_PME_EN_BIT)
		outl(gpe_en_reg, GPE0A_EN_PORT);
	mutex_unlock(&gd->lock);
}

static int cht_vgpio_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cht_vgpio_data *gd;
	int err;

	gd = devm_kzalloc(dev, sizeof(*gd), GFP_KERNEL);
	if (!gd)
		return -ENOMEM;

	mutex_init(&gd->lock);
	INIT_WORK(&gd->work, cht_vgpio_work);
	platform_set_drvdata(pdev, gd);

	dev_info(dev, "Assigning IRQ %d\n", CHT_VGPIO_IRQ);
	err = devm_request_irq(dev, CHT_VGPIO_IRQ, cht_vgpio_irq_handler,
			       IRQF_SHARED, CHT_VGPIO_DRIVER_NAME, gd);
	if (err)
		return err;

	outl(inl(GPE0A_EN_PORT) | GPE0A_PME_EN_BIT, GPE0A_EN_PORT);
	return 0;
}

static int cht_vgpio_remove(struct platform_device *pdev)
{
	struct cht_vgpio_data *gd = platform_get_drvdata(pdev);

	cancel_work_sync(&gd->work);
	outl(inl(GPE0A_EN_PORT) & ~GPE0A_PME_EN_BIT, GPE0A_EN_PORT);
	return 0;
}

static struct platform_driver cht_vgpio_driver = {
	.driver = {
		.name = CHT_VGPIO_DRIVER_NAME,
	},
	.probe	= cht_vgpio_probe,
	.remove	= cht_vgpio_remove,
};

static struct dmi_system_id cht_vgpio_dmi_table[] __initdata = {
	{
		.ident = "Dell Wyse 3040",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "Dell Inc."),
			DMI_MATCH(DMI_PRODUCT_NAME, "Wyse 3040"),
		},
	},
	{
		.ident = "ASUS E200HA",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME, "E200HA"),
		},
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, cht_vgpio_dmi_table);

static struct platform_device *cht_dev;

static int __init cht_vgpio_driver_init(void)
{
	int err;

	if (!dmi_check_system(cht_vgpio_dmi_table))
		return -ENODEV;
	err = platform_driver_register(&cht_vgpio_driver);
	if (err < 0)
		return err;
	cht_dev = platform_device_register_simple(CHT_VGPIO_DRIVER_NAME,
						   0, NULL, 0);
	if (IS_ERR(cht_dev)) {
		platform_driver_unregister(&cht_vgpio_driver);
		return PTR_ERR(cht_dev);
	}

	return 0;
}

static void __exit cht_vgpio_driver_exit(void)
{
	platform_device_unregister(cht_dev);
	platform_driver_unregister(&cht_vgpio_driver);
}

module_init(cht_vgpio_driver_init);
module_exit(cht_vgpio_driver_exit);

MODULE_LICENSE("GPL v2");
