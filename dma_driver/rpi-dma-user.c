// SPDX-License-Identifier: GPL-2.0-only
/*
 * Simple BCM2835 I2S static sample playback driver
 *
 * This driver configures the BCM2835 PCM/I2S block and repeatedly writes
 * an embedded sample buffer to the I2S transmit FIFO.
 *
 * It is intended as a self-contained example driver that plays static
 * audio data through the BCM2835 I2S interface without a full ALSA PCM
 * user-space playback pipeline.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/workqueue.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>

#define BCM2835_I2S_CS_A_REG            0x00
#define BCM2835_I2S_FIFO_A_REG          0x04
#define BCM2835_I2S_MODE_A_REG          0x08
#define BCM2835_I2S_RXC_A_REG           0x0c
#define BCM2835_I2S_TXC_A_REG           0x10
#define BCM2835_I2S_DREQ_A_REG          0x14

#define BCM2835_I2S_STBY                BIT(25)
#define BCM2835_I2S_SYNC                BIT(24)
#define BCM2835_I2S_TXE                 BIT(21)
#define BCM2835_I2S_TXD                 BIT(19)
#define BCM2835_I2S_TXW                 BIT(17)
#define BCM2835_I2S_RXCLR               BIT(4)
#define BCM2835_I2S_TXCLR               BIT(3)
#define BCM2835_I2S_TXON                BIT(2)
#define BCM2835_I2S_RXON                BIT(1)
#define BCM2835_I2S_EN                  BIT(0)

#define BCM2835_I2S_FRXP                BIT(25)
#define BCM2835_I2S_FTXP                BIT(24)
#define BCM2835_I2S_CLKM                BIT(23)
#define BCM2835_I2S_CLKI                BIT(22)
#define BCM2835_I2S_FSM                 BIT(21)
#define BCM2835_I2S_FSI                 BIT(20)
#define BCM2835_I2S_FLEN(v)             ((v) << 10)
#define BCM2835_I2S_FSLEN(v)            (v)

#define BCM2835_I2S_CHEN                BIT(14)
#define BCM2835_I2S_CHPOS(v)            ((v) << 4)
#define BCM2835_I2S_CHWID(v)            (v)
#define BCM2835_I2S_CH1(v)              ((v) << 16)
#define BCM2835_I2S_CH2(v)              (v)
#define BCM2835_I2S_CH1_POS(v)          BCM2835_I2S_CH1(BCM2835_I2S_CHPOS(v))
#define BCM2835_I2S_CH2_POS(v)          BCM2835_I2S_CH2(BCM2835_I2S_CHPOS(v))

#define BCM2835_I2S_TXTHR(v)            ((v) << 5)
#define BCM2835_I2S_RXTHR(v)            ((v) << 7)

#define BCM2835_I2S_TX_PANIC(v)         ((v) << 24)
#define BCM2835_I2S_RX_PANIC(v)         ((v) << 16)
#define BCM2835_I2S_TX(v)               ((v) << 8)
#define BCM2835_I2S_RX(v)               (v)

#define SAMPLE_RATE                     48000
#define SAMPLE_CHANNELS                 2
#define SAMPLE_WIDTH_BITS               16
#define SAMPLE_FRAME_COUNT              48

#define STOP_FRAME_COUNT                192
#define BCM2835_I2S_GRAY_REG		0x20
static const struct regmap_config bcm2835_regmap_config = {
	.reg_bits = 32,
	.reg_stride = 4,
	.val_bits = 32,
	.max_register = BCM2835_I2S_GRAY_REG,
};

static const int16_t sample_data[SAMPLE_FRAME_COUNT * SAMPLE_CHANNELS] = {
	[0 ... 23]  = 0x7fff,
	[24 ... 47] = -0x8000,
};

struct bcm2835_i2s_static_dev {
	struct device *dev;
	struct regmap *i2s_regmap;
	struct clk *clk;
	bool clk_prepared;
	unsigned int clk_rate;
	struct mutex lock;
	struct delayed_work work;
	unsigned int sample_frame;
	bool playback_enabled;
	/* DMA fields */
	struct dma_chan *tx_chan;
	void *dma_buf;
	dma_addr_t dma_addr;
	size_t dma_size;
	dma_cookie_t dma_cookie;
	struct completion dma_complete;
};

