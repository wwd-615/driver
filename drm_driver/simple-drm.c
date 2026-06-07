// SPDX-License-Identifier: GPL-2.0-only
/*
 * simple-drm.c
 * 虚拟 DRM 驱动示例，实现一个基本的 DRM 驱动框架，支持显示模式枚举和简单的显示管道回调。
 * author：wwd
 * 主要特性：
 * - 使用 drm_simple_display_pipe 提供的简单 KMS 管道辅助代码
 * - 使用 GEM/DMA helper 提供 dumb buffer 支持
 * - 注册一个虚拟 connector，使得 userspace 工具（如 modetest）可枚举
 *
 */

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

struct simple_drm_dev {
	struct drm_device drm;			// 内嵌 DRM 核心设备对象
	struct drm_simple_display_pipe pipe;	// 简化显示管道（含 CRTC+Encoder+Plane）
	struct drm_connector connector;		/* 内嵌的 connector */
};


/* 显示模式 800x600@60Hz

 * 水平扫描线时序:
 * <-- htotal ------------------------------------------------------>
 * <-- hdisplay --><-- 前沿 --><-- 同步宽 --><-- 后沿 -->
 * +--------------+-----------+---------- --+----------+
 * |   有效像素    |  右边界   |   水平同步    |   左边界      |
 * |   显示区域    |  HFP      |   (HSYNC)     |   HBP       |
 * +--------------+-----------+---------------+--------------+
 * ^              ^           ^               ^               ^
 * 0           hdisplay   hsync_start      hsync_end        htotal
 */
static const struct drm_display_mode simple_drm_mode = {	
	.clock = 40000,						// 像素时钟 40 MHz
	.hdisplay = 800,					// 水平有效像素
	.hsync_start = 800 + 40,				// 水平同步开始（有效结束+后沿）
	.hsync_end = 800 + 40 + 48,				// 水平同步结束（+同步宽度）
	.htotal = 800 + 40 + 48 + 40,				// 一行总像素
	.vdisplay = 600,					// 垂直有效行数
	.vsync_start = 600 + 13,				// 垂直同步开始
	.vsync_end = 600 + 13 + 3,				// 垂直同步结束
	.vtotal = 600 + 13 + 3 + 29,				// 一帧总行数
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,	//模式标志（如 DRM_MODE_FLAG_PHSYNC、DRM_MODE_FLAG_NVSYNC、DRM_MODE_FLAG_INTERLACE 等）
};

/* 
 * 作用：探测并填充该连接器上可用的所有显示模式 
 * 成功：返回添加的模式数量（通常 ≥ 1）
 * 错误：返回负的错误码（如 -ENODEV、-ENOMEM）
 */
