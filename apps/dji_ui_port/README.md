# MaixCAM2 动作相机（LVGL）移植层

在 **Sipeed MaixCAM2（AX630C, 1GB LPDDR4）** 上运行 [SanKuai_DIY_Action_Camera](https://github.com/PrismClov/SanKuai_DIY_Action_Camera) 的 LVGL 界面。

上游工程面向 K230/RK3566（V4L2 + DRM/fbdev + RGA/RKMPP），本目录只包含**平台适配层与补丁**，不包含 LVGL/SanKuai 的完整源码（体积过大，按下方步骤获取）。

## 结构

```
apps/dji_ui_port/
├── port/
│   ├── port.cpp               # LVGL 显示/触摸驱动（32bpp，直接 memcpy 到 maix::display）
│   ├── cam_maix.cpp           # camera_hal 后端 → maix::camera（RGB888 → RGB565）
│   ├── thumb_gen.c/h          # 后台缩略图 worker（.thumbs/，原子 rename，nice+限速）
│   ├── main.cpp               # 入口 + lv_timer_handler + P50/P95/P99 帧耗时统计
│   └── recorder_rkmpp_stub.c  # RK MPP 录制后端空实现（AX 平台无 MPP）
├── main/CMakeLists.txt        # MaixCDK 组件构建描述
├── lv_conf.h                  # LVGL 9.4 配置（32bpp, TJPGD, FS_STDIO 等）
├── patches/
│   ├── sankuai-app.patch      # 对 SanKuai 应用层的修改（unified diff）
│   └── maixcdk.patch          # 对 MaixCDK 构建脚本的修改（Ninja 兼容）
└── scripts/{build.sh,install.sh}
```

## 关键设计

### 1. 显示：照抄官方 MaixCDK LVGL 集成

- `LV_COLOR_DEPTH 32`，与 `maix::display` 的 `BGRA8888` 内存布局一致
- 两块 `width × 100 × 4` 缓冲 + `LV_DISPLAY_RENDER_MODE_PARTIAL`
- flush 时只把脏区域 `memcpy` 进唯一的 `maix::image`，`lv_display_flush_is_last()` 时 `display->show()`
- **零像素转换**（早期 16bpp 方案要做 RGB565→BGRA，是小面积花屏与高 CPU 的根因）

### 2. 相机：`camera_hal` → `maix::camera`

- 1080p 在 512MB 可用内存的机器上会让 ISP 初始化卡死，HAL 层钳制到 640×480
- 帧统一转 RGB565 交给上游 preview 线程（其内部仍做 crop/rotate/scale）

### 3. 相册：把解码搬离 UI 线程

- 拍照保存成功后 `thumb_gen_submit()`，worker 生成 `256px` 缩略图到 `<照片目录>/.thumbs/`
  文件名带 `mtime + size + 版本号`，先写 `.tmp` 再 `rename()`
- `album_image_get_path()` 优先返回该缩略图；缺失时返回占位图并排队，**绝不在 UI 线程解码原图**
- 全屏查看：worker 解码并缩放到 640×480 以内的 **RGB565**，UI 线程轮询后 `lv_image_set_src()`，
  用 generation 丢弃过期结果（`thumb_preview_request/poll/cancel`）
- worker `nice(10)` + 每任务 `usleep(5ms)` 限速，避免和 UI 抢双核 A53

### 4. 触摸

- 直接读 `/dev/input/event*`（`hyn_ts`，MT protocol B）
- 面板 480×640 竖屏 → LVGL 640×480 横屏，坐标做 90° 旋转映射
- 按下/抬起进入 32 深队列并置 `continue_reading`，快速点按不会被丢帧

## 构建

前置：

```bash
# 1. 取上游源码（与 MaixCDK 平级放置）
git clone https://github.com/PrismClov/SanKuai_DIY_Action_Camera
git clone https://github.com/sipeed/MaixCDK

# 2. 把上游 app 与 LVGL 复制进本工程
cp -r SanKuai_DIY_Action_Camera/lv_port_linux/src/app  main/app
cp -r SanKuai_DIY_Action_Camera/lv_port_linux/src/wifi main/wifi
cp -r SanKuai_DIY_Action_Camera/lv_port_linux/lvgl     main/lvgl
cp lv_conf.h main/

# 3. 打补丁
patch -p1 -d main/.. < patches/sankuai-app.patch   # 路径按实际调整
patch -p1 -d "$MAIXCDK_PATH" < patches/maixcdk.patch

# 4. 交叉编译（需要 maixcam2 工具链，maixcdk 会自动下载）
./scripts/build.sh
```

工具链：`arm-gnu-toolchain-11.3.rel1-x86_64-aarch64-none-linux-gnu`（MaixCDK `platforms/maixcam2.yaml`）。

## 部署

板子上（root）：

```bash
scp build/dji_ui root@<board>:/opt/dji_ui/
ssh root@<board> 'sh -s' < scripts/install.sh
```

`install.sh` 会：

- 装到 `/opt/dji_ui`（含 `libdatachannel.so` / `libtinyalsa.so.2` / `libsamplerate.so`）
- 放 CJK 字体 `DroidSansFallbackFull.ttf`
- 写 systemd 单元 `dji_ui.service`，disable `launcher.service`

开机自启与官方界面切换：

```bash
# 启用（会禁用 launcher / maixvision-server）
systemctl disable launcher.service maixvision-server.service
systemctl enable --now dji_ui.service

# 回官方界面
systemctl disable --now dji_ui.service
systemctl enable --now launcher.service
```

`dji_ui.service` 里 `ExecStart=/opt/dji_ui/start.sh`，脚本先 `sleep 6` 再启动，
给 ISP/摄像头驱动模块留出初始化时间，避免开机即开 sensor 卡死。
`TimeoutStartSec=0` 是必需的：MaixCAM2 的 rootfs 把 systemd 默认启动超时改成了 5s。

## 实测

| 指标 | 数值 |
|---|---|
| 渲染 | 30–32 fps，640×480 |
| `lv_timer_handler` P95 / max | ~12ms / ~25ms |
| CPU | ~85%（相机转换 + LVGL 合成，双核 A53） |
| 拍照 | 640×480 JPEG → `/root/photos/` |
| 相册缩略图 | 256px，后台生成，`.thumbs/` 持久化 |

## 待办

- [ ] 全屏预览改用 AX JDEC（`AX_VDEC_JpegDecodeOneFrame`）硬解
- [ ] 相册索引常驻内存，避免每次进入重新扫描
- [ ] 缩略图 RGB565 LRU（16 张上限）
- [ ] 录像接到 `ax_venc`（当前 MJPEG 软编路径未验证）
- [ ] 旧照片目录 `/userdata/DCIM` 合并到 `/root/photos`