static void bcm2835_i2s_static_start_clock(struct bcm2835_i2s_static_dev *dev)
{
	if (dev->clk_prepared)
		return;

	if (clk_prepare_enable(dev->clk) == 0)
		dev->clk_prepared = true;
}

static void bcm2835_i2s_static_stop_clock(struct bcm2835_i2s_static_dev *dev)
{
	if (!dev->clk_prepared)
		return;

	clk_disable_unprepare(dev->clk);
	dev->clk_prepared = false;
}

static void bcm2835_i2s_static_clear_fifos(struct bcm2835_i2s_static_dev *dev)
{
	regmap_update_bits(dev->i2s_regmap, BCM2835_I2S_CS_A_REG,
			BCM2835_I2S_TXCLR | BCM2835_I2S_RXCLR,
			BCM2835_I2S_TXCLR | BCM2835_I2S_RXCLR);
}

static void bcm2835_i2s_static_fill_fifo(struct bcm2835_i2s_static_dev *dev)
{
	uint32_t cs;
	unsigned int written = 0;

	while (written < 8) {
		if (regmap_read(dev->i2s_regmap, BCM2835_I2S_CS_A_REG, &cs))
		break;

		if (!(cs & BCM2835_I2S_TXW))
		break;

		uint16_t left = sample_data[dev->sample_frame * SAMPLE_CHANNELS];
		uint16_t right = sample_data[dev->sample_frame * SAMPLE_CHANNELS + 1];
		uint32_t fifo_word = (uint32_t)(uint16_t)left |
					((uint32_t)(uint16_t)right << 16);

		regmap_write(dev->i2s_regmap, BCM2835_I2S_FIFO_A_REG, fifo_word);

		dev->sample_frame++;
		if (dev->sample_frame >= SAMPLE_FRAME_COUNT)
		dev->sample_frame = 0;

		written++;
	}
}

static void bcm2835_i2s_static_playback_work(struct work_struct *work)
{
	struct bcm2835_i2s_static_dev *dev = container_of(to_delayed_work(work),
							struct bcm2835_i2s_static_dev,
							work);

	mutex_lock(&dev->lock);
	if (dev->playback_enabled) {
		bcm2835_i2s_static_fill_fifo(dev);
		schedule_delayed_work(&dev->work, msecs_to_jiffies(2));
	}
	mutex_unlock(&dev->lock);
}

static void bcm2835_i2s_static_dma_complete(void *param)
{
	struct bcm2835_i2s_static_dev *dev = param;

	mutex_lock(&dev->lock);
	dev->playback_enabled = false;
	mutex_unlock(&dev->lock);

	regmap_update_bits(dev->i2s_regmap, BCM2835_I2S_CS_A_REG,
			BCM2835_I2S_TXON | BCM2835_I2S_EN | BCM2835_I2S_STBY,
			0);
	bcm2835_i2s_static_stop_clock(dev);

	complete(&dev->dma_complete);

	dev_info(dev->dev, "BCM2835 I2S DMA playback complete\n");
}

