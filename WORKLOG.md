# WORKLOG

工业迭代的关键决策、踩过的坑与验证记录。最新在上。
`git log` 有完整「做了什么」，这里只补「为什么」「怎么验证」「哪里差点被骗」。

---

## 2026-07-23 · 网络健壮性：CL-SEND + RTMP-SPIN（`8ceeaf4`）

- **CL-SEND**：`RtspClient::sendRequest` 控制面用裸阻塞 `send()`（无写超时）→ 改
  `sendAll(..., recv_timeout_ms)`，与 server 侧一致。真实但边缘：RTSP 请求小、一次
  进 send buffer 就返回；只有对端接收窗口满（卡死的 server）时裸 send 才无限阻塞，
  拖垮 `close()/teardown()` 的契约超时。
- **RTMP-SPIN**：`rtmp_handshake.cpp` 的 `recvExact` 与 `rtmp_publisher.cpp` 的
  `waitForCommandResult` 原本把 `recv<0` 一律当「超时重试」→ 改用 `waitReadable`
  分离「超时(poll==0，继续)」与「硬错误(POLLERR/POLLHUP，立即退)」。

- ⚠️ **重要发现（避免误导后人）**：记忆/审计里把 RTMP-SPIN 记成「对端 RST → 100%
  CPU 忙等满 timeout(10s)」。**实测在 Linux 上不成立**：RST 后 `recv` 第一次返回
  `-1/ECONNRESET`、**下一次立即返回 `0`**，旧代码经 `recv==0` 的 "peer closed"
  分支在 2 次迭代(~200ms)内退出，并不会忙等。修复的真实价值是：①不依赖
  「ECONNRESET 之后 recv 返回 0」这一平台特定行为（部分 BSD/macOS 未必如此，那些
  平台上旧代码才会真忙等）；②少一次无谓迭代 + 错误信息更准。**不是** critical spin 修复。
- 🪤 **差点被骗**：最初写的「能捕获 spin」端到端测试，把修复回退后**旧代码也一样
  ~200ms 通过** —— 典型 happy-path 假绿。靠「回退修复重跑 + 独立诊断小程序打印
  recv 返回序列」才戳穿。最终把 `tests/test_rtmp_bad_peer.cpp` 如实降级为「坏-peer
  冒烟」（posix-only，裸 socket mock RST）：验证 `open()` 对 RST peer 有限时间失败、
  不崩溃；注释明说它在 Linux 无法区分本次修复，只守「未来引入真 hang」+「跨平台」。
  另修掉了该测试脚手架自身的 TSan 竞争（`poll` 轮询 accept + join-before-close）。
- ✅ 验证：Debug / ASan / TSan(CI 配置) 各 18/18。

## 2026-07-23 · 帧共享优化 + 客户端端口撞车 + 测试固化（`a5930e7`..`89c4f6e`）

- **OPT-1**：`broadcastFrame` 原本给 `latest_frame` 缓存和每个 player session 各深拷
  一次帧 → 改为只 clone 一次 managed `VideoFrame`，`shared_ptr` 同时共享给缓存和每个
  `session->pushFrame`。发布后 `managed_data` 只读，TSan(CI 配置) 净。（NALU 只解析
  一次那半**未做**，留作 OPT。）
- **C-RTPPORT**（新发现，不在旧审计清单）：`Socket::bindUdp` 恒设 `SO_REUSEADDR`；
  Linux 下 UDP+REUSEADDR 允许两 socket 绑同 `0.0.0.0:port`，包只进一个 → 同机多个
  `RtspClient` 都绑 RTP 20000、都报 `client_port=20000`，只有一个收到帧。修：`bindUdp`
  加 `reuse_addr` 形参（默认 true，server/多播不变），client RTP/RTCP 接收口用
  `reuse_addr=false`，二次绑失败 → SETUP 循环换下一端口。**实测 1/3 → 3/3 并发拉流。**
  根因先用 Python 双绑复现确认。
- **测试固化**：`tests/test_dos_hardening.cpp`（AMF0 嵌套深度 / RTMP msg_len / csid 上限，
  纯函数）；`rtsp-cli-smoke` 加多消费者 CI leg（3 并发 rtsp-cli 拉流各自验帧 + ffmpeg
  交叉，动态端口/就绪轮询/PID 数组 kill/逐消费者帧数断言）。
- ✅ 验证：Debug/ASan/TSan 18/18 + 多消费者本地 3/3@101 帧。

## 2026-07-23 · DoS 加固 + rtsp-cli 双修（`26f6328`、`67736a5`）

- **DoS/NTP**：RTMP chunk msg_len 8MB 上限 + buffer 上限 + csid≤64；AMF0 `parseValue`
  递归深度≤32；`recvRtspMessage`/client `recvRtspResponse` 的 Content-Length(2MB)/
  header(64KB) 上限；`frame_buffer_` 8MB 上限（server+client 单NAL/FU-A/FU-HEVC 追加
  路径）；RTCP SR NTP 小数位从纳秒填充（原恒 0）。
- **rtsp-cli**：`Mp4Writer::create` 错误路径 double-free/double-fclose；`push` 总返回 0
  掩盖 CI 失败 → 改成 rtmp 断连/0 帧返回非零。

## 2026-06-10 · 传输兼容 + ONVIF + Publisher 鉴权（PR #6 / #7）

- **PR #6**：R-DNS（getaddrinfo，原全项目无 DNS 解析，推真实 CDN 域名直接废）、
  C-SR（RTCP SR 声明长度 vs 实发不符 → 严格客户端丢 SR/坏 A/V 同步）、C-HEVC（hvcC
  PTL 从 `s[1]` 应为 `s[3]`）、C-NAL（Annex-B 末尾短 NAL 边界）、O-XML（ONVIF 全程无
  XML 转义 → 反射注入）、O-BODY（SOAP body 无上限）。
- **PR #7**：P-200OK（publisher 用 `find("200 OK")` 判成功 → 改状态行解析）、
  P-AUTH（publisher 实现 Digest 401 重试）、N6（player SETUP 锁范围）、N8（publisher
  RTP 源 IP 校验）。

---

## Backlog（非必修，按需——「真遇到问题再说」）

必修项已清零。剩余都是打磨/看场景，2026-06-10 已定「停机械扫清单、由真实部署反馈排序」：

- **T-CLOSE**：socket teardown 的既有 TSan 竞争（`socket.cpp` 跨线程 close vs bind/
  accept）。根治后可删 `ci/tsan.supp` 里对 test 文件的抑制。不依赖外部场景。
- **N1/N2**：OPTIONS `Public` 漏 ANNOUNCE/RECORD/GET_PARAMETER；PLAY 响应缺 `RTP-Info`。
- **H4**：client TCP interleaved 下 GET_PARAMETER/PAUSE/TEARDOWN 仍「停收线程→发→重启」，
  应改单收线程多路分发。
- **N7/N6c**：client H.264 FU-A 丢包无 resync（H.265 有）；RTP 重排序 seq 回绕比较。
- **OPT-2/OPT-3 + OPT-1 余项**：RtpPacket 每包裸 new/delete；RTMP 每帧 2-3 次拷贝；
  OPT-1 的 NALU-只解析一次。
