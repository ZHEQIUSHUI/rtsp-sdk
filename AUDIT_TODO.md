# RTSP SDK 工业化改进任务清单

审查基线：`master @ 28be918`（2026-04-21）
审查范围：`src/server/`、`src/client/`、`src/publisher/`、`src/rtsp-common/`

> 图例：`[ ]` 未开始　`[~]` 进行中　`[x]` 已完成
>
> 每条任务标注：**文件位置**、**问题**、**修复方案**、**建议验证方式**

---

## P0 — 阻塞工业落地（死锁、崩溃、静默数据损坏）

### C1. `MediaPath` 锁序倒置导致死锁
- [ ] 位置：`src/server/rtsp_server.cpp:910-938`
- 问题：
  - `broadcastFrame` 先 `latest_frame_mutex` 再 `sessions_mutex`
  - `addSession` 先 `sessions_mutex` 再 `latest_frame_mutex`
  - AB-BA 锁序倒置，SETUP 并发推帧必然偶发死锁
- 修复：
  - 统一锁序：永远先 `sessions_mutex` 再 `latest_frame_mutex`
  - 或：`addSession` 里把"推最新 IDR"操作改为：先拿 `latest_frame_mutex` 拷贝帧到本地，释放后再 `pushFrame`
- 验证：
  - TSan 跑 `test_soak.sh --duration 120 --transport tcp` 并发 SETUP
  - 新增单测：多线程并发 broadcast + addSession

### C2. `cleanup_loop` 持双锁 join 发送线程 → 整 server 挂死
- [ ] 位置：`src/server/rtsp_server.cpp:1736-1758`
- 问题：
  - cleanup 持 `paths_mutex_` + `path->sessions_mutex` 时调用 `session->stop()` → `send_thread.join()`
  - TCP interleaved 模式下 `send_thread` 里的 `control_socket->send()`（`line 865`）在客户端不读时阻塞 → 整 server 所有请求、推流、清理全挂死
- 修复：
  1. 临界区内仅采集过期 session 的 `shared_ptr` 到局部 `vector`，释放锁后再 `stop()`
  2. 给 `control_socket->send()` 加写超时（非阻塞 send + poll，或 `SO_SNDTIMEO`），超时即关闭该 socket
  3. 进一步：给 `ClientSession::stop()` 的 `send_thread.join()` 包一层 `joinThreadWithTimeout`（模板已在 file 中存在）
- 验证：
  - 恶劣客户端测试：`nc host 8554`，发完 SETUP/PLAY 不读，观察 server 是否能正常服务其他连接
  - TSan 下 `test_soak.sh`

### C3. `RtspConnection::handle` 结尾无锁访问 `paths_`
- [ ] 位置：`src/server/rtsp_server.cpp:1061-1070`
- 问题：直接 `paths_.find(...)` 未持 `paths_mutex_`，与 `addPath`/`removePath`/清理线程构成 `std::map` 数据竞争，可 segv
- 修复：
  - 加 `std::lock_guard<std::mutex> lock(paths_mutex_);` 包住 find + removeSession
  - 和 `handleTeardown`（line 1534-1540）保持一致的模式：锁内仅拿 `shared_ptr`，锁外再 `removeSession`
- 验证：TSan soak

### C4. RTCP SR 的 SSRC 与 RTP 包 SSRC 不一致
- [ ] 位置：
  - `src/rtsp-common/rtp_packer.cpp:565` 写死 `ssrc = 0x12345678`
  - `src/server/rtsp_server.cpp:1321` RTP 用 `0x12345678 + hash(session_id)`
- 问题：RTCP SR 永远携带错误 SSRC，严格客户端（live555、GStreamer）忽略/断开，A/V 同步失效
- 修复：
  1. `RtpSender` 增加 `setSsrc(uint32_t)` 或在 `setPeer` 时传入
  2. `sendSenderReport` 使用保存的 SSRC
  3. `handlePlayerSetup` 构造完 `rtp_packer` 后把相同 SSRC 也写进 `rtp_sender`
- 验证：`Wireshark` 抓包对比 RTP 与 RTCP SR 的 SSRC 字段

### C5. `PathConfig.sps/pps/vps` 的读写数据竞争
- [ ] 位置：
  - 写：`src/server/rtsp_server.cpp:1414-1424`（RTP 接收线程回调里 `autoExtract*ParameterSets(path->config, ...)`）
  - 读：`src/server/rtsp_server.cpp:1175`（`handleDescribe` 读 `config.sps.data()`）
