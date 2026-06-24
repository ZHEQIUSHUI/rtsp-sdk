# rtsp-cli

一个类 ffmpeg 的命令行工具，基于本 SDK，做 RTSP 拉流 / 推流，带实时刷新的状态行。

## 构建

随主工程构建（`BUILD_APPS=ON`，默认开）：

```
cmake -S . -B build -DBUILD_APPS=ON
cmake --build build --target rtsp-cli
# 产物: build/apps/rtsp-cli/rtsp-cli
```

## 用法

```
rtsp-cli pull -i rtsp://host/path [-o out.mp4]
rtsp-cli push -i in.mp4 -o rtsp://host/path [-r -1]
rtsp-cli help [pull|push]
```

`rtsp-cli help` 查看分组参数；`rtsp-cli help pull` / `pull --help` 看单个命令。

### pull — 拉流
连接 → 打印流信息块 → 接收并在底部单行实时刷新（时长 / 帧数 / fps / 码率 / 累计字节 / 丢包 / 传输），`-o` 可选保存。

```
rtsp-cli pull -i rtsp://127.0.0.1:8554/live -o out.mp4
rtsp-cli pull -i rtsp://cam/stream --transport tcp -t 30      # TCP，拉 30s
rtsp-cli pull -i rtsp://cam/stream -o - > raw.h264            # 裸流到 stdout
```

### push — 推流
读本地视频 → 按目标协议：
- `rtsp://`：端口**无服务器则自起一个托管**（消费者来拉）、**已有则 ANNOUNCE/RECORD 推给它**；端口被非 RTSP 服务占用（端口冲突）则报错，不强行。
- `rtmp://`：用 RTMP 推给目标服务器/CDN（B 站/抖音/YouTube/mediamtx 等）。**RTMP 无法本地自起**，对端不在线直接报错。

`-r` 控制循环（-1 无限 / 0|1 一次 / N 次）。

```
rtsp-cli push -i in.mp4   -o rtsp://127.0.0.1:8554/live -r -1   # 自起 server 无限循环
rtsp-cli push -i in.h264  -o rtsp://media-server:8554/live      # 推给已有 RTSP server
rtsp-cli push -i in.mp4   -o rtsp://srv/live --auth user:pass   # Digest 鉴权
rtsp-cli push -i in.mp4   -o rtmp://127.0.0.1:1935/live/key     # 推到 RTMP 服务器/CDN
```

> 注：`pull` 只支持 `rtsp://`（SDK 无 RTMP 收流端）。

## 容器 / 编码支持

| 格式 | 拉流保存(-o) | 推流读取(-i) |
|---|---|---|
| `.h264`/`.h265` 裸流 (Annex-B) | ✅ | ✅ |
| `-` (stdout 裸流) | ✅ | — |
| `.mp4` H.264 | ✅ | ✅ |
| `.mp4` H.265 | ✅ (写) | best-effort |

- MP4 封装/解封装用内置的 `third_party/minimp4.h`（CC0，零运行时依赖）。
- 仅视频（H.264/H.265）；MP4 的音频轨被忽略。

## 已知限制

- 裸流输入无法得知分辨率（裸流不含可解析的尺寸来源），信息块/服务端 SDP 的宽高可能显示为占位值；不影响实际播放（SPS 内含真实尺寸）。
- 拉流存 MP4 时，落在首个关键帧之前的帧无法写入（MP4 需要从关键帧起头），属正常。
- H.265 的 MP4 读取依赖容器内的参数集，标记 best-effort。
