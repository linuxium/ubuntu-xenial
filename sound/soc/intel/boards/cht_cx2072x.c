/*
 *  cht_cx207x.c - ASoc DPCM Machine driver for CherryTrail w/ CX2072x
 *
 *  Copyright (C) 2016 Intel Corp
 *  Author: Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>
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
#include "../../codecs/cx2072x.h"
#include "../atom/sst-atom-controls.h"
#include "../common/sst-acpi.h"


#define CHT_PLAT_CLK_3_HZ	19200000

enum {
	CHT_CX_DMIC_MAP,
};

#define CHT_CX_MAP(quirk)	((quirk) & 0xff)
#define CHT_CX_MCLK_EN	BIT(16)


struct cht_mc_private {
	struct clk *mclk;
};

static unsigned long cht_cx_quirk = CHT_CX_MCLK_EN;

#define BYT_CODEC_DAI1	"cx2072x-hifi"

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
	struct cht_mc_private *priv = snd_soc_card_get_drvdata(card);
	int ret;

	codec_dai = byt_get_codec_dai(card);
	if (!codec_dai) {
		dev_err(card->dev,
			"Codec dai not found; Unable to set platform clock\n");
		return -EIO;
	}

	if (SND_SOC_DAPM_EVENT_ON(event)) {
		if ((cht_cx_quirk & CHT_CX_MCLK_EN) && priv->mclk) {
			ret = clk_prepare_enable(priv->mclk);
			if (ret < 0) {
				dev_err(card->dev,
					"could not configure MCLK state");
				return ret;
			}
		}
		ret = snd_soc_dai_set_sysclk(codec_dai, CX2072X_MCLK_EXTERNAL_PLL,
				     19200000, SND_SOC_CLOCK_IN);
	} else {
		/*
		 * Set codec clock source to internal clock before
		 * turning off the platform clock. Codec needs clock
		 * for Jack detection and button press
		 */

		/* FIXME: what is the internal setting, if any? */
		ret = snd_soc_dai_set_sysclk(codec_dai, CX2072X_MCLK_EXTERNAL_PLL,
				     19200000, SND_SOC_CLOCK_IN);

		if (!ret) {
			if ((cht_cx_quirk & CHT_CX_MCLK_EN) && priv->mclk)
				clk_disable_unprepare(priv->mclk);
		}
	}

	if (ret < 0) {
		dev_err(card->dev, "can't set codec sysclk: %d\n", ret);
		return ret;
	}

	return 0;
}

static const struct snd_soc_dapm_widget cht_dapm_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Int Mic", NULL),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_SUPPLY("Platform Clock", SND_SOC_NOPM, 0, 0,
			platform_clock_control, SND_SOC_DAPM_PRE_PMU |
			SND_SOC_DAPM_POST_PMD),
};

static const struct snd_soc_dapm_route cht_audio_map[] = {
		/* External Speakers: HFL, HFR */
		{"Headphone", NULL, "PORTA"},
		{"Ext Spk", NULL, "PORTG"},
		{"PORTC", NULL, "Int Mic"},
		/* Headset Mic: Headset Mic with bias */
		{"PORTD", NULL, "Headset Mic"},
		/*{"Headset Bias", NULL, "Headset Mic"},*/
		/*{"PortD Mic Bias", NULL, "Headset Mic"},*/

		/* Headset Stereophone(Headphone): HSOL, HSOR */
		//{"Headphone", NULL, "HPL"},
		//{"Headphone", NULL, "HPR"},

		{"Playback", NULL, "ssp2 Tx"},
		{"ssp2 Tx", NULL, "codec_out0"},
		{"ssp2 Tx", NULL, "codec_out1"},
		{"codec_in0", NULL, "ssp2 Rx"},
		{"codec_in1", NULL, "ssp2 Rx"},
		{"ssp2 Rx", NULL, "Capture"},
		{"ssp0 Tx", NULL, "modem_out"},
		{"modem_in", NULL, "ssp0 Rx"},
		{"Playback", NULL, "Platform Clock"},
		{"Capture", NULL, "Platform Clock"},
};

static const struct snd_kcontrol_new cht_mc_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_DAPM_PIN_SWITCH("Int Mic"),
	SOC_DAPM_PIN_SWITCH("Ext Spk"),
};