- 问题：两条路径并发写读 `std::vector<uint8_t>`，UB，TSan 必报
- 修复：
  - 给 `MediaPath` 加 `std::mutex config_mutex_`
  - `autoExtract*` / DESCRIBE / SETUP 读 codec 信息都在此锁下
  - 或 RTP 回调里先 copy config 副本到本地再 `broadcastFrame`
- 验证：TSan soak（ANNOUNCE+RECORD 推流路径 + 同时 DESCRIBE）

### H1. `getNextRtpPort` 非原子 → 多会话共享同一 RTP socket
- [ ] 位置：`src/server/rtsp_server.cpp:1772-1779`
- 问题：
  - `current += 2` 非原子；并发 SETUP 可读到同一端口
  - 又因 `bindUdp` 设了 `SO_REUSEADDR`（`socket.cpp:238`），第二次 bind 成功 → 两会话共享同一 socket，RTP 互相污染
- 修复：
  1. `rtp_port_current` 改 `std::atomic<uint32_t>` + CAS，或用互斥锁保护
  2. 考虑移除 UDP 端口上的 `SO_REUSEADDR`（RTP 不需要复用）
- 验证：并发 SETUP 压测，检查两会话端口不重叠

### H2. `std::stoi/stoul/stod` 无 try/catch → 畸形输入致崩溃
- [ ] 位置（非穷举）：
  - `src/rtsp-common/rtsp_request.cpp:104`（CSeq）
  - `src/rtsp-common/rtsp_request.cpp:324`（响应状态码）
  - `src/rtsp-common/rtsp_request.cpp:150,160`（Transport 端口）
  - `src/rtsp-common/sdp.cpp:197,212,229,237,249`
  - `src/server/rtsp_server.cpp:1336`（interleaved 通道）
  - `src/client/rtsp_client.cpp:683,791,802`
  - `src/publisher/rtsp_publisher.cpp:56,102,103`
- 问题：任一异常穿透请求处理，终止连接线程或客户端进程，可一键 DoS
- 修复：
  - 在 `common.h` / `common.cpp` 新增 `bool parseIntSafe(const std::string&, int64_t&)`、`bool parseUintSafe(const std::string&, uint64_t&)`
  - 所有来自网络的数值字段统一走安全版本
- 验证：libFuzzer 对 `RtspRequest::parse` / `SdpParser::parse` 做输入 fuzz

### H3. 无请求大小上限 → 内存耗尽式 DoS
- [ ] 位置：`src/server/rtsp_server.cpp:1021-1046`
- 问题：
  - `content_length` 直接 `stoul` 无上限（攻击者填 4GB）
  - header 未收到 `\r\n\r\n` 时 `buffer` 无限累积（slowloris 类攻击）
- 修复：
  - 常量：`MAX_REQUEST_BODY = 64 * 1024`、`MAX_REQUEST_HEADER = 32 * 1024`、`MAX_BUFFER_TOTAL = 128 * 1024`
  - 超过上限直接 close socket、break 循环
- 验证：
  - 手工：`curl -H "Content-Length: 9999999999" ...`
  - slowloris 脚本：每秒发 1 字节不发 `\r\n\r\n`

---

## P1 — 强烈建议近期修

### H4. TCP interleaved 下 GET_PARAMETER 保活的"停-发-重启"模式有残留风险
- [ ] 位置：`src/client/rtsp_client.cpp:1441-1466`
- 问题：
  - 保活时暂停 TCP 接收线程 → `sendRequest → recvRtspResponse` 调用期间，内核 socket buffer 里可能混着 `$...` 二进制与 RTSP 响应文本
  - `recvRtspResponse` 按文本解析 `Content-Length`，遇到 `$` 二进制可能解析错
  - 怀疑这是部分"VLC/严格客户端"间歇性症状的来源
- 修复：
  - 把 TCP interleaved 模式改成**单接收线程多路分发**（服务端 `rtsp_server.cpp:997-1014` 已是此模式，客户端镜像）
  - 接收线程 peek 第一字节：
    - `$` → 读 channel + len + payload，按通道路由到 `rtp_receiver_` 或 RTCP 处理
    - 其他 → 按行累积 RTSP 文本响应，完成后投递到等待 `sendRequest` 的 `std::promise`
  - `sendRequest` 改为异步发 + 同步 future 等待
- 验证：保活期间持续推流，抓包确认无 RTP 丢失、无响应误判

### H5. Publisher 的 `sendRequest` 用单次 `recv` 读响应
- [ ] 位置：`src/publisher/rtsp_publisher.cpp:86-91`
- 问题：TCP 必然分片，单次 recv 不保证收完头部（更不保证带 body）
- 修复：
  - 把 `src/client/rtsp_client.cpp:90-140` 的 `recvRtspResponse` 抽到 `src/rtsp-common/` 作为公共函数
  - Publisher 复用
