// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/platform_device.h>

#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_simple_kms_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode_config.h>
#include <drm/drm_gem_framebuffer_helper.h>

/*
 * simple-drm.c
 * 最小可测试的 DRM 驱动示例（教学/验证用途）
 *
 * 主要特性：
 * - 使用 drm_simple_display_pipe 提供的简单 KMS 管道辅助代码
 * - 使用 GEM/DMA helper 提供 dumb buffer 支持
 * - 注册一个虚拟 connector，使得 userspace 工具（如 modetest）可枚举
 *
 * 本文件添加了足够的注释来说明驱动的注册流程和运行时工作流程。
 */

/* 显示模式 800x600@60Hz */
static const struct drm_display_mode simple_drm_mode = {
	/* 定义显示模式：800x600@60Hz
	 *
	 * 注意：为保证该模式在 userspace 工具枚举时被视为驱动提供的首选模式，
	 * 我们将 .type 设置为 DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED。
	 */
	
	.clock = 40000,				// 像素时钟 40 MHz
	.hdisplay = 800,			// 水平有效像素
	.hsync_start = 800 + 40,		// 水平同步开始（有效结束+后沿）
	.hsync_end = 800 + 40 + 48,		// 水平同步结束（+同步宽度）
	.htotal = 800 + 40 + 48 + 40,		// 一行总像素
	.vdisplay = 600,			// 垂直有效行数
	.vsync_start = 600 + 13,		// 垂直同步开始
	.vsync_end = 600 + 13 + 3,		// 垂直同步结束
	.vtotal = 600 + 13 + 3 + 29,		// 一帧总行数
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

struct simple_drm_dev {
	struct drm_device drm;			// 内嵌 DRM 核心设备对象
	struct drm_simple_display_pipe pipe;	// 简化显示管道（含 CRTC+Encoder+Plane）
	struct drm_connector connector;		/* 内嵌的 connector */
	/* 驱动私有数据结构
	 * - 内嵌 struct drm_device 作为 DRM 子系统的实例
	 * - drm_simple_display_pipe: 简化的 plane/crtc/encoder 管道封装
	 * - drm_connector: 内嵌一个虚拟 connector 用于暴露 modes 给 userspace
	 */
};

/* ---- Connector 辅助函数 ---- */
/* Connector 代表物理显示接口（如 HDMI、VGA），这里是一个虚拟连接器，始终报告“已连接 
当用户态查询连接器支持的模式时，这个回调被调用。它复制预定义的显示模式，添加到连接器的模式列表中。drm_mode_probed_add 将模式挂到 connector 的 probed_modes 链表*/
static int simple_drm_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &simple_drm_mode);
	if (!mode) {
		DRM_ERROR("Failed to duplicate mode\n");
		return 0;
	}
	drm_mode_probed_add(connector, mode);
	drm_mode_set_name(mode);
	return 1;
}

static const struct drm_connector_helper_funcs simple_drm_connector_helper_funcs = {
	.get_modes = simple_drm_connector_get_modes,
};

static enum drm_connector_status
simple_drm_connector_detect(struct drm_connector *connector, bool force)
{
	return connector_status_connected;	// 始终已连接
}

