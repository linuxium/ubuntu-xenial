/*
 *  bytcr_aic3100.c - ASoc Machine driver for Intel Byt CR platform
 *
 *  Copyright (C) 2016 Intel Corporation
 *
 *  Author: Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>
 *  Based on earlier 3.14 work by Praveen Diwakar and Gurudatta Bhakte
 *
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/acpi.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/slab.h>
#include <asm/cpu_device_id.h>
#include <asm/platform_sst_audio.h>
#include <linux/clk.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/jack.h>
#include "../../codecs/tlv320aic31xx.h"
#include "../atom/sst-atom-controls.h"
#include "../common/sst-acpi.h"
#include "../common/sst-dsp.h"

enum {
	BYT_AIC3100_IN1_MAP,
};

#define BYT_AIC3100_MAP(quirk)	((quirk) & 0xff)
#define BYT_AIC3100_SSP2_AIF    BIT(16) /* default true, use SSP2 */
#define BYT_AIC3100_MCLK_25MHZ	BIT(18) /* default false, use 19.2MHz */

struct byt_aic3100_private {
	struct clk *mclk;
};

static unsigned long byt_aic3100_quirk =
	BYT_AIC3100_IN1_MAP | BYT_AIC3100_SSP2_AIF;

static void log_quirks(struct device *dev)
{
	if (BYT_AIC3100_MAP(byt_aic3100_quirk) == BYT_AIC3100_IN1_MAP)
		dev_info(dev, "quirk IN1_MAP enabled");
	if (byt_aic3100_quirk & BYT_AIC3100_SSP2_AIF)
		dev_info(dev, "quirk SSP2_AIF enabled");
	else
		dev_info(dev, "quirk SSP0_AIF enabled");
	if (byt_aic3100_quirk & BYT_AIC3100_MCLK_25MHZ)
		dev_info(dev, "quirk MCLK_25MHZ enabled");
	else
		dev_info(dev, "quirk MCLK_19200kHZ enabled");
}

#define BYT_CODEC_DAI1	"tlv320aic31xx-hifi"

static inline struct snd_soc_dai *byt_get_codec_dai(struct snd_soc_card *card)
{
	struct snd_soc_pcm_runtime *rtd;

	list_for_each_entry(rtd, &card->rtd_list, list) {
		if (!strncmp(rtd->codec_dai->name, BYT_CODEC_DAI1,
			     strlen(BYT_CODEC_DAI1)))
			return rtd->codec_dai;
	}
	return NULL;
}

static int platform_clock_control(struct snd_soc_dapm_widget *w,
				  struct snd_kcontrol *k, int  event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct snd_soc_dai *codec_dai;
	struct byt_aic3100_private *priv = snd_soc_card_get_drvdata(card);
	int ret;

	codec_dai = byt_get_codec_dai(card);
	if (!codec_dai) {
		dev_err(card->dev,
			"Codec dai not found; Unable to set platform clock\n");
		return -EIO;
	}

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if (priv->mclk) {
			ret = clk_prepare_enable(priv->mclk);
			if (ret < 0) {
				dev_err(card->dev,
					"could not configure MCLK state");
				return ret;
			}
		}

		if (byt_aic3100_quirk & BYT_AIC3100_MCLK_25MHZ) {
			ret = snd_soc_dai_set_sysclk(codec_dai,
						AIC31XX_PLL_CLKIN_MCLK,
						25000000,
						SND_SOC_CLOCK_IN);
		} else {
			ret = snd_soc_dai_set_sysclk(codec_dai,
						AIC31XX_PLL_CLKIN_MCLK,
						19200000,
						SND_SOC_CLOCK_IN);
		}
	} else {
		/*
		 * Set codec clock source to internal clock before
		 * turning off the platform clock. Codec needs clock
		 * for Jack detection and button press
		 */
		/* FIXME: use RC oscillator? */
		ret = snd_soc_dai_set_sysclk(codec_dai, AIC31XX_PLL_CLKIN_MCLK,
					0, SND_SOC_CLOCK_IN);
		if (!ret) {
			if (priv->mclk)
				clk_disable_unprepare(priv->mclk);
		}
	}

	if (ret < 0) {
		dev_err(card->dev, "can't set codec sysclk: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct snd_soc_dapm_widget byt_aic3100_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Internal Mic", NULL),
	SND_SOC_DAPM_SPK("Speakers", NULL),
	SND_SOC_DAPM_SUPPLY("Platform Clock", SND_SOC_NOPM, 0, 0,
			    platform_clock_control, SND_SOC_DAPM_PRE_PMU |
			    SND_SOC_DAPM_POST_PMD),

};