- 验证：对慢速/高延迟的 mediamtx 场景回归测试

### H6. `RtspPublisher::closeWithTimeout` 假超时
- [ ] 位置：`src/publisher/rtsp_publisher.cpp:244-252`
- 问题：`(void)timeout_ms;` 直接忽略；`teardown()` 内 `sendRequest` 有 5s recv，违反契约
- 修复：
  - `sendRequest` 增加 `recv_timeout_ms` 参数
  - `closeWithTimeout` 把 timeout 透传给 `sendRequest`
- 验证：故意连接一个不响应 TEARDOWN 的 server，`closeWithTimeout(500)` 必须 500ms 内返回

### M1. 全局无 `TCP_NODELAY`
- [ ] 位置：`src/rtsp-common/socket.cpp`
- 问题：
  - RTSP 控制通道：Nagle 引入 ~40ms 延迟
  - RTP-over-TCP：小 RTP 包被合并，端到端抖动上升，播放卡顿
- 修复：
  - `Socket::connect` 成功后、`Socket::accept` 返回前 `setsockopt(IPPROTO_TCP, TCP_NODELAY, 1)`
  - `bindUdp` 不受影响
- 验证：TCP 推流场景用 iperf/tcpdump 确认小包立即发送

### M6. DESCRIBE 响应未发 `Content-Base` 头 ⭐可能解决一类 VLC 问题
- [ ] 位置：`src/server/rtsp_server.cpp:1139-1190`
- 问题：
  - SDP 里 `a=control:stream` 是相对 URL
  - 不同客户端对相对解析 base 的默认行为不同（request URL / Content-Location / Content-Base）
  - 部分 VLC 版本、老 live555 会算出错误 SETUP URL → 404
- 修复：
  - 在 DESCRIBE 响应加 `Content-Base: <request_url>/`（结尾必须带 `/`）
  - 相应的 `RtspResponse::createDescribe` 加重载或 `setHeader`
- 验证：VLC / ffmpeg / GStreamer rtspsrc 都能正确 SETUP `/live/stream/stream`

---

## P2 — 下一个迭代

### M2. `RtpPacket` 裸 `uint8_t*` 泄漏易发
- [ ] 位置：
  - 声明：`include/rtsp-common/common.h:60-67`
  - new 处：`src/rtsp-common/rtp_packer.cpp:209, 272, 383, 448`
  - delete 处：`src/server/rtsp_server.cpp:876`、`src/publisher/rtsp_publisher.cpp:196`
- 问题：所有下游必须手动配对 `delete[]`，`packFrame` 里任一 `push_back` 抛 `bad_alloc` 就漏最近一块
- 修复：
  - `struct RtpPacket { std::vector<uint8_t> data; uint16_t seq; ... }`
  - 或 `std::unique_ptr<uint8_t[]>`
  - 所有 new/delete 对应处同步改完
- 验证：ASan + 压测

### M3. UDP 接收循环 1ms 忙等 → 嵌入式跑满一核
- [ ] 位置：
  - `src/server/rtsp_server.cpp:355-368`（Publisher 侧 receiver）
  - `src/client/rtsp_client.cpp:341-354`（Client 侧 receiver）
- 问题：非阻塞 `recvFrom` + `sleep_for(1ms)` → idle 时 ~1000 次/秒唤醒
- 修复：
  - 改阻塞 `recvFrom`（去掉 `setNonBlocking(true)` 的地方），用 `poll(fd, 200ms)` 等
  - 关闭时继续通过 `shutdownReadWrite` + 自发 UDP 包唤醒（现机制保留）
- 验证：`top` 观察空闲时 CPU；压测吞吐不下降

### M7. Publisher ANNOUNCE 的 SDP 使用 `0.0.0.0`
- [ ] 位置：`src/publisher/rtsp_publisher.cpp:131`
- 问题：某些严格服务器拒收 `c=IN IP4 0.0.0.0`
- 修复：改用 `control_socket_->getLocalIp()`，空则回退 `127.0.0.1`

### M8. 客户端 setup 不识别 `Content-Base` 响应头
- [ ] 位置：`src/client/rtsp_client.cpp:1124-1127`
- 问题：相对 control URL 应以 `Content-Base` 为 base，当前只用 `request_url_` 拼
- 修复：
  - `describe()` 里从响应 header 解 `Content-Base` / `Content-Location`，存到 Impl
  - `setup()` 拼 URL 时优先用这个 base
- 验证：对接带复杂控制 URL 的 IP 摄像头

