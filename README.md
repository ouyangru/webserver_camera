# webserver_camera

这是一个用于摄像头视频采集、编码与网络服务的示例工程，包含 HTTP 与 RTSP 相关模块以及资源页面。

## 目录结构

- include/: 头文件
- src/: 源码
- resources/: Web 资源
- scripts/: 脚本
- bin/: 编译产物
- obj/: 中间产物

## 主要功能

- 采集摄像头数据
- H.264 编码
- HTTP 服务与页面展示
- RTSP 推流

## 构建

板子上没有 GStreamer 时，直接执行默认构建：

```bash
make clean
make
```

默认构建不会编译 `rtsp_server.cpp`，也不会检测或链接 GStreamer。此时仍然包含 HTTP、线程池、V4L2 采集和 MJPEG。

如果在 PC 或虚拟机里交叉编译，可以让 SDK 直接注入 `CC/CXX`，也可以手动传 `CROSS_COMPILE`：

```bash
make clean
make CROSS_COMPILE=arm-linux-gnueabihf-
```

如果交叉编译器由板厂 SDK 提供，通常更推荐让 SDK 调用本项目 `makefile`，传入 `TARGET_CC`、`TARGET_CXX`、`TARGET_CFLAGS`、`TARGET_LDFLAGS`。项目已经提供 `install` 目标，可以安装到一个临时 rootfs：

```bash
make install DESTDIR=/path/to/rootfs
```

安装后会生成：

```text
/path/to/rootfs/usr/bin/webserver_camera
/path/to/rootfs/usr/share/webserver_camera/
```

程序启动时会优先使用环境变量 `WEBSERVER_CAMERA_RESOURCES` 指定的资源目录；如果没有指定，会先按开发目录结构查找 `../resources`，再回退到 `/usr/share/webserver_camera`。

## 打包进 img

只执行本项目的 `make`，只能证明应用被编译成了可执行文件，不能证明它已经进入最终烧录镜像。完整链路应该是：

```text
源码 -> 交叉编译出 webserver_camera -> 安装进 rootfs -> SDK 打包 rootfs -> 生成 img
```

如果使用 Tina/OpenWrt 风格 SDK，可以参考：

```text
packaging/tina/Makefile
```

接入方式通常是：

```bash
mkdir -p package/allwinner/webserver_camera/src
cp packaging/tina/Makefile package/allwinner/webserver_camera/Makefile
cp -r include src resources tools tests makefile package/allwinner/webserver_camera/src/
make menuconfig
make package/webserver_camera/compile V=s
make
pack
```

具体命令以你的 SDK 为准。关键点是：package Makefile 的 `Package/webserver_camera/install` 阶段必须把程序复制到 `$(1)/usr/bin/`，把网页资源复制到 `$(1)/usr/share/webserver_camera/`。`$(1)` 可以理解成 SDK 正在准备的 rootfs 目录。

判断应用是否真的进了 img，可以用两个办法。烧录后在板子上执行：

```bash
which webserver_camera
find / -name webserver_camera 2>/dev/null
sha256sum /usr/bin/webserver_camera
```

也可以在打包前检查 SDK 的 rootfs 目录：

```bash
find /path/to/rootfs -name webserver_camera
sha256sum /path/to/rootfs/usr/bin/webserver_camera
```

如果 rootfs 或板子里找不到 `/usr/bin/webserver_camera`，那就说明它还没有被安装进镜像；这时不是重新写业务代码，而是修 SDK 的 package Makefile 或安装规则。

MJPEG 地址为：

```text
http://设备IP:端口/mjpeg
```

只有摄像头实际输出 MJPEG/JPEG 帧时该接口才会建立长连接。如果设备回退到 NV12 或 YUYV，接口返回 `503 Service Unavailable`，避免把原始像素错误地当作 JPEG 发送。

安装 GStreamer 开发包后，可以显式启用 RTSP 和 HLS：

```bash
make clean
make ENABLE_GSTREAMER=1
```

## H264 转 FLV