static int simple_drm_connector_get_modes(struct drm_connector *connector)
{
	struct drm_display_mode *mode;
	int num_modes;

	mode = drm_mode_duplicate(connector->dev, &simple_drm_mode);
	if (!mode) {
		DRM_ERROR("Failed to duplicate mode\n");
		return 0;
	}

	drm_mode_probed_add(connector, mode);	// 将模式添加到连接器的模式列表中
	drm_mode_set_name(mode);		// 设置模式名称（如 "hdisplayxvdisplay = 800x600"），便于 userspace 工具显示
	num_modes = 1;

	return num_modes;
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
	.detect = simple_drm_connector_detect,					// 连接器检测回调，始终返回已连接
	.fill_modes = drm_helper_probe_single_connector_modes,			// 填充模式回调，使用 DRM 提供的 helper 函数根据连接器状态填充模式列表
	.destroy = simple_drm_connector_destroy,				// 销毁连接器时调用，清理资源
	.reset = drm_atomic_helper_connector_reset,				// 原子配置重置，分配初始状态
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state, 	// 原子配置复制，分配新的状态对象并复制当前状态
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,	// 原子配置销毁，释放状态对象
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
	.fb_create = drm_gem_fb_create,			// 创建 framebuffer，作用是将用户态提供的 framebuffer 参数转换为 DRM 内部的 framebuffer 对象
	.atomic_check = drm_atomic_helper_check,	// 原子配置检查，作用是验证用户态提交的原子配置是否合法，是否满足驱动的要求
	.atomic_commit = drm_atomic_helper_commit,	// 原子配置提交，作用是将用户态提交的原子配置应用到硬件上，完成显示模式切换、帧缓冲更新等操作
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

static struct drm_driver drm_driver = {
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

static int simple_drm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct simple_drm_dev *priv;
	struct drm_device *drm;
	int ret;

	priv = devm_drm_dev_alloc(dev, &drm_driver, struct simple_drm_dev, drm);	// 分配并初始化 drm_device 结构，内嵌在 simple_drm_dev 中
	if (IS_ERR(priv))
		return PTR_ERR(priv);
	drm = &priv->drm;

	drm_mode_config_init(drm);							// 初始化 mode_config 结构
	drm->mode_config.min_width = 800;						// 设置显示模式的最小/最大分辨率，匹配我们定义的 simple_drm_mode
	drm->mode_config.max_width = 800;
	drm->mode_config.min_height = 600;
	drm->mode_config.max_height = 600;
	drm->mode_config.preferred_depth = 32;						// 设置首选颜色深度（32 位），与 simple_drm_formats 中的格式一致	
	drm->mode_config.funcs = &simple_drm_mode_config_funcs;				// 设置 mode_config 的回调函数，提供 framebuffer 创建和原子检查/提交功能

	/*
	 * 初始化 connector 内部的锁、列表头。
	 * 分配一个唯一的 connector_id。
	 * 将 connector 挂入 dev->mode_config.connector_list。
	 * 关联用户空间对应的 connector 设备文件。
	 * 初始化与 EDID、刷新率、强制模式等相关的字段。
	 * 注册调试文件系统（debugfs）节点（如果启用）
	 * 添加我们自定义的connector funcs
	 */
	ret = drm_connector_init(drm, &priv->connector,
				&simple_drm_connector_funcs,
				DRM_MODE_CONNECTOR_VIRTUAL);
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to init connector: %d\n", ret);
		goto err_mode_config_cleanup;
	}

	/*
	 * 添加连接器辅助函数，提供 get_modes 功能，使得用户空间工具（如 modetest）能够枚举显示模式
	 * 其中get_modes是必须实现，用于返回连接器支持的显示模式列表。
	 */
	drm_connector_helper_add(&priv->connector, &simple_drm_connector_helper_funcs);

	/*
	 * 初始化 simple pipe
	 * drm_simple_display_pipe_init 是一个专为简化显示驱动开发的辅助函数
	 * 它能将标准DRM显示管线中复杂的 plane（图层）、crtc（显示控制器）和 encoder（编码器） 三个组件，
	 * 捆绑成一个单一的、易于管理的结构体 struct drm_simple_display_pipe，并把一个用户提供的 connector（连接器）连接至此管线上
	 * dev：指向 struct drm_device 的指针。它标识了当前操作属于哪个DRM设备，通常是在驱动的 probe 函数中初始化好的那个设备对象。
	 * pipe：指向 struct drm_simple_display_pipe 结构体的指针。
	 * 	 这个结构体由驱动维护，是该辅助函数的核心，代表这个简化的显示管线。函数执行成功后，这里会存放所有初始化好的KMS对象信息。
	 * funcs：指向 struct drm_simple_display_pipe_funcs 结构体的指针，是一个可选参数。
	 * 	 它包含了一组回调函数（如 enable、disable、update 等），用于让驱动在硬件状态变化时执行特定的操作。如果不实现这些回调，可以传入 NULL。
	 * formats：一个指向 uint32_t 数组的指针，数组内列出驱动所支持的像素格式（例如 DRM_FORMAT_XRGB8888）。这些格式是内核中预定义的宏。
	 * format_count：指明前面 formats 数组中的元素数量
	 * format_modifiers: array of formats modifiers
	 * connector：指向 struct drm_connector 结构体的指针。用来指定该显示管线最终连接到哪个显示输出接口上。
	 * 	     如果使用 drm_bridge 架构（一种更灵活的显示管线连接方式），此参数可以传入 NULL，因为 bridge 本身会负责管理 connector。
	 */
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

	/* 
	 * 将所有 KMS（Kernel Mode Setting）对象的软件状态重置为一个默认的“全关闭”状态
	 * 它通过遍历所有CRTC、Encoder、Connector等对象，调用它们各自的->reset回调函数来完成这个任务
	 * 1.驱动加载时, 建立初始状态：在驱动加载的 probe 阶段，KMS 对象的软件状态是未初始化的。
	 * 			     通过调用 drm_mode_config_reset() 可以立即为所有对象分配默认的“全关闭”状态，确保系统在启动时拥有一个明确、有效的初始配置
	 * 2. 系统唤醒时，同步软硬件状态：系统从休眠（Suspend）中恢复时，硬件的显示状态可能已被重置。
	 * 			     此时调用此函数，可以重置相应的软件状态，使之与硬件重新同步，避免出现状态错乱
	 * 
	 * KMS对象：Kernel Mode Setting 在 DRM 子系统中，KMS 负责管理显示输出的配置，包括分辨率、刷新率、像素格式、连接器状态等。为了实现这些功能，内核定义了几种核心对象类型
	 * CRTC： struct drm_crtc	代表一个显示控制器，负责从内存中读取 framebuffer 数据，并生成符合时序的视频信号（行场同步、消隐等）。通常每个 CRTC 对应一个显示输出。
	 * Plane：struct drm_plane	代表一个硬件图层。CRTC 读取的数据可以来自多个 Plane（比如主平面、光标平面、叠加层），最后在硬件上合成。简单的显示器可能只有一个主平面。
	 * Encoder：struct drm_encoder	将 CRTC 输出的信号转换为特定物理接口所需的格式，例如将并行 RGB 转换为 HDMI TMDS 信号，或将 LVDS 信号转换为 Panel 需要的格式。
	 * Connector：struct drm_connector	代表一个物理连接器（如 HDMI、DP、VGA 插口）或内部面板。它负责报告连接状态、读取 EDID、提供支持的显示模式列表。
	 * Framebuffer：struct drm_framebuffer	代表一块内存缓冲区，里面存储了要显示的图像像素数据。它关联到具体的 GEM 对象，并包含了宽度、高度、像素格式、 pitches 等信息。
	 */
	drm_mode_config_reset(drm);

	ret = drm_dev_register(drm, 0);	// 注册 DRM 设备，创建 /dev/dri/cardX 设备文件，使得用户空间工具能够访问和控制该 DRM 设备 
	if (ret) {
		DRM_DEV_ERROR(dev, "Failed to register DRM device: %d\n", ret);
		goto err_connector_cleanup;
	}

	drm_fbdev_dma_setup(drm, 32);	// 设置 fbdev 模式，提供一个基于 DMA 的 framebuffer 实现，使得旧的 fbdev 应用（如 startx）能够在没有专门用户空间驱动的情况下显示图形界面

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

static struct platform_device *simple_drm_pdev;

static int __init simple_drm_init(void)
{
	int ret;

	simple_drm_pdev = platform_device_register_simple("simple-drm", -1, NULL, 0);  // 注册一个简单的 platform device，名字与 platform driver 匹配, 为分配具体资源
	if (IS_ERR(simple_drm_pdev))
		return PTR_ERR(simple_drm_pdev);

	ret = platform_driver_register(&simple_drm_platform_driver);	// 注册 platform driver，与device匹配后调用 probe 函数初始化 DRM 设备
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