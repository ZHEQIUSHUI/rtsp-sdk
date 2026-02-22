# RTSP SDK

A lightweight C++ RTSP server and client SDK for H.264/H.265 video streaming.

## Features

- **RTSP Server**: Full RTSP 1.0 server implementation
  - Supports OPTIONS, DESCRIBE, SETUP, PLAY, PAUSE, TEARDOWN methods
  - H.264 (RFC 6184) and H.265/HEVC (RFC 7798) RTP payload formats
  - UDP (RTP/AVP) + TCP interleaved transport
  - Basic/Digest authentication
  - Automatic SDP generation with sprop-parameter-sets

- **RTSP Client**: Both low-level and high-level APIs
  - `RtspClient`: Low-level client for custom control flow
  - `SimpleRtspPlayer`: High-level player with callback-based frame receiving
  - Automatic RTP packet reception, reordering and decoding
  - URL auth parsing (`rtsp://user:pass@host/path`)
  - UDP/TCP setup fallback

- **RTP Robustness**:
  - H.264 STAP-A/STAP-B aggregation support
  - H.265 AP + FU loss resync handling
- **Observability**:
  - Structured logging (`plain` / `json`)
  - Server/client runtime stats API
- **Cross-Platform**: Linux with standard socket APIs

## Requirements

- C++14 compatible compiler
- CMake 3.10 or higher
- Linux operating system
- pthread library

## Build

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

## Run Tests

```bash
ctest --output-on-failure
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
    // Create server
    RtspServer server("0.0.0.0", 8554);
    
    // Create a media path
    MediaConfig config;
    config.codec = CodecType::H264;
    config.width = 1920;
    config.height = 1080;
    config.fps = 30;
    config.sps = {0x67, 0x42, 0x00, 0x28, ...};  // SPS NALU
    config.pps = {0x68, 0xCE, 0x3C, 0x80, ...};  // PPS NALU
    
    server.createPath("/live/stream", config);
    
    // Start server
    server.start();
    
    // Push video frames (from encoder thread)
    while (running) {
        uint8_t* h264_data = ...;  // From video encoder
        size_t data_size = ...;
        int64_t pts = ...;         // Presentation timestamp in ms
        
        server.pushH264Data("/live/stream", h264_data, data_size, pts);
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

- `RtspServer(const std::string& ip, int port)` - Create server
- `bool createPath(const std::string& path, const MediaConfig& config)` - Create media endpoint
- `bool start()` / `void stop()` - Control server
- `void pushH264Data(path, data, size, pts)` - Push H.264 frame
- `void pushH265Data(path, data, size, pts)` - Push H.265 frame

### Client API

- `SimpleRtspPlayer::open(url)` - Connect to stream
- `SimpleRtspPlayer::play()` / `stop()` - Control playback
- `SimpleRtspPlayer::setFrameCallback(cb)` - Set frame receiver

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
