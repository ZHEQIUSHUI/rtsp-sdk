# RTSP SDK

A lightweight C++ RTSP server and client SDK for H.264/H.265 video streaming.

## Features

- **RTSP Server**: Full RTSP 1.0 server implementation
  - Supports OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN methods
  - H.264 (RFC 6184) and H.265/HEVC (RFC 7798) RTP payload formats
  - UDP (RTP/AVP) + TCP interleaved transport
  - Basic/Digest authentication
  - Auto-extract H.264 SPS/PPS and H.265 VPS/SPS/PPS from keyframes (no mandatory manual fill)
  - Automatic SDP generation with sprop-parameter-sets

- **RTSP Client**: Both low-level and high-level APIs
  - `RtspClient`: Low-level client for custom control flow
  - `SimpleRtspPlayer`: High-level player with callback-based frame receiving
  - `VideoFrame` payload uses smart-managed memory (no manual `delete[]`)
  - Automatic RTP packet reception, reordering and decoding
  - URL auth parsing (`rtsp://user:pass@host/path`)
  - UDP/TCP setup fallback

- **RTP Robustness**:
  - H.264 STAP-A/STAP-B aggregation support
  - H.265 AP + FU loss resync handling
- **Observability**:
  - Structured logging (`plain` / `json`)
  - Server/client runtime stats API
- **Cross-Platform**: Linux / Windows

## Requirements

- C++14 compatible compiler
- CMake 3.10 or higher
- Linux or Windows
- pthread library (Linux)
- WinSock2 (Windows, system default)

## Build

Linux:

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

Windows (PowerShell):

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Run Tests

Linux:

```bash
ctest --output-on-failure
```

Windows (PowerShell):

```powershell
ctest --test-dir build -C Release --output-on-failure
```

## Soak Test

```bash
./test_soak.sh --duration 120 --transport tcp
```

Optional auth soak:

```bash
./test_soak.sh --duration 120 --auth admin:123456 --digest --transport tcp
```

The script writes `server.log`, `client.log`, and markdown report under `soak_reports/<timestamp>/`.

## Usage

### Server Example

```cpp
#include <rtsp-server/rtsp-server.h>

using namespace rtsp;

int main() {
    RtspServer server;
    server.init("0.0.0.0", 8554);

    // Add media path
    PathConfig config;
    config.path = "/live/stream";
    config.codec = CodecType::H264;
    config.width = 1920;
    config.height = 1080;
    config.fps = 30;
    // Optional: manually set SPS/PPS.
    // If omitted, server auto-extracts from pushed keyframes.
    // config.sps = {...};
    // config.pps = {...};
    server.addPath(config);

    server.start();

    // Push Annex-B H264 frame data (with start codes)
    while (running) {
        const uint8_t* h264_data = ...;
        size_t data_size = ...;     // includes SPS/PPS/IDR for keyframe
        uint64_t pts_ms = ...;
        bool is_key = ...;
        server.pushH264Data("/live/stream", h264_data, data_size, pts_ms, is_key);
    }

    server.stop();
    return 0;
}
```

### Client Example

```cpp
#include <rtsp-client/rtsp-client.h>

using namespace rtsp;

int main() {
    // Simple callback-based player
    SimpleRtspPlayer player;
    
    // Set frame callback
    player.setFrameCallback([](const VideoFrame& frame) {
        std::cout << "Received frame: " << frame.width << "x" << frame.height
                  << " pts=" << frame.pts << std::endl;
        
        // Decode and display frame...
        decode_and_display(frame.data, frame.size);
    });
    
    // Connect and play
    if (player.open("rtsp://192.168.1.100:8554/live/stream")) {
        player.play();
        
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        player.stop();
    }
    
    return 0;
}
```

## API Reference

### Server API

- `bool init(const std::string& host, uint16_t port)` - Initialize server
- `bool addPath(const PathConfig& config)` - Add media path
- `bool start()` / `void stop()` - Control server
- `bool pushH264Data(path, data, size, pts, is_key)` - Push H.264 Annex-B frame
- `bool pushH265Data(path, data, size, pts, is_key)` - Push H.265 Annex-B frame
- `void setAuth(user, pass)` / `setAuthDigest(user, pass)` - Enable auth
- `RtspServerStats getStats()` - Runtime metrics

### Client API

- `bool open(url)` / `void close()` - Connect/disconnect
- `bool describe()` / `bool setup()` / `bool play()` - RTSP control flow
- `bool receiveFrame(frame, timeout_ms)` - Blocking receive
- `RtspClientStats getStats()` - Runtime metrics

## Project Structure

```
include/
  rtsp-server/        # Server public headers
    rtsp-server.h
    rtsp_server.h
  rtsp-client/        # Client public headers
    rtsp-client.h
    rtsp_client.h
  rtsp-common/        # Common public headers
    common.h

src/
  rtsp-common/        # Internal implementations (private)
    sdp.cpp/h
    rtp_packer.cpp/h
    rtsp_request.cpp/h
    socket.cpp/h
  server/             # Server implementation
  client/             # Client implementation

examples/             # Example applications
tests/                # Unit tests
```

## License

MIT License