static const struct snd_soc_dapm_route byt_aic3100_audio_map[] = {
	/* External Speakers: HFL, HFR */
	{"Speakers", NULL, "SPK"},

	/* Headset Stereophone(Headphone): HSOL, HSOR */
	{"Headphone", NULL, "HPL"},
	{"Headphone", NULL, "HPR"},

	{"Headphone", NULL, "Platform Clock"},
	{"Headset Mic", NULL, "Platform Clock"},
	{"Internal Mic", NULL, "Platform Clock"},
	{"Speaker", NULL, "Platform Clock"},

};

static const struct snd_soc_dapm_route byt_aic3100_intmic_in1_map[] = {
	{"micbias", NULL, "Internal Mic"},
	/* Headset Mic: Headset Mic with bias */
	{"MIC1RP", NULL, "Headset Mic"},
};

static const struct snd_soc_dapm_route byt_aic3100_ssp2_aif_map[] = {
	{"ssp2 Tx", NULL, "codec_out0"},
	{"ssp2 Tx", NULL, "codec_out1"},
	{"codec_in0", NULL, "ssp2 Rx"},
	{"codec_in1", NULL, "ssp2 Rx"},

	{"Playback", NULL, "ssp2 Tx"},
	{"ssp2 Rx", NULL, "Capture"},
};

static const struct snd_soc_dapm_route byt_aic3100_ssp0_aif_map[] = {
	{"ssp0 Tx", NULL, "modem_out"},
	{"modem_in", NULL, "ssp0 Rx"},

	{"Playback", NULL, "ssp0 Tx"},
	{"ssp0 Rx", NULL, "Capture"},
};

static const struct snd_kcontrol_new byt_aic3100_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Internal Mic"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
};

static int byt_aic3100_aif1_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;

	if (byt_aic3100_quirk & BYT_AIC3100_MCLK_25MHZ) {
		ret = snd_soc_dai_set_sysclk(codec_dai, AIC31XX_PLL_CLKIN_MCLK,
					25000000,
					SND_SOC_CLOCK_IN);
	} else {
		ret = snd_soc_dai_set_sysclk(codec_dai, AIC31XX_PLL_CLKIN_MCLK,
					19200000,
					SND_SOC_CLOCK_IN);
	}

	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec clock %d\n", ret);
		return ret;
	}

	if (byt_aic3100_quirk & BYT_AIC3100_MCLK_25MHZ) {
		ret = snd_soc_dai_set_pll(codec_dai, 0, AIC31XX_PLL_CLKIN_MCLK,
					250000000, params_rate(params));
	} else {
		ret = snd_soc_dai_set_pll(codec_dai, 0, AIC31XX_PLL_CLKIN_MCLK,
					19200000, params_rate(params));

	}

	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec pll: %d\n", ret);
		return ret;
	}

	return 0;
}

/* uncomment when we have an actual quirk
static int byt_aic3100_quirk_cb(const struct dmi_system_id *id)
{
	byt_aic3100_quirk = (unsigned long)id->driver_data;
	return 1;
}
*/

static const struct dmi_system_id byt_aic3100_quirk_table[] = {
	{}
};

