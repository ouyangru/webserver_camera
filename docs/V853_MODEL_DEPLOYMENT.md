# V853 模型部署说明与新聊天交接

更新日期：2026-11-02。第一版人形检测已完成板端真实 NPU 推理与摄像头接入。下文保留后续迭代步骤，已实施结果以本节为准。

**最新版本已扩展为直播页开关、严格同帧叠加和YOLOv5 COCO80多类别检测。当前使用与验证以 [CV_SYNC_MULTICLASS.md](CV_SYNC_MULTICLASS.md) 为准；下文第一版5fps、独立cv.html及最新结果叠加描述是历史阶段。现在打开首页index.html控制，cv.html自动跳转首页，开机默认关闭检测。**

## 0. 已交付的人形检测第一版

最终实测与验收边界见 [V853_CV_VALIDATION.md](V853_CV_VALIDATION.md)：重启自启动、60秒采样、静态/旋转图片、停止推理隔离和FLV解码均已通过。

模型：百问网配套 `100ASK_humandet_384_224_V1.nb`，使用同一资源包的 AWNN 与 VIPLite 1.8.0 运行库，静态链接到独立程序 `webserver_cv`。来源与归档校验值见 `vendor/100ask-cv/SOURCE.md`。供应商提供的模型和库不代表本项目自行训练或自行编写；公开再分发前需核对授权。

打开 `http://127.0.0.1:18080/cv.html` 查看实时 FLV、最新人形框和推理指标；首页 Quick Links 中也有入口。该模型只做人形检测，不是任意物体识别，也不做人脸身份识别。近距离只有头部、身体被遮挡或侧倒时可能漏检，实时场景准确率尚未系统评估。

实现结构：采集线程以最多 5fps 将应用拥有的紧凑 NV12 写入 `/tmp/webserver_cv.frame` 的单槽；独立推理进程读出后立即释放文件锁，在自有缓冲中预处理和推理。采集使用非阻塞锁，遇到竞争跳过发布，推流不等待 NPU。单槽覆写旧的未处理帧，不建立无界队列，不在 QBUF 后保留驱动帧指针。

输入保持宽高比缩放到 384×216 内容区，补到模型 384×224 存储；输出框去掉 padding、逆旋转并映射回原图坐标。预处理先采用最近邻以控制成本，后续可用精度/延迟对照评估更高质量缩放。

当前安装方向默认 `CV_ROTATE=270`（顺时针角度），只校正推理输入，不旋转原视频。恢复正常安装后，在板端 `/etc/webserver_cv.conf` 写 `export CV_ROTATE=0`，再重启推理服务；支持 0/90/180/270。网页会显示生效角度。

`/api/status` 新增 `inference`：`available` 仅表示结果更新距今不足 2 秒，不是精度保证；`result_age_ms` 是结果发布距今，`result.frame_age_ms` 是采集出队到结果生成的板端时间。结果带 frame_id、采集/完成单调时间、模型、方向、框、实际推理 fps、阶段耗时、进程 CPU/RSS 及请求的 NPU heap 字节数。帧龄不包含浏览器延迟；NPU heap 字段不是全部 NPU 内存实测值。

页面使用最新结果叠加，尚未与视频逐帧同步，运动时存在框与画面错位；过期结果停止画框。worker 停止/失败时视频继续工作，状态过期后标为不可用；当前服务没有崩溃自动拉起，需要重启服务，不能声称具备完整生产级故障恢复。

板端控制：

```sh
/etc/init.d/webserver_cv stop
/etc/init.d/webserver_cv start
cat /tmp/webserver_cv.json
cat /tmp/webserver_cv.log
```

Tina 的 rc.common 将动作放在后台执行，远程脚本中调用 start/stop 后需等待动作完成再检查，避免把异步执行误判为失败。推理日志约每 5 秒一条，1MiB 轮转至 `.1`；记录单调时间、推理数、耗时、人数、RSS 和 CPU。

独立构建 `bash scripts/build-v853.sh` 会同时构建视频程序和推理程序；也可单独运行 `bash scripts/build-cv.sh`。Tina 包定义已补齐推理程序、模型、初始化脚本及 S96 开机入口。供应商推理库静态链接，不需要向系统 `/usr/lib` 覆盖同名库。

产物部署对应关系：

| 编译产物 / 项目文件 | 板端位置 |
| --- | --- |
| build-v853/bin/webserver_camera | /usr/bin/webserver_camera |
| build-v853/bin/webserver_cv | /usr/bin/webserver_cv |
| vendor/100ask-cv/100ASK_Algo/pdet/100ASK_humandet_384_224_V1.nb | /usr/share/webserver_camera/models/100ASK_humandet_384_224_V1.nb |
| resources/cv.html、resources/index.html | /usr/share/webserver_camera/ 对应文件 |
| scripts/webserver_cv.init | /etc/init.d/webserver_cv，开机链接 S96webserver_cv |

