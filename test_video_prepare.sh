#!/bin/bash

# 准备不同编码格式的测试视频

set -e

INPUT_VIDEO="person_test_ov2_1920x1080_30fps_gop60_4Mbps.mp4"
OUTPUT_DIR="test_videos"

echo "=== Preparing Test Videos ==="
echo "Input: $INPUT_VIDEO"
echo ""

mkdir -p "$OUTPUT_DIR"

# 1. H.264 (Baseline Profile)
echo "[1/5] Converting to H.264 Baseline..."
ffmpeg -i "$INPUT_VIDEO" -c:v libx264 -profile:v baseline -level 3.0 \
    -preset fast -crf 23 -t 10 \
    "$OUTPUT_DIR/test_h264_baseline.mp4" -y -loglevel error

# 2. H.264 (Main Profile)
echo "[2/5] Converting to H.264 Main..."
ffmpeg -i "$INPUT_VIDEO" -c:v libx264 -profile:v main -level 4.0 \
    -preset fast -crf 23 -t 10 \
    "$OUTPUT_DIR/test_h264_main.mp4" -y -loglevel error

# 3. H.264 (High Profile)
echo "[3/5] Converting to H.264 High..."
ffmpeg -i "$INPUT_VIDEO" -c:v libx264 -profile:v high -level 4.0 \
    -preset fast -crf 23 -t 10 \
    "$OUTPUT_DIR/test_h264_high.mp4" -y -loglevel error

# 4. H.265 / HEVC
echo "[4/5] Converting to H.265/HEVC..."
ffmpeg -i "$INPUT_VIDEO" -c:v libx265 -preset fast -crf 28 -t 10 \
    -tag:v hvc1 "$OUTPUT_DIR/test_h265.mp4" -y -loglevel error

# 5. MPEG-4 Part 2 (用于测试容灾 - 不支持的格式)
echo "[5/5] Converting to MPEG-4 Part 2 (unsupported format for disaster test)..."
ffmpeg -i "$INPUT_VIDEO" -c:v mpeg4 -q:v 5 -t 10 \
    "$OUTPUT_DIR/test_mpeg4.mp4" -y -loglevel error 2>/dev/null || echo "      (mpeg4 encoder not available, skipping)"

echo ""
echo "=== Test Videos Created ==="
ls -lh "$OUTPUT_DIR/"
echo ""
echo "Video Info:"
for f in "$OUTPUT_DIR"/*.mp4; do
    echo "  $(basename $f):"
    ffprobe -v error -select_streams v:0 \
        -show_entries stream=codec_name,profile,width,height,r_frame_rate \
        -of default=noprint_wrappers=1 "$f" 2>/dev/null | sed 's/^/    /'
done