static int cht_aif1_hw_params(struct snd_pcm_substream *substream,
			     struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;
	
	ret = snd_soc_dai_set_sysclk(codec_dai, CX2072X_MCLK_EXTERNAL_PLL,
				     19200000, SND_SOC_CLOCK_IN);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec sysclk: %d\n", ret);
		return ret;
	}

	/* FIXME: No need PLL for conexant codec */
#if 0
	ret = snd_soc_dai_set_pll(codec_dai, 0, RT5670_PLL1_S_MCLK,
				  CHT_PLAT_CLK_3_HZ, rate * 512);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set codec pll: %d\n", ret);
		return ret;
	}
#endif


	return 0;
}

static int cht_codec_init(struct snd_soc_pcm_runtime *runtime)
{
	int ret=0;
	struct snd_soc_card *card = runtime->card;
	struct cht_mc_private *priv = snd_soc_card_get_drvdata(runtime->card);

	card->dapm.idle_bias_off = true;

	/* FIXME: is this necessary */
//      snd_soc_dapm_ignore_suspend(&codec->dapm, "PortD");
//	snd_soc_dapm_enable_pin(&card->dapm, "Headset Mic");
//	snd_soc_dapm_enable_pin(&card->dapm, "Headphone");
//	snd_soc_dapm_enable_pin(&card->dapm, "Ext Spk");
//	snd_soc_dapm_enable_pin(&card->dapm, "Int Mic");

//	snd_soc_dapm_sync(&card->dapm);

	if ((cht_cx_quirk & CHT_CX_MCLK_EN) && priv->mclk) {
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

		ret = clk_set_rate(priv->mclk, 19200000);

		if (ret)
			dev_err(card->dev, "unable to set MCLK rate\n");
	}

	return ret;
}

static const struct snd_soc_pcm_stream cht_dai_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
};

static int cht_codec_fixup(struct snd_soc_pcm_runtime *rtd,
			    struct snd_pcm_hw_params *params)
{
	struct snd_interval *rate = hw_param_interval(params,
			SNDRV_PCM_HW_PARAM_RATE);
	struct snd_interval *channels = hw_param_interval(params,
						SNDRV_PCM_HW_PARAM_CHANNELS);
	int ret;
	
	/* The DSP will covert the FE rate to 48k, stereo, 24bits */
	rate->min = rate->max = 48000;
	channels->min = channels->max = 2;

	/* set SSP2 to 24-bit */
	params_set_format(params, SNDRV_PCM_FORMAT_S24_LE);
	
