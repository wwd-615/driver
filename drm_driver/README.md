# simple-drm 驱动示例

## 构建
```bash
cd drm_driver
make
```

## 加载与检查（在目标机器上以 root 运行）
```bash
# 加载模块
sudo insmod simple-drm.ko
# 查看内核日志末尾
dmesg | tail -n 20
# 查看 /dev/dri 节点（如果驱动注册了）
ls -l /dev/dri || true
```

如果你安装了 libdrm 的测试工具，可以用 `modetest` 检查 KMS（可选）:
```bash
modetest
```

## 卸载
```bash
sudo rmmod simple-drm
dmesg | tail -n 20
```

## 注意
- 驱动使用了 DRM 的辅助 helper（`drm_simple_display_pipe`、GEM DMA 以及 fbdev 模拟）。
- 若要进行更复杂的显示测试，可使用 userspace 工具（`modetest`、`kmscube`、或基于 libdrm 的测试程序）。
- 如果模块加载失败，请先检查内核配置、符号版本和 `dmesg` 的具体错误信息。

## 数据流传输

这个最简 DRM 驱动的数据流传输路径主要包含以下阶段：

- userspace 分配显存
  - 应用通过 DRM dumb/GEM 接口创建缓冲区（`DRM_IOCTL_MODE_CREATE_DUMB` 等）。
  - 驱动使用 `drm_gem_dma_dumb_create` 分配 GEM/DMA buffer。
- userspace 写入像素数据
  - 应用通过 `DRM_IOCTL_MODE_MAP_DUMB` 和 `mmap()` 将 buffer 映射到用户空间。
  - 应用在映射内存中填充像素数据。
- 创建 framebuffer
  - `drm_gem_fb_create()` 将 GEM buffer 关联为 DRM framebuffer，表示一个可被 CRTC 扫描输出的图像对象。
- modeset/atomic commit
  - userspace 选择 connector、CRTC、plane 和 framebuffer 并发起 modeset 提交。
  - 内核 atomic 框架调用驱动的检查与提交回调，最终执行 `drm_simple_display_pipe` 的 `.update()` / `.enable()` 回调。
- 驱动提交到显示硬件
  - 对于真实硬件，`.update()` 应把 framebuffer 的物理地址 / DMA 地址写入显示控制器寄存器。
  - 这一步是把缓冲区内的数据真正“交给”显示控制器去扫描输出。

当前示例驱动在 `simple_drm_pipe_update()` 中仅打印日志，说明已经到了 DRM 提交点，但没有实际编程硬件输出，因此能被 `modetest` 枚举，但不一定真正显示图像。

真实硬件驱动的数据流完整路径是：userspace -> GEM buffer -> framebuffer -> CRTC/plane -> 显示控制器 -> 屏幕。

## 驱动注册与运行流程

下面是该最简 DRM 驱动在内核中的主要注册与运行步骤，便于调试与扩展：

- 模块初始化：`module_init(simple_drm_init)` 在加载时注册一个 `platform_device` 和 `platform_driver`，触发驱动的 `probe`。
- probe（`simple_drm_probe`）：
	- 使用 `devm_drm_dev_alloc()` 分配并初始化内嵌的 `struct drm_device`。
	- 初始化 `drm_mode_config`（设置可接受分辨率、`mode_config.funcs` 等）。
	- 初始化并注册一个虚拟 `drm_connector`，并通过 helper 提供 `get_modes`，返回我们预定义的模式（如 800x600）。
	- 调用 `drm_simple_display_pipe_init()` 初始化简化显示管道（plane/crtc/encoder），并指定支持的像素格式。
	- 调用 `drm_dev_register()` 将设备注册到 DRM 子系统；内核将创建设备节点（如 `/dev/dri/cardN`）。
	- 可选：调用 `drm_fbdev_dma_setup()` 提供 fbdev 兼容层以方便测试。
- 运行时：
	- userspace（如 `modetest`）通过 DRM 接口枚举 connector 和 modes；驱动在 `get_modes` 中返回预定义模式。
	- userspace 发起 modeset（通常通过 atomic API），内核 atomic 框架调用驱动的检查与提交回调；drm_simple_display_pipe 的 `.update/.enable/.disable` 回调在提交时被调用以让驱动设置硬件（本示例以日志替代硬件编程）。
	- 显存分配与 mmap：通过 dumb/GEM 接口分配缓冲区并映射到 userspace，用于绘制测试图像。
- 卸载：`rmmod` 调用模块 exit，驱动在 remove 中执行 `drm_dev_unregister()`、`drm_atomic_helper_shutdown()` 和 `drm_mode_config_cleanup()` 完成清理。

将以上步骤作为扩展或移植真实硬件的参考：真实硬件驱动需要在 `.enable/.update` 中对显示控制器寄存器进行编程，并在适当位置处理 vblank/中断与缓存同步。