static int byt_aic3100_init(struct snd_soc_pcm_runtime *runtime)
{
	int ret;
	struct snd_soc_card *card = runtime->card;
	const struct snd_soc_dapm_route *custom_map;
	struct byt_aic3100_private *priv = snd_soc_card_get_drvdata(card);
	int num_routes;

	card->dapm.idle_bias_off = true;

	ret = snd_soc_add_card_controls(card, byt_aic3100_controls,
					ARRAY_SIZE(byt_aic3100_controls));
	if (ret) {
		dev_err(card->dev, "unable to add card controls\n");
		return ret;
	}

	switch (BYT_AIC3100_MAP(byt_aic3100_quirk)) {
	case BYT_AIC3100_IN1_MAP:
	default:
		custom_map = byt_aic3100_intmic_in1_map;
		num_routes = ARRAY_SIZE(byt_aic3100_intmic_in1_map);
	}

	ret = snd_soc_dapm_add_routes(&card->dapm, custom_map, num_routes);
	if (ret)
		return ret;

	if (byt_aic3100_quirk & BYT_AIC3100_SSP2_AIF) {
		ret = snd_soc_dapm_add_routes(&card->dapm,
					byt_aic3100_ssp2_aif_map,
					ARRAY_SIZE(byt_aic3100_ssp2_aif_map));
	} else {
		ret = snd_soc_dapm_add_routes(&card->dapm,
					byt_aic3100_ssp0_aif_map,
					ARRAY_SIZE(byt_aic3100_ssp0_aif_map));
	}
	if (ret)
		return ret;

	snd_soc_dapm_ignore_suspend(&card->dapm, "Headphone");
	snd_soc_dapm_ignore_suspend(&card->dapm, "Speaker");

	if (priv->mclk) {
		/*
		 * The firmware might enable the clock at
		 * boot (this information may or may not
		 * be reflected in the enable clock register).
		 * To change the rate we must disable the clock
		 * first to cover these cases. Due to common
		 * clock framework restrictions that do not allow
		 * to disable a clock that has not been enabled,
		 * we need to enable the clock first.
		 */
		ret = clk_prepare_enable(priv->mclk);
		if (!ret)
			clk_disable_unprepare(priv->mclk);

		if (byt_aic3100_quirk & BYT_AIC3100_MCLK_25MHZ)
			ret = clk_set_rate(priv->mclk, 25000000);
		else
			ret = clk_set_rate(priv->mclk, 19200000);

		if (ret)
			dev_err(card->dev, "unable to set MCLK rate\n");
	}

	return ret;
}

static const struct snd_soc_pcm_stream byt_aic3100_dai_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
};

static int byt_aic3100_codec_fixup(struct snd_soc_pcm_runtime *rtd,
			    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	int ret;

	/* The DSP will covert the FE rate to 48k, stereo */
	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;

	if (!(byt_aic3100_quirk & BYT_AIC3100_SSP2_AIF)) {

		/* set SSP0 to 16-bit */
		params_set_format(params, SNDRV_PCM_FORMAT_S16_LE);

		/*
		 * Default mode for SSP configuration is TDM 4 slot,
		 * override config with explicit setting to I2S 2ch 16-bit.
		 * The word length is set with dai_set_tdm_slot() since there
		 * is no other API exposed
		 */
		ret = snd_soc_dai_set_fmt(rtd->cpu_dai,
					SND_SOC_DAIFMT_I2S     |
					SND_SOC_DAIFMT_NB_NF   |
					SND_SOC_DAIFMT_CBS_CFS
			);
		if (ret < 0) {
			dev_err(rtd->dev,
				"can't set format to I2S, err %d\n", ret);
			return ret;
		}

		ret = snd_soc_dai_set_tdm_slot(rtd->cpu_dai, 0x3, 0x3, 2, 16);
		if (ret < 0) {
			dev_err(rtd->dev,
				"can't set I2S config, err %d\n", ret);
			return ret;
		}

	} else {

		/* set SSP2 to 24-bit */
		params_set_format(params, SNDRV_PCM_FORMAT_S24_LE);

		/*
		 * Default mode for SSP configuration is TDM 4 slot,
		 * override config with explicit setting to I2S 2ch 24-bit.
		 * The word length is set with dai_set_tdm_slot() since
		 * there is no other API exposed
		 */
		ret = snd_soc_dai_set_fmt(rtd->cpu_dai,
					SND_SOC_DAIFMT_I2S     |
					SND_SOC_DAIFMT_NB_NF   |
					SND_SOC_DAIFMT_CBS_CFS
			);
		if (ret < 0) {
			dev_err(rtd->dev,
				"can't set format to I2S, err %d\n", ret);
			return ret;
		}

		ret = snd_soc_dai_set_tdm_slot(rtd->cpu_dai, 0x3, 0x3, 2, 24);
		if (ret < 0) {
			dev_err(rtd->dev,
				"can't set I2S config, err %d\n", ret);
			return ret;
		}
	}
	return 0;
}