默认构建还会生成离线封装工具 `bin/h264_to_flv`。它读取 Annex-B 格式的 H264 文件，缓存 SPS/PPS，把同一画面的多个 slice 聚合为一个 access unit，再封装成 FLV：

```bash
./bin/h264_to_flv test.h264 out.flv 25
```

最后一个参数是帧率。当前第一版按固定帧率生成时间戳，并假设没有 B 帧，因此 $PTS=DTS$。

媒体解析和封装的单元测试可以这样执行：

```bash
make test-media
```

## HTTP-FLV 服务

默认构建现在也包含 HTTP-FLV 服务，不依赖 GStreamer。HTTP-FLV 需要 H264 Annex-B 输入源；如果只是开发验证，可以把 `.h264` 文件作为第二个参数传给主程序：

```bash
./bin/my_program 10000 test.h264
```

如果不传第二个参数，服务不会默认读取 `test.h264`，`/live.flv` 会保持不可用，`/api/status` 会显示 H264 source 未配置。这样做是为了避免测试文件掩盖真实摄像头或编码器链路的问题。

如果板子上有能直接输出 H264 的 V4L2 设备，可以用 `v4l2:` 前缀切换输入源：

```bash
./bin/my_program 10000 v4l2:/dev/video0
```

这条路径要求设备支持 `V4L2_PIX_FMT_H264`。如果启动日志里枚举的 V4L2 格式没有 `H264`，说明摄像头不能直接给 HTTP-FLV 使用；此时服务会记录错误并保持 `/live.flv` 不可用，而不会自动回退到测试文件。

### MIPI/CSI 和 H264 的关系

MIPI CSI-2 可以理解成“摄像头传感器把图像数据送进 SoC 的高速通道”。它解决的是传输问题，不解决视频压缩编码问题。H264 则是编码后的压缩码流，它通常来自 SoC 内部的视频编码器、VPU、ISP 后面的编码模块，或者一个独立的 USB/IP 编码设备。

所以不要把“MIPI 摄像头”直接等同于“能输出 H264 的视频源”。很多 MIPI 传感器经 V4L2 暴露出来的是 RAW、YUYV、NV12、MJPEG 这类图像格式；只有当某个 V4L2 节点明确支持 `V4L2_PIX_FMT_H264`，这个节点才适合直接接入当前 HTTP-FLV 链路。

如果摄像头只支持 MJPEG/YUYV/NV12，那么正确路线是接入硬件编码器：采集线程拿到原始或压缩图像帧，编码器输出 H264 NAL，HTTP-FLV 层继续复用现在的 `IH264Source -> FlvMuxer -> StreamManager -> http_conn` 分发链路。

注意：如果用 `v4l2:/dev/video0` 作为 HTTP-FLV 的 H264 源，主程序会跳过原来的 MJPEG 采集线程，避免两个线程同时打开同一个 V4L2 设备。

HTTP-FLV 播放地址为：

```text
http://设备IP:端口/live.flv
```

可以用 `ffplay` 验证：

```bash
ffplay http://设备IP:端口/live.flv
```

浏览器不能像播放 MP4 那样直接播放 FLV 文件。项目页面通过 flv.js 把 HTTP-FLV 长连接里的 FLV/H264 数据解析出来，再交给浏览器的 MediaSource Extensions 播放。首页 `/index.html` 和 `/monitor.html` 都已经内置 HTTP-FLV 播放入口。如果 flv.js CDN 加载失败，或者浏览器不支持 MSE live FLV，页面会显示明确错误；这种情况下可以先用 `ffplay` 验证服务端 `/live.flv` 是否正常。

控制 API：

```text
GET /api/status
GET /api/start_flv
GET /api/stop_flv
```

`/api/status` 会返回 MJPEG 和 FLV 的客户端数量、FLV 输入源运行状态、当前实际使用的输入源、是否已经拿到 SPS/PPS、发送帧数、关键帧数、丢包数等统计。

## 运行

根据实际平台与设备节点运行生成的可执行文件。示例：

```bash
./bin/my_program 10000
```

## 备注

该工程依赖平台相关的 V4L2 设备与编码器实现，具体适配请参考源码与平台文档。
