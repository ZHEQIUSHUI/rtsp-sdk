#pragma once

// AMF0 编解码（足够 RTMP 控制消息 connect/createStream/publish 往返使用）。
// AMF0 规范参考 Adobe Action Message Format -- AMF0 (December 2007)。
//
// 支持的数据类型：
//   - Number       (marker 0x00) : 8-byte big-endian IEEE 754 double
//   - Boolean      (marker 0x01) : 1 byte (0/1)
//   - String       (marker 0x02) : 2-byte BE length + UTF-8 bytes
//   - Object       (marker 0x03) : (2-byte key + typed value)* + object-end marker (0x00 0x00 0x09)
//   - Null         (marker 0x05)
//   - Undefined    (marker 0x06)
//   - ECMA Array   (marker 0x08) : 4-byte count + object-style properties + end marker
//   - Strict Array (marker 0x0a) : 4-byte count + typed values
//
// 编码：流式追加到 std::vector<uint8_t>。
// 解码：值树 AmfValue（递归结构）+ parse() 返回消费的字节数。

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rtsp {
namespace amf0 {

enum class Type {
    Number,
    Boolean,
    String,
    Object,
    Null,
    Undefined,
    EcmaArray,
    StrictArray,
};

struct Value;
using ValuePtr = std::shared_ptr<Value>;

struct Value {
    Type type = Type::Null;
    double num = 0.0;
    bool boolean = false;
    std::string str;
    // Object / EcmaArray: 按插入顺序保存 key-value 对
    std::vector<std::pair<std::string, ValuePtr>> obj_props;
    // StrictArray
    std::vector<ValuePtr> array;

    static ValuePtr makeNumber(double v);
    static ValuePtr makeBool(bool v);
    static ValuePtr makeString(std::string v);
    static ValuePtr makeNull();
    static ValuePtr makeUndefined();
    static ValuePtr makeObject();  // 返回可用 addProp 填充的空对象

    // Object / EcmaArray 辅助填充。按顺序插入。
    Value& addProp(const std::string& key, ValuePtr v);
    Value& addNumber(const std::string& key, double v);
    Value& addBool(const std::string& key, bool v);
    Value& addString(const std::string& key, std::string v);

    // 按 key 查找（Object / EcmaArray）
    const Value* getProp(const std::string& key) const;
    // 取字符串（若类型不符返回空字符串）
    std::string asString() const;
    double asNumber() const;
};

// --------- 编码 ----------
void encodeNumber(std::vector<uint8_t>& out, double v);
void encodeBool(std::vector<uint8_t>& out, bool v);
void encodeString(std::vector<uint8_t>& out, const std::string& v);
void encodeNull(std::vector<uint8_t>& out);
void encodeUndefined(std::vector<uint8_t>& out);
void encodeObject(std::vector<uint8_t>& out, const Value& obj);
void encodeEcmaArray(std::vector<uint8_t>& out, const Value& arr);
void encode(std::vector<uint8_t>& out, const Value& v);

// --------- 解码 ----------
// 从 data[0..len) 解析一个 AMF0 值，写入 *out。返回消费的字节数；失败返回 0。
size_t parseValue(const uint8_t* data, size_t len, Value* out);

// 一次解析多个值（RTMP command message 就是多个 AMF0 值的连续序列）。
// 返回消费字节数，解析的值追加到 out_values。失败返回 0。
size_t parseValues(const uint8_t* data, size_t len, std::vector<Value>* out_values);

}  // namespace amf0
}  // namespace rtsp
