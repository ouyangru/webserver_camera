# V853 编译与部署

日常修改应用时，优先单独交叉编译，通过 ADB 上传。需要集成固件时使用 Tina 包构建。
两种入口共用 `app.mk`；根目录 `Makefile` 专用于 OpenWrt 包定义。
不要在包根目录重新创建小写 `makefile`，GNU Make 会优先读取它，导致包扫描失败。

## 单独交叉编译（推荐）

在本项目目录执行：

```bash
bash scripts/build-v853.sh
```

自动查找父目录中的 Tina SDK，使用 SDK 自带 ARM hard-float / musl 工具链。
项目移出 SDK 后可显式指定：

```bash
TINA_SDK_ROOT=/path/to/tina-v853-open bash scripts/build-v853.sh
```

输出为 `build-v853/bin/webserver_camera`、`build-v853/bin/h264_to_flv`、
`build-v853/resources/`。每次重新编译，目标文件保存在独立目录中。
当前脚本关闭 GStreamer，默认编译 HTTP/MJPEG/HTTP-FLV 应用路径。

若 ADB 设备连接在 Ubuntu，且板上 `/tmp` 可写，可在项目目录执行：

```bash
adb devices
adb shell 'mkdir -p /tmp/webserver-camera/bin'
adb push build-v853/bin/webserver_camera /tmp/webserver-camera/bin/webserver_camera
adb push build-v853/resources /tmp/webserver-camera/
adb shell 'chmod +x /tmp/webserver-camera/bin/webserver_camera; /tmp/webserver-camera/bin/webserver_camera 10000'
```

在另一个 Ubuntu 终端执行 `adb forward tcp:10000 tcp:10000`，
再从 Ubuntu 浏览器访问 `http://127.0.0.1:10000/index.html`。
`/tmp` 内容重启后可能丢失；部署前需确认板上 musl 与 C++ 运行库兼容。
以上是操作步骤，编译验证不代表已经完成开发板摄像头运行验证。

## Tina 包及固件集成

```bash
cd /home/aosika/Desktop/workspace/tina-v853-open
source .buildconfig
make -C openwrt/openwrt package/webserver_camera/clean V=s
make -C openwrt/openwrt package/webserver_camera/compile V=s
```

保持 V853 Vision 配置中 `CONFIG_PACKAGE_webserver_camera=y`。
包级构建生成 IPK 并安装到目标 staging 根目录；完整固件仍需执行 SDK 的系统构建和打包流程。
固件安装路径为 `/usr/bin/webserver_camera` 和 `/usr/share/webserver_camera/`。

## 本地 SDK 依赖

Git 分支包含应用代码和依赖来源说明。板端构建需要从相应来源补齐 `vendor/cedarc-v853/PROVENANCE.md` 和 `vendor/100ask-cv/SOURCE.md`、`vendor/100ask-cv/yolo/SOURCE.md` 所列的本地依赖。百问网包中的 `100ASK_Algo` 和 `sdk_release` 保持原目录结构放到 `vendor/100ask-cv/`，YOLO 模型放到 `vendor/100ask-cv/yolo/yolov5_model.nb`。Cedar 源码与 musl 库按构建脚本所用路径放到 `vendor/cedarc-v853/`。