static int bcm2835_i2s_static_configure(struct bcm2835_i2s_static_dev *dev)
{
	unsigned int bclk_rate = SAMPLE_RATE * SAMPLE_CHANNELS * SAMPLE_WIDTH_BITS;
	unsigned int format;
	unsigned int mode;
	unsigned int tx_pos;

	if (dev->clk_rate != bclk_rate) {
		int ret = clk_set_rate(dev->clk, bclk_rate);
		if (ret)
		return ret;
		dev->clk_rate = bclk_rate;
	}

	bcm2835_i2s_static_start_clock(dev);

	format = BCM2835_I2S_CHEN |
		BCM2835_I2S_CHWID((SAMPLE_WIDTH_BITS - 8) & 0xf);
	format = BCM2835_I2S_CH1(format) | BCM2835_I2S_CH2(format);

	tx_pos = BCM2835_I2S_CH1_POS(1) | BCM2835_I2S_CH2_POS(17);

	regmap_write(dev->i2s_regmap, BCM2835_I2S_RXC_A_REG,
			format | tx_pos);
	regmap_write(dev->i2s_regmap, BCM2835_I2S_TXC_A_REG,
			format | tx_pos);

	mode = BCM2835_I2S_FTXP |
		BCM2835_I2S_FRXP |
		BCM2835_I2S_FLEN((SAMPLE_CHANNELS * SAMPLE_WIDTH_BITS) - 1) |
		BCM2835_I2S_FSLEN(SAMPLE_CHANNELS * SAMPLE_WIDTH_BITS) |
		BCM2835_I2S_CLKI |
		BCM2835_I2S_FSI;

	regmap_write(dev->i2s_regmap, BCM2835_I2S_MODE_A_REG, mode);

	regmap_update_bits(dev->i2s_regmap, BCM2835_I2S_CS_A_REG,
			BCM2835_I2S_RXTHR(1) | BCM2835_I2S_TXTHR(1),
			BCM2835_I2S_RXTHR(1) | BCM2835_I2S_TXTHR(1));

	bcm2835_i2s_static_clear_fifos(dev);

	regmap_update_bits(dev->i2s_regmap, BCM2835_I2S_CS_A_REG,
			BCM2835_I2S_EN | BCM2835_I2S_STBY,
			BCM2835_I2S_EN | BCM2835_I2S_STBY);

	regmap_update_bits(dev->i2s_regmap, BCM2835_I2S_CS_A_REG,
			BCM2835_I2S_TXON, BCM2835_I2S_TXON);

	return 0;
}

static int bcm2835_i2s_static_probe(struct platform_device *pdev)
{
	struct bcm2835_i2s_static_dev *dev;
	void __iomem *base;
	struct resource *res;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->dev = &pdev->dev;
	mutex_init(&dev->lock);
	INIT_DELAYED_WORK(&dev->work, bcm2835_i2s_static_playback_work);
	dev->clk_prepared = false;
	dev->sample_frame = 0;

	dev->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(dev->clk))
		return dev_err_probe(&pdev->dev, PTR_ERR(dev->clk),
				"could not get clk\n");

	base = devm_platform_get_and_ioremap_resource(pdev, 0, &res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	dev->i2s_regmap = devm_regmap_init_mmio(&pdev->dev, base, &bcm2835_regmap_config);
	if (IS_ERR(dev->i2s_regmap))
		return PTR_ERR(dev->i2s_regmap);

	platform_set_drvdata(pdev, dev);

	ret = bcm2835_i2s_static_configure(dev);
	if (ret) {
		dev_err(&pdev->dev, "failed to configure I2S: %d\n", ret);
		return ret;
	}

	/* Prepare DMA one-shot buffer for STOP_FRAME_COUNT frames */
	init_completion(&dev->dma_complete);
	dev->dma_size = STOP_FRAME_COUNT * SAMPLE_CHANNELS * (SAMPLE_WIDTH_BITS / 8);
	dev->dma_buf = dma_alloc_coherent(dev->dev, dev->dma_size, &dev->dma_addr, GFP_KERNEL);
	if (!dev->dma_buf) {
		dev_err(&pdev->dev, "failed to allocate coherent DMA buffer\n");
		return -ENOMEM;
	}

	/* Fill DMA buffer with repeated sample pattern */
	{
		uint32_t *p = dev->dma_buf;
		int i;

		for (i = 0; i < STOP_FRAME_COUNT; i++) {
		int src_idx = (i % SAMPLE_FRAME_COUNT) * SAMPLE_CHANNELS;
		int16_t left = sample_data[src_idx];
		int16_t right = sample_data[src_idx + 1];
		p[i] = (uint32_t)(uint16_t)left | ((uint32_t)(uint16_t)right << 16);
		}
	}

	/* Request TX DMA channel (OF dma binding should provide "tx") */
	dev->tx_chan = dma_request_slave_channel(dev->dev, "tx");
	if (!dev->tx_chan) {
		dev_warn(&pdev->dev,
			 "tx DMA channel unavailable, falling back to FIFO playback\n");
		dev->playback_enabled = true;
		schedule_delayed_work(&dev->work, msecs_to_jiffies(2));
		goto out;
	}

	/* Configure slave DMA */
	{
		struct dma_slave_config cfg = { 0 };
		struct resource *mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);

		if (!mem) {
			ret = -ENODEV;
			dev_err(&pdev->dev, "missing I2S MMIO resource\n");
			goto err_release_chan;
		}

		cfg.direction = DMA_MEM_TO_DEV;
		cfg.dst_addr = mem->start + BCM2835_I2S_FIFO_A_REG;
		cfg.dst_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.src_addr_width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		cfg.dst_maxburst = 2;
		cfg.src_maxburst = 2;

		ret = dmaengine_slave_config(dev->tx_chan, &cfg);
		if (ret) {
			dev_err(&pdev->dev, "dmaengine_slave_config failed: %d\n", ret);
			goto err_release_chan;
		}
	}

	/* Prepare and submit a one-shot DMA transfer */
	{
		struct dma_async_tx_descriptor *desc;

		desc = dmaengine_prep_slave_single(dev->tx_chan, dev->dma_addr,
						dev->dma_size, DMA_MEM_TO_DEV,
						DMA_PREP_INTERRUPT | DMA_CTRL_ACK);
		if (!desc) {
			dev_err(&pdev->dev, "dmaengine_prep_slave_single failed\n");
			ret = -ENOMEM;
			goto err_release_chan;
		}

		desc->callback = bcm2835_i2s_static_dma_complete;
		desc->callback_param = dev;

		dev->dma_cookie = dmaengine_submit(desc);
		if (dma_submit_error(dev->dma_cookie)) {
			dev_err(&pdev->dev, "dmaengine_submit failed\n");
			ret = -EIO;
			goto err_release_chan;
		}

		/* Start DMA and enable TX */
		dma_async_issue_pending(dev->tx_chan);
		dev->playback_enabled = true;
	}