`webserver_camera` 启动环境需有 `CV_ENABLE=1`，初始化脚本已设置；手动启动漏设时不会发布帧。停止推理只需停止 worker；要完全关闭帧发布开销，去掉该环境变量并重启视频服务。

静态图片回归命令（先停止现有 worker，避免两进程争用单个 NPU 初始化）：

```sh
CV_ROTATE=0 /usr/bin/webserver_cv MODEL.nb INPUT.nv12 384 216
```

输入必须是指定尺寸的紧凑 NV12，大小严格为宽×高×3/2；输出 `RESULT_JSON=` 行给出真实检测框。离线模式会暂时更新相同状态文件，测试后启动实时 worker。

已验证：原始官方测试图片和顺时针旋转 90° 后再以 CV_ROTATE=270 校正的图片，各检测出 2 个人，框映射正确；本项目编译的 worker 单次 NPU 约 12.4ms。实时链路短时约 5fps 推理、约 20fps 视频；摄像头进程 CPU 约 10%，推理进程 RSS 约 4.9MiB。方向校正后的 CPU、最终版本和重启结果需按随附最终验证记录判断，不能将短时样本当作长期稳定性或精度验收。

已通过主机 100 次 HTTP 请求、FLV 头和本地播放器资源冒烟；主机无 JPEG 开发头，主机冒烟以 ENABLE_JPEG=0 构建，板端交叉编译启用 JPEG 和 Cedar。停止 worker 后状态不可用、FLV 源仍 ready 的隔离测试通过。

## 1. 目标与优先级

优先单独交叉编译、通过 Windows ADB 部署调试，同时保持 Tina/OpenWrt 打包完整固件的能力。
先用 SDK 配套示例验证 NPU，再用选定模型完成静态图片推理，最后异步接入摄像头。视频播放应持续运行，不能每来一帧都等待模型结果。

第一阶段不强行在编码前画框，先提供检测结果 JSON 和网页叠加；如果以后要求录像本身带框，再评估编码前绘制与同步成本。

## 2. 开发位置与连接

Ubuntu VM：`aosika@192.168.149.132`，实际地址重连时需确认。

SDK 根目录：
`/home/aosika/Desktop/workspace/tina-v853-open`

项目根目录：
`/home/aosika/Desktop/workspace/tina-v853-open/openwrt/package/allwinner/vision/webserver_camera`

当前工作分支：`optimize/v853-latency-metrics`。工作区包含此前硬件修复和日志/延迟优化，尚未统一提交或推送；不要 reset、clean 或强制覆盖。日志分支 `refactor/stream-logging-stage1` 的最终净差异已应用，不能说已经创建合并提交。

Windows 编辑副本：
`C:\Users\aosika\Documents\Codex\2026-10-31\referenced-chatgpt-conversation-this-is-an\work\optimization`

Windows ADB：`D:\Android\Sdk\platform-tools\adb.exe`，板子序列号 `20080411`。USB 默认由 Windows 接管，Ubuntu VM 不一定能直接看到板子。

Windows PowerShell：

```powershell
& D:\Android\Sdk\platform-tools\adb.exe devices -l
& D:\Android\Sdk\platform-tools\adb.exe -s 20080411 shell
& D:\Android\Sdk\platform-tools\adb.exe -s 20080411 forward tcp:18080 tcp:10000
```

浏览器访问 `http://127.0.0.1:18080/index.html`，状态 `http://127.0.0.1:18080/api/status`。板端程序监听 10000；局域网连接后使用板子 IP 与 10000 端口，不使用电脑的 127.0.0.1。

## 3. 已确认与待验证的边界

| 项目 | 状态 |
| --- | --- |
| 板卡 | 照片丝印 AICT-V853 BTB，百问网，底板 V1.1 |
| 系统 | 本次读取为 Tina Linux 4.9.191 #2，armv7l |
| 内存 | 本次 `/proc/meminfo`：MemTotal 503712kB、MemAvailable 462928kB；这是一次快照，不是模型可用内存保证 |
| 摄像头链路 | GC2053 / VIN 多平面采集 / NV12 / ISP / Cedar H.264 / HTTP-FLV 已跑通 |
| NPU 设备 | 本次 ADB 确认 `/dev/vipcore` 存在 |
| NPU 用户库和工具 | 已取得并静态链接配套 AWNN/VIPLite，已执行真实推理；通用模型转换工具尚未部署 |
| 模型与任务 | 首次部署选择配套轻量人形检测模型，固定图片与实时摄像头已运行；后续自定义模型任务和精度目标待明确 |
| 联网 | 替换匹配 AICT 的默认 XR829 SDD 后已连接热点，曾获 IP 192.168.191.236；当前优先 USB，IP 可能变化 |