static int byt_aic3100_aif1_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_single(substream->runtime,
			SNDRV_PCM_HW_PARAM_RATE, 48000);
}

static struct snd_soc_ops byt_aic3100_aif1_ops = {
	.startup = byt_aic3100_aif1_startup,
};

static struct snd_soc_ops byt_aic3100_be_ssp2_ops = {
	.hw_params = byt_aic3100_aif1_hw_params,
};

static struct snd_soc_dai_link byt_aic3100_dais[] = {
	[MERR_DPCM_AUDIO] = {
		.name = "Baytrail Audio Port",
		.stream_name = "Baytrail Audio",
		.cpu_dai_name = "media-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-mfld-platform",
		.ignore_suspend = 1,
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ops = &byt_aic3100_aif1_ops,
	},
	[MERR_DPCM_DEEP_BUFFER] = {
		.name = "Deep-Buffer Audio Port",
		.stream_name = "Deep-Buffer Audio",
		.cpu_dai_name = "deepbuffer-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-mfld-platform",
		.ignore_suspend = 1,
		.nonatomic = true,
		.dynamic = 1,
		.dpcm_playback = 1,
		.ops = &byt_aic3100_aif1_ops,
	},
	[MERR_DPCM_COMPR] = {
		.name = "Baytrail Compressed Port",
		.stream_name = "Baytrail Compress",
		.cpu_dai_name = "compress-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-mfld-platform",
	},
		/* back ends */
	{
		.name = "SSP2-Codec",
		.id = 1,
		.cpu_dai_name = "ssp2-port", /* overwritten for ssp0 routing */
		.platform_name = "sst-mfld-platform",
		.no_pcm = 1,
		.codec_dai_name = "tlv320aic31xx-hifi",
		.codec_name = "i2c-10TI3100:00", /* overwritten with HID */
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
						| SND_SOC_DAIFMT_CBS_CFS,
		.be_hw_params_fixup = byt_aic3100_codec_fixup,
		.ignore_suspend = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.init = byt_aic3100_init,
		.ops = &byt_aic3100_be_ssp2_ops,
	},
};

/* SoC card */
static struct snd_soc_card byt_aic3100_card = {
	.name = "bytcr-aic3100",
	.owner = THIS_MODULE,
	.dai_link = byt_aic3100_dais,
	.num_links = ARRAY_SIZE(byt_aic3100_dais),
	.dapm_widgets = byt_aic3100_widgets,
	.num_dapm_widgets = ARRAY_SIZE(byt_aic3100_widgets),
	.dapm_routes = byt_aic3100_audio_map,
	.num_dapm_routes = ARRAY_SIZE(byt_aic3100_audio_map),
	.fully_routed = true,
};

static char byt_aic3100_codec_name[16];/* i2c-<HID>:00 with HID being 8 chars */
static char byt_aic3100_cpu_dai_name[10]; /*  = "ssp[0|2]-port" */

