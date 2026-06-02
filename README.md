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

在该目录下执行：

```bash
make
```

## 运行

根据实际平台与设备节点运行生成的可执行文件。示例：

```bash
./bin/my_program
```

## 备注

该工程依赖平台相关的 V4L2 设备与编码器实现，具体适配请参考源码与平台文档。