### M9. `broadcastFrame` 在 `paths_mutex_` 内广播
- [ ] 位置：`src/server/rtsp_server.cpp:1941, 1970, 1999`
- 问题：大锁期间遍历 sessions + clone 帧，SETUP/DESCRIBE/addPath 被严重阻塞
- 修复：锁内仅 `shared_ptr<MediaPath> mp = it->second;`，释放后再 `mp->broadcastFrame(...)`
- 验证：高帧率 + 高并发连接场景吞吐测试

### M10. `SimpleRtspPlayer::open` 的 play 放异步线程
- [ ] 位置：`src/client/rtsp_client.cpp:1544-1556`
- 问题：`open()` 返回 true 但异步 `play()` 可能立即失败，调用者未设 `error_callback_` 毫不知情
- 修复：把 `play()` 放到 `open()` 同步路径里，或新增 `bool waitForPlaying(timeout)`

---

## P3 — 代码质量与边角

- [ ] **L1** `src/rtsp-common/rtp_packer.cpp:61` `findStartCode` 3 字节起始码 `i + 3 < size` 应为 `i + 2 < size`（末尾漏检，低影响）
- [ ] **L4** `src/server/rtsp_server.cpp:2004-2022` `getFrameInput` 返回对象持 `Impl*` 裸指针；Server 先析构则 UAF。改用 `std::weak_ptr<Impl>`/`weak_ptr<RtspServer>`
- [ ] **L5** `src/server/rtsp_server.cpp:856-877` interleaved 发送 hot path 每包新建 `vector` + `memcpy`；改 `thread_local` 预分配 buffer
- [ ] **L6** `src/client/rtsp_client.cpp:1027-1044` `onFrame` 对同一帧 clone 两次（入队 + 回调）；合并为一次
- [ ] **M4** Digest nonce 用非 CSPRNG（`src/server/rtsp_server.cpp:695-708`），可预测。改读 `/dev/urandom`（Linux）/`BCryptGenRandom`（Windows）
- [ ] **M5** 允许 `setAuth(..., auth_nonce)` 固定 nonce（`src/server/rtsp_server.cpp:979`）；删除该字段或加强警告
- [ ] **L10** `src/server/rtsp_server.cpp:1330` 残留"变量作用域问题"注释与冗余 `is_tcp = use_tcp` 代码，清理掉
- [ ] **L2** `handlePlay` 幂等判定只看 `playing`（`rtsp_server.cpp:1466-1478`），不校验 CSeq/Range；重复 PLAY 可能导致 RTP 时间戳重排
- [ ] **L3** `RtspServer::start` 新连接 lambda 捕获 `this`；Server 先释放可能悬空。改 `weak_ptr` 或 `shared_from_this`
- [ ] **L7** `generateSessionId` 可预测（同 M4），改 CSPRNG
- [ ] **L8** `RtspPublisher::setup` 未设 SSRC，多 Publisher 并发易撞 SSRC。生成随机 SSRC 再 `setSsrc`

---

## 推进顺序建议

| 批次 | 任务 | 改动规模 | 风险 |
|---|---|---|---|
| 1 | C1, C3, H1 | 小（纯临界区修正） | 低 |
| 2 | C4 | 小（SSRC 统一） | 低 |
| 3 | C5 | 中（新锁 + 调用点改） | 中 |
| 4 | C2 | 中（cleanup 解耦 + send 写超时） | 中 |
| 5 | H2, H3 | 中（安全解析工具 + 上限常量） | 低 |
| 6 | M6 | 小（加 header） | 低，兼容性收益大 |
| 7 | H4 | 大（client interleaved 重构） | 中 |
| 8 | H5, H6 | 小（公共 recvRtspResponse + 真 timeout） | 低 |
| 9 | M1 | 小（TCP_NODELAY） | 低 |
| 10 | P2 / P3 | 按需 | 低 |

---

## 配套质量基础设施

- [ ] CI 加一个 **TSan 构建矩阵**，跑 `test_soak.sh --duration 120 --transport tcp` 和 `--transport udp`
- [ ] CI 加一个 **ASan 构建矩阵**，跑所有 gtest
- [ ] 新增 `tests/test_fuzz_parse.cpp`：libFuzzer 喂 `RtspRequest::parse`、`SdpParser::parse`（H2 回归用）
- [ ] 新增 `tests/test_hostile_client.cpp`：连接后不读、慢速发字节、发超大 Content-Length，验证 server 不挂不 OOM（C2/H3 回归用）
- [ ] 新增 `tests/test_compat_matrix.sh`：ffmpeg TCP/UDP + VLC + GStreamer rtspsrc 连通回归（C4/M6 回归用）