static void simple_drm_connector_destroy(struct drm_connector *connector)
{
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs simple_drm_connector_funcs = {
	.detect = simple_drm_connector_detect,
	.fill_modes = drm_helper_probe_single_connector_modes,	// 标准辅助函数
	.destroy = simple_drm_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

/* ---- 显示管道回调 ---- */
static void simple_drm_pipe_enable(struct drm_simple_display_pipe *pipe,
                                   struct drm_crtc_state *crtc_state,
                                   struct drm_plane_state *plane_state)
{
	struct drm_device *drm = pipe->crtc.dev;
	drm_info(drm, "Pipe enabled\n");
}

static void simple_drm_pipe_disable(struct drm_simple_display_pipe *pipe)
{
	struct drm_device *drm = pipe->crtc.dev;
	drm_info(drm, "Pipe disabled\n");
}

static void simple_drm_pipe_update(struct drm_simple_display_pipe *pipe,
                                   struct drm_plane_state *old_plane_state)
{
	struct drm_plane_state *state = pipe->plane.state;
	struct drm_framebuffer *fb = state->fb;
	if (fb) {
		drm_info(fb->dev, "Update fb: %dx%d\n", fb->width, fb->height);
	}
}

static const struct drm_simple_display_pipe_funcs simple_drm_pipe_funcs = {
	.enable = simple_drm_pipe_enable,
	.disable = simple_drm_pipe_disable,
	.update = simple_drm_pipe_update,
};

static const uint32_t simple_drm_formats[] = {
	DRM_FORMAT_XRGB8888,
	DRM_FORMAT_ARGB8888,
};

static const struct drm_mode_config_funcs simple_drm_mode_config_funcs = {
	.fb_create = drm_gem_fb_create,          // 创建 framebuffer
	.atomic_check = drm_atomic_helper_check, // 原子配置检查
	.atomic_commit = drm_atomic_helper_commit, // 原子配置提交
};

static struct drm_driver simple_drm_driver;

static int simple_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct simple_drm_dev *priv;
	struct drm_device *drm;
	int ret;

	priv = devm_drm_dev_alloc(dev, &simple_drm_driver,
				struct simple_drm_dev, drm);
	if (IS_ERR(priv))
		return PTR_ERR(priv);
	drm = &priv->drm;

	drm_mode_config_init(drm);
	drm->mode_config.min_width = 800;
	drm->mode_config.max_width = 800;
	drm->mode_config.min_height = 600;
	drm->mode_config.max_height = 600;
	drm->mode_config.preferred_depth = 32;
	drm->mode_config.funcs = &simple_drm_mode_config_funcs;

	/* 初始化 connector */
	ret = drm_connector_init(drm, &priv->connector,
				&simple_drm_connector_funcs,
				DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to init connector: %d\n", ret);
		goto err_mode_config_cleanup;
	}
	drm_connector_helper_add(&priv->connector,
				&simple_drm_connector_helper_funcs);

	/* 初始化 simple pipe */
	ret = drm_simple_display_pipe_init(drm, &priv->pipe,
					&simple_drm_pipe_funcs,
					simple_drm_formats,
					ARRAY_SIZE(simple_drm_formats),
					NULL,
					&priv->connector);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to init pipe: %d\n", ret);
		goto err_connector_cleanup;
	}

	/* ★★★ 添加这一行：重置所有 KMS 对象的状态，分配初始 state ★★★ */
	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to register DRM device: %d\n", ret);
		goto err_connector_cleanup;
	}

	drm_fbdev_dma_setup(drm, 32);
	platform_set_drvdata(pdev, priv);
	return 0;

	err_connector_cleanup:
	drm_connector_cleanup(&priv->connector);
	err_mode_config_cleanup:
	drm_mode_config_cleanup(drm);

	return ret;
}

static int simple_drm_remove(struct platform_device *pdev)
{
	struct simple_drm_dev *priv = platform_get_drvdata(pdev);
	struct drm_device *drm = &priv->drm;

	drm_dev_unregister(drm);
	drm_atomic_helper_shutdown(drm);
	drm_mode_config_cleanup(drm);
	return 0;
}

static struct platform_driver simple_drm_platform_driver = {
	.probe = simple_drm_probe,
	.remove = simple_drm_remove,
	.driver = {
		.name = "simple-drm",
	},
};

static const struct file_operations simple_drm_fops = {
	.owner          = THIS_MODULE,
	.open           = drm_open,
	.release        = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl   = drm_compat_ioctl,
	.poll           = drm_poll,
	.read           = drm_read,
	.llseek         = no_llseek,
	.mmap           = drm_gem_mmap,
};

static struct drm_driver simple_drm_driver = {
	.driver_features = DRIVER_GEM | DRIVER_MODESET | DRIVER_ATOMIC,
	.name = "simple-drm",
	.desc = "Simple DRM driver example",
	.date = "20250101",
	.major = 1,
	.minor = 0,
	.patchlevel = 0,
	.dumb_create = drm_gem_dma_dumb_create,
	.fops = &simple_drm_fops,
};

static struct platform_device *simple_drm_pdev;

static int __init simple_drm_init(void)
{
	int ret;
	simple_drm_pdev = platform_device_register_simple("simple-drm", -1, NULL, 0);
	if (IS_ERR(simple_drm_pdev))
		return PTR_ERR(simple_drm_pdev);
	ret = platform_driver_register(&simple_drm_platform_driver);
	if (ret) {
		platform_device_unregister(simple_drm_pdev);
		return ret;
	}
	return 0;
}

static void __exit simple_drm_exit(void)
{
	platform_driver_unregister(&simple_drm_platform_driver);
	platform_device_unregister(simple_drm_pdev);
}

module_init(simple_drm_init);
module_exit(simple_drm_exit);

MODULE_DESCRIPTION("Simple DRM driver with connector");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Your Name");