static bool is_valleyview(void)
{
	static const struct x86_cpu_id cpu_ids[] = {
		{ X86_VENDOR_INTEL, 6, 55 }, /* Valleyview, Bay Trail */
		{}
	};

	if (!x86_match_cpu(cpu_ids))
		return false;
	return true;
}


static int snd_byt_aic3100_mc_probe(struct platform_device *pdev)
{
	int ret_val = 0;
	struct sst_acpi_mach *mach;
	const char *i2c_name = NULL;
	int i;
	int dai_index;
	struct byt_aic3100_private *priv;
	bool is_bytcr = false;

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_ATOMIC);
	if (!priv)
		return -ENOMEM;

	/* register the soc card */
	byt_aic3100_card.dev = &pdev->dev;
	mach = byt_aic3100_card.dev->platform_data;
	snd_soc_card_set_drvdata(&byt_aic3100_card, priv);

	/* fix index of codec dai */
	dai_index = MERR_DPCM_COMPR + 1;
	for (i = 0; i < ARRAY_SIZE(byt_aic3100_dais); i++) {
		if (!strcmp(byt_aic3100_dais[i].codec_name,
				"i2c-10TI3100:00")) {
			dai_index = i;
			break;
		}
	}

	/* fixup codec name based on HID */
	i2c_name = sst_acpi_find_name_from_hid(mach->id);
	if (i2c_name != NULL) {
		snprintf(byt_aic3100_codec_name, sizeof(byt_aic3100_codec_name),
			"%s%s", "i2c-", i2c_name);

		byt_aic3100_dais[dai_index].codec_name = byt_aic3100_codec_name;
	}

	/*
	 * swap SSP0 if bytcr is detected
	 * (will be overridden if DMI quirk is detected)
	 */
	if (is_valleyview()) {
		struct sst_platform_info *p_info = mach->pdata;
		const struct sst_res_info *res_info = p_info->res_info;

		if (res_info->acpi_ipc_irq_index == 0) {
			is_bytcr = true;
			byt_aic3100_quirk &= ~BYT_AIC3100_SSP2_AIF;
		}
	}

	/* check quirks before creating card */
	dmi_check_system(byt_aic3100_quirk_table);
	log_quirks(&pdev->dev);

	if (!(byt_aic3100_quirk & BYT_AIC3100_SSP2_AIF))  {

		/* fixup cpu dai name name */
		snprintf(byt_aic3100_cpu_dai_name,
			sizeof(byt_aic3100_cpu_dai_name),
			"%s", "ssp0-port");

		byt_aic3100_dais[dai_index].cpu_dai_name =
			byt_aic3100_cpu_dai_name;
	}

	if (is_valleyview()) {
		priv->mclk = devm_clk_get(&pdev->dev, "pmc_plt_clk_3");
		if (IS_ERR(priv->mclk)) {
			dev_err(&pdev->dev,
				"Failed to get MCLK from pmc_plt_clk_3: %ld\n",
				PTR_ERR(priv->mclk));

			/*
			 * Audio output only works with MCLK enabled
			 */
			return PTR_ERR(priv->mclk);
		}
	}

	ret_val = devm_snd_soc_register_card(&pdev->dev, &byt_aic3100_card);

	if (ret_val) {
		dev_err(&pdev->dev, "devm_snd_soc_register_card failed %d\n",
			ret_val);
		return ret_val;
	}
	platform_set_drvdata(pdev, &byt_aic3100_card);
	return ret_val;
}

static struct platform_driver snd_byt_aic3100_mc_driver = {
	.driver = {
		.name = "bytcr_aic3100",
	},
	.probe = snd_byt_aic3100_mc_probe,
};

module_platform_driver(snd_byt_aic3100_mc_driver);

MODULE_DESCRIPTION("ASoC Intel(R) Baytrail CR Machine driver");
MODULE_AUTHOR("Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:bytcr_aic3100");