NPU 设备节点、用户态运行库、模型转换工具、编译后的模型格式必须匹配。现有视频编码 IOMMU 修复只说明 Cedar 链路可用，不能直接套用为 NPU 已验证。

本次还在 `/home/aosika/Desktop/07_Tina_SDK/tina-v853-open/kernel/linux-4.9/include/npu/` 找到 `vip_lite.h` 和 `vip_lite_common.h`；这是进一步定位驱动接口的线索，不等于已取得完整用户态推理 SDK。

## 4. 现有代码与需要保护的行为

- `src/v4l2_capture_multi.cpp`：DQBUF、格式/stride 处理、NV12 拷贝、硬编码和按需 JPEG。推理取帧应在现有格式归一化之后接出，复用已有采集，不另开同一摄像头争抢设备。
- `src/h264_encoder.cpp`、`scripts/build-cedar-runtime.sh`：Cedar 硬编码及私有运行库；保留 `CONF_USE_IOMMU=1` 等已验证适配。
- `src/stream_manager.cpp`：实时流组帧和时间戳；保留 AUD 及时 flush、SPS/PPS 去重和实时源不重复 sleep。
- `src/http_conn.cpp`、`src/main.cpp`：HTTP、FLV、连接与背压；不要清空已发送一部分的 FLV tag。
- `include/pipeline_metrics.h`、`src/pipeline_metrics.cpp`：`perf_now_us`、`perf_observe`、`perf_count` 和状态输出。
- `resources/index.html`：本地播放器与性能卡片；`docs/V853_OPTIMIZATION.md` 记录此前修复理由。
- `scripts/build-v853.sh`、`app.mk`：独立交叉编译；根目录 `Makefile` 是 Tina 包定义。

板端：`/usr/bin/webserver_camera`，运行库 `/usr/lib/webserver_camera`，网页 `/usr/share/webserver_camera`。
开机入口 `/etc/init.d/S95webserver_camera` 指向 `webserver_camera`；不能只按普通 OpenWrt 的 rc.d 规则判断。
日志 `/tmp/webserver_camera.log`，1MiB 轮转到 `.1`；重启后临时日志可能消失，测试前后及时拉取。

## 5. 实施步骤与通过条件

### A. 先盘点 SDK 推理环境

1. 检查 `/dev/vipcore`、相关驱动日志及内核模块；记录驱动版本和错误。
2. 在当前 SDK、用户的 `07_Tina_SDK` 和随板资料中精确查找 NPU 示例、头文件、用户库、模型转换工具和许可证说明。不要下载名字相似但平台不同的二进制来替代。
3. 对库和示例执行 `file`、`readelf -h/-d`，检查 ARM 架构、浮点 ABI、动态加载器、libc 和依赖。项目使用 musl 工具链；供应商若只提供另一种 libc 的库，需要明确兼容方案，不能仅改 CC 蒙混通过。
4. 先运行供应商自带、模型与运行库成套的示例，保存命令、版本、模型校验值、结果和耗时。

通过条件：设备实际执行一次推理且结果可验证；设备节点存在或程序能启动不算通过。若配套文件确实缺失，列出缺少的具体文件及应向板卡供应商索取的内容，再继续不依赖这些文件的框架工作。

### B. 确定模型并离线验收

模型至少需要记录：用途/类别、来源/许可、原始文件 SHA256、输入尺寸、布局、RGB/BGR、归一化、量化信息、输出张量含义、后处理和阈值。
先问清具体任务；无指定任务时可以提出轻量检测模型候选，但不要把候选直接当成已获验证的硬件支持列表。

按该 SDK 配套转换工具支持的输入格式、算子和版本转换；必要时准备有代表性的量化校准集。ONNX 能在 PC 上执行不等于板端运行库能直接载入。
先用固定测试图片比较转换前后结果，覆盖有目标、无目标、多目标、暗光与边缘目标；核对缩放/letterbox 逆映射和 NMS。

通过条件：静态图片结果正确，误差有记录，板上稳定重复运行，没有持续增长的内存或驱动报错。

### C. 接入摄像头，但不阻塞推流

建议结构：

```text
VIN 采集 → 原有 NV12 归一化 → Cedar 编码 → FLV → 浏览器
                     ↓ 只在到达推理节拍时复制到自有缓冲
                最新帧槽（容量 1，覆盖旧的待处理帧）
                     ↓ 独立推理线程
              缩放/颜色转换 → NPU → 后处理 → 最新结果
```

第一版使用自有缓冲复制以明确生命周期。禁止把 DQBUF 得到的 mmap 指针在 QBUF 后继续交给推理线程；驱动会复用其内容。
生产者只能覆盖“待处理”槽，不能覆盖推理线程正在读取的缓冲。建议固定 2～3 个预分配缓冲，锁内交换所有权，锁外预处理/推理；停止时唤醒线程并 join。
零拷贝留待后续：必须先确认运行库的 DMA 导入、stride、cache 同步和所有权约束，不能仅传物理地址。