out:
	dev_info(&pdev->dev, "BCM2835 I2S static playback driver loaded\n");
	return 0;

err_release_chan:
	if (dev->tx_chan)
		dma_release_channel(dev->tx_chan);
	if (dev->dma_buf)
		dma_free_coherent(dev->dev, dev->dma_size, dev->dma_buf, dev->dma_addr);
	return ret;
}


static int bcm2835_i2s_static_remove(struct platform_device *pdev)
{
	struct bcm2835_i2s_static_dev *dev = platform_get_drvdata(pdev);

	mutex_lock(&dev->lock);
	dev->playback_enabled = false;
	mutex_unlock(&dev->lock);

	cancel_delayed_work_sync(&dev->work);
	regmap_update_bits(dev->i2s_regmap, BCM2835_I2S_CS_A_REG,
			BCM2835_I2S_TXON | BCM2835_I2S_EN | BCM2835_I2S_STBY,
			0);
	bcm2835_i2s_static_stop_clock(dev);

	/* Terminate any outstanding DMA and free resources */
	if (dev->tx_chan) {
		dmaengine_terminate_all(dev->tx_chan);
		dma_release_channel(dev->tx_chan);
		dev->tx_chan = NULL;
	}

	if (dev->dma_buf) {
		dma_free_coherent(dev->dev, dev->dma_size, dev->dma_buf, dev->dma_addr);
		dev->dma_buf = NULL;
	}

	dev_info(&pdev->dev, "BCM2835 I2S static playback driver unloaded\n");
	return 0;
}

static const struct of_device_id bcm2835_i2s_static_of_match[] = {
	{ .compatible = "brcm,bcm2835-i2s-static" },
	{ .compatible = "brcm,bcm2835-i2s", },
	{}
};
MODULE_DEVICE_TABLE(of, bcm2835_i2s_static_of_match);

static struct platform_driver bcm2835_i2s_static_driver = {
	.probe = bcm2835_i2s_static_probe,
	.remove = bcm2835_i2s_static_remove,
	.driver = {
		.name = "bcm2835-i2s-static",
		.of_match_table = bcm2835_i2s_static_of_match,
	},
};

module_platform_driver(bcm2835_i2s_static_driver);

MODULE_DESCRIPTION("BCM2835 I2S static sample playback driver");
MODULE_AUTHOR("GitHub Copilot");
MODULE_LICENSE("GPL v2");