	/*
	 * Default mode for SSP configuration is TDM 4 slot, override config
	 * with explicit setting to I2S 2ch 24-bit. The word length is set with
	 * dai_set_tdm_slot() since there is no other API exposed
	 */
	ret = snd_soc_dai_set_fmt(rtd->cpu_dai,
				SND_SOC_DAIFMT_I2S     |
				SND_SOC_DAIFMT_NB_NF   |
				SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0) {
		dev_err(rtd->dev, "can't set format to I2S, err %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_tdm_slot(rtd->cpu_dai, 0x3, 0x3, 2, 24);
	if (ret < 0) {
		dev_err(rtd->dev,
			"can't set I2S config, err %d\n", ret);
		return ret;
	}
	

	return 0;
}

static const struct snd_soc_pcm_stream cht_cx_dai_params = {
	.formats = SNDRV_PCM_FMTBIT_S24_LE,
	.rate_min = 48000,
	.rate_max = 48000,
	.channels_min = 2,
	.channels_max = 2,
};

static int cht_aif1_startup(struct snd_pcm_substream *substream)
{
	return snd_pcm_hw_constraint_single(substream->runtime,
					SNDRV_PCM_HW_PARAM_RATE, 48000);
}

static struct snd_soc_ops cht_aif1_ops = {
	.startup = cht_aif1_startup,
};

static struct snd_soc_ops cht_be_ssp2_ops = {
	.hw_params = cht_aif1_hw_params,
};

static struct snd_soc_dai_link cht_dailink[] = {
	[MERR_DPCM_AUDIO] = {
		.name = "Audio Port",
		.stream_name = "Audio",
		.cpu_dai_name = "media-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-mfld-platform",
		.nonatomic = true,
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ops = &cht_aif1_ops,
	},
	[MERR_DPCM_DEEP_BUFFER] = {
		.name = "Deep-Buffer Audio Port",
		.stream_name = "Deep-Buffer Audio",
		.cpu_dai_name = "deepbuffer-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-mfld-platform",
		.nonatomic = true,
		.dynamic = 1,
		.dpcm_playback = 1,
		.ops = &cht_aif1_ops,
	},
	[MERR_DPCM_COMPR] = {
		.name = "Compressed Port",
		.stream_name = "Compress",
		.cpu_dai_name = "compress-cpu-dai",
		.codec_dai_name = "snd-soc-dummy-dai",
		.codec_name = "snd-soc-dummy",
		.platform_name = "sst-mfld-platform",
	},
	/* CODEC<->CODEC link */
	/* back ends */
	{
		.name = "SSP2-Codec",
		.id = 1,
		.cpu_dai_name = "ssp2-port",
		.platform_name = "sst-mfld-platform",
		.no_pcm = 1,
		.codec_dai_name = "cx2072x-hifi",
		.codec_name = "i2c-14F10720:00",
		.dai_fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF
		                              | SND_SOC_DAIFMT_CBS_CFS,
		.init = cht_codec_init,
		.be_hw_params_fixup = cht_codec_fixup,
		.nonatomic = true,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ops = &cht_be_ssp2_ops,
	},
};

/* SoC card */
static struct snd_soc_card chtcx2072x_card = {
	.name = "chtcx2072x",
	.dai_link = cht_dailink,
	.num_links = ARRAY_SIZE(cht_dailink),
	.dapm_widgets = cht_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cht_dapm_widgets),
	.dapm_routes = cht_audio_map,
	.num_dapm_routes = ARRAY_SIZE(cht_audio_map),
	.controls = cht_mc_controls,
	.num_controls = ARRAY_SIZE(cht_mc_controls),
};

static char cht_cx_codec_name[16]; /* i2c-<HID>:00 with HID being 8 chars */

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

static int snd_cht_mc_probe(struct platform_device *pdev)
{
	int ret_val = 0;
	int i;
	int dai_index;
	struct cht_mc_private *drv;
	struct sst_acpi_mach *mach;
	const char *i2c_name = NULL;
	
	drv = devm_kzalloc(&pdev->dev, sizeof(*drv), GFP_ATOMIC);
	if (!drv) {
		pr_err("allocation failed\n");
		return -ENOMEM;
	}

	/* register the soc card */
	chtcx2072x_card.dev = &pdev->dev;
	mach = chtcx2072x_card.dev->platform_data;
	snd_soc_card_set_drvdata(&chtcx2072x_card, drv);
	
	/* fix index of codec dai */
	dai_index = MERR_DPCM_COMPR + 1;
	for (i = 0; i < ARRAY_SIZE(cht_dailink); i++) {
		if (!strcmp(cht_dailink[i].codec_name, "i2c-14F10720:00")) {
			dai_index = i;
			break;
		}
	}

	/* fixup codec name based on HID */
	i2c_name = sst_acpi_find_name_from_hid(mach->id);
	if (i2c_name != NULL) {
		snprintf(cht_cx_codec_name, sizeof(cht_cx_codec_name),
			"%s%s", "i2c-", i2c_name);

		cht_dailink[dai_index].codec_name = cht_cx_codec_name;
	}

	if ((cht_cx_quirk & CHT_CX_MCLK_EN) && (is_valleyview())) {
		drv->mclk = devm_clk_get(&pdev->dev, "pmc_plt_clk_3");
		if (IS_ERR(drv->mclk)) {
			dev_err(&pdev->dev,
				"Failed to get MCLK from pmc_plt_clk_3: %ld\n",
				PTR_ERR(drv->mclk));
			
			return PTR_ERR(drv->mclk);
		}
	}

	ret_val = devm_snd_soc_register_card(&pdev->dev, &chtcx2072x_card);
	
	if (ret_val) {
		dev_err(&pdev->dev, "devm_snd_soc_register_card failed %d\n",
			ret_val);
		return ret_val;
	}
	platform_set_drvdata(pdev, &chtcx2072x_card);
	return ret_val;
}



static struct platform_driver snd_cht_mc_driver = {
	.driver = {
		.name = "cht-cx2072x",
	},
	.probe = snd_cht_mc_probe,
};
module_platform_driver(snd_cht_mc_driver);

MODULE_DESCRIPTION("ASoC Intel(R) Cherrytrail Machine driver");
MODULE_AUTHOR("Pierre-Louis Bossart <pierre-louis.bossart@linux.intel.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:cht-cx2072x");