起步推理率 3～5fps，独立于实际约 20fps 的采集。模型慢时覆盖旧待处理帧，不能无限排队；开关关闭、模型加载失败或推理失败时，视频应继续，状态中明确错误。
结果携带 frame_id、采集单调时间、推理开始/结束、源图尺寸和坐标转换信息；网页可显示结果年龄。简单最新结果叠加会有时间偏差，不声称帧级精确同步。

### D. 接口与监控（拟新增，当前未实现）

建议新增 `inference_enabled`、`model_loaded`、模型版本、输入尺寸、实际推理 fps、最新结果年龄及错误信息。
建议阶段指标 `infer_preprocess`、`infer_run`、`infer_postprocess`、`infer_total`、`infer_frame_age`，计数 `infer_drop` 和 `infer_error`；复用现有单调时钟与聚合指标。
结果 API 路径、模型配置项及文件命名应在实现时确定，文档中的建议不能当作已有接口调用。

同时测 CPU、RSS、MemAvailable、推流 fps、编码/队列 P95，以及模型加载和第一次推理前后内存；RSS 不涵盖全部 DMA/ION/NPU 内存。
不要逐帧刷日志，不在状态接口暴露凭据。

### E. 构建、部署与回滚

Ubuntu 项目根目录独立编译现有程序：

```bash
bash scripts/build-v853.sh
```

当前产物为 `build-v853/bin/webserver_camera`、`build-v853/lib/` 与资源目录。新增模型依赖后，必须同时更新独立构建和 Tina 包的依赖/install；明确运行库、模型、配置各自安装位置。
不要把 PC 转换工具或整个 Python 环境打入板端，也不要把模型文件随每次网页更新反复上传。

部署前保存现有程序、库、配置及模型清单；先停止服务，上传匹配的一整套文件，再启动并核对 `/api/status` 和日志。
回滚应恢复整套匹配版本。首次模型验证优先单独 ADB 部署，不需要每轮都刷完整固件；最后再做完整包/固件和重启验证。

## 6. 性能验收与风险

此前仅 FLV 的参考数据：CPU 约 7%～11%，RSS 约 5.8～5.9MiB，实际编码约 19.5～20fps；编码 P95 约 4.7～7.3ms。这是先前短时样本，需要在新模型测试同一分辨率、观众数、码率和网络条件下重新测量。
MJPEG 有观众时会启动软件 JPEG，CPU 显著升高，不能与仅 FLV 的基线混比。

至少对比：关闭推理 / 只加载模型 / 开启推理；记录 3～5fps 和逐步提高频率的效果。
每种配置做至少 10 分钟稳定性观察，最后做 30 分钟持续推流与推理、客户端重连和开关推理测试。记录丢帧、错误、峰值内存及推理失败时视频是否继续。
第一版以持续可用、队列不积压为门槛，具体精度和延迟目标待选模型后确定，不预先承诺固定 fps。
浏览器 bufferAhead 和 ping 都不是完整端到端延迟；拍摄同一时钟/物理事件测量视频延迟。NPU 与视频硬编码分工不同，也可能争用内存带宽和缓冲，必须实测，不能保证部署模型后延迟不变。

## 7. 联网与当前未完成事项

用户已在当前聊天提供热点信息；凭据不写入本文或 Git。热点需支持 2.4GHz，板子与 PC 需在可互通网络中。联网上线不是本地 ADB 推理验证的前置条件。
XR829 默认 SDD 对照测试已让 wlan0 出现；完整固件构建配置曾被 defconfig 重新选回 40M SDD，**此项尚待修正并核对最终镜像的固件哈希**。不要把旧镜像声称为已含联网修复。
配套人形模型和运行库已部署；自定义模型转换工具、最终完整镜像验收及长期稳定性/精度测试仍待完成。新聊天应先核实现场，不假设所有部署阶段均已完成。

## 8. 可复制到新聊天的启动说明

请先读取项目 `docs/V853_MODEL_DEPLOYMENT.md` 和 `docs/V853_OPTIMIZATION.md`，核对实际工作区与板端状态，在保留未提交改动的前提下继续完善 V853 模型部署。第一版 NPU 人形检测已通过独立 worker、单槽 NV12 IPC 和 cv.html 接入；先验证已有模型，再按新的任务迭代。优先独立交叉编译和 Windows ADB 部署，保留 Tina 固件兼容。继续完善准确率、视频结果同步、长期稳定性和自定义模型转换；不要阻塞现有硬件 H.264/FLV。将实现理由和实测结果继续记录在项目文档中。
