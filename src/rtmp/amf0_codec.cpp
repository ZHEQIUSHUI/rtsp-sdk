#include "amf0_codec.h"

#include <cstring>

namespace rtsp {
namespace amf0 {

namespace {

constexpr uint8_t kMarkerNumber     = 0x00;
constexpr uint8_t kMarkerBoolean    = 0x01;
constexpr uint8_t kMarkerString     = 0x02;
constexpr uint8_t kMarkerObject     = 0x03;
constexpr uint8_t kMarkerNull       = 0x05;
constexpr uint8_t kMarkerUndefined  = 0x06;
constexpr uint8_t kMarkerEcmaArray  = 0x08;
constexpr uint8_t kMarkerObjectEnd  = 0x09;
constexpr uint8_t kMarkerStrictArr  = 0x0a;

void writeBE16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}
void writeBE32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8)  & 0xFF));
    out.push_back(static_cast<uint8_t>(v & 0xFF));
}

uint16_t readBE16(const uint8_t* p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}
uint32_t readBE32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
           (uint32_t(p[2]) << 8)  |  uint32_t(p[3]);
}

}  // namespace

// ---------- Value factory / helpers ----------

ValuePtr Value::makeNumber(double v) {
    auto p = std::make_shared<Value>();
    p->type = Type::Number; p->num = v;
    return p;
}
ValuePtr Value::makeBool(bool v) {
    auto p = std::make_shared<Value>();
    p->type = Type::Boolean; p->boolean = v;
    return p;
}
ValuePtr Value::makeString(std::string v) {
    auto p = std::make_shared<Value>();
    p->type = Type::String; p->str = std::move(v);
    return p;
}
ValuePtr Value::makeNull() {
    auto p = std::make_shared<Value>();
    p->type = Type::Null;
    return p;
}
ValuePtr Value::makeUndefined() {
    auto p = std::make_shared<Value>();
    p->type = Type::Undefined;
    return p;
}
ValuePtr Value::makeObject() {
    auto p = std::make_shared<Value>();
    p->type = Type::Object;
    return p;
}

Value& Value::addProp(const std::string& key, ValuePtr v) {
    obj_props.emplace_back(key, std::move(v));
    return *this;
}
Value& Value::addNumber(const std::string& key, double v) {
    return addProp(key, makeNumber(v));
}
Value& Value::addBool(const std::string& key, bool v) {
    return addProp(key, makeBool(v));
}
Value& Value::addString(const std::string& key, std::string v) {
    return addProp(key, makeString(std::move(v)));
}

const Value* Value::getProp(const std::string& key) const {
    for (const auto& kv : obj_props) {
        if (kv.first == key) return kv.second.get();
    }
    return nullptr;
}

std::string Value::asString() const {
    return type == Type::String ? str : std::string{};
}
double Value::asNumber() const {
    return type == Type::Number ? num : 0.0;
}

// ---------- Encode ----------

void encodeNumber(std::vector<uint8_t>& out, double v) {
    out.push_back(kMarkerNumber);
    uint8_t tmp[8];
    std::memcpy(tmp, &v, 8);
    // IEEE 754 double, big-endian
    for (int i = 7; i >= 0; --i) out.push_back(tmp[i]);
}

void encodeBool(std::vector<uint8_t>& out, bool v) {
    out.push_back(kMarkerBoolean);
    out.push_back(v ? 1 : 0);
}

void encodeString(std::vector<uint8_t>& out, const std::string& v) {
    out.push_back(kMarkerString);
    const uint16_t len = static_cast<uint16_t>(v.size());
    writeBE16(out, len);
    out.insert(out.end(), v.begin(), v.end());
}

void encodeNull(std::vector<uint8_t>& out) { out.push_back(kMarkerNull); }
void encodeUndefined(std::vector<uint8_t>& out) { out.push_back(kMarkerUndefined); }

static void encodePropsThenEnd(std::vector<uint8_t>& out,
                               const std::vector<std::pair<std::string, ValuePtr>>& props) {
    for (const auto& kv : props) {
        const uint16_t len = static_cast<uint16_t>(kv.first.size());
        writeBE16(out, len);
        out.insert(out.end(), kv.first.begin(), kv.first.end());
        encode(out, *kv.second);
    }
    // object-end marker: empty name (2 bytes 0x00 0x00) + marker 0x09
    writeBE16(out, 0);
    out.push_back(kMarkerObjectEnd);
}

void encodeObject(std::vector<uint8_t>& out, const Value& obj) {
    out.push_back(kMarkerObject);
    encodePropsThenEnd(out, obj.obj_props);
}

void encodeEcmaArray(std::vector<uint8_t>& out, const Value& arr) {
    out.push_back(kMarkerEcmaArray);
    // associative-count: 信息性字段，可为 0（spec 允许）
    writeBE32(out, static_cast<uint32_t>(arr.obj_props.size()));
    encodePropsThenEnd(out, arr.obj_props);
}

void encode(std::vector<uint8_t>& out, const Value& v) {
    switch (v.type) {
        case Type::Number:      encodeNumber(out, v.num);  break;
        case Type::Boolean:     encodeBool(out, v.boolean); break;
        case Type::String:      encodeString(out, v.str);   break;
        case Type::Object:      encodeObject(out, v);       break;
        case Type::Null:        encodeNull(out);            break;
        case Type::Undefined:   encodeUndefined(out);       break;
        case Type::EcmaArray:   encodeEcmaArray(out, v);    break;
        case Type::StrictArray: {
            out.push_back(kMarkerStrictArr);
            writeBE32(out, static_cast<uint32_t>(v.array.size()));
            for (const auto& item : v.array) encode(out, *item);
            break;
        }
    }
}

// ---------- Decode ----------

namespace {

// 读 2 字节长度 UTF-8 字符串；成功返回消费字节数；否则 0
size_t readShortString(const uint8_t* data, size_t len, std::string* out) {
    if (len < 2) return 0;
    const uint16_t s = readBE16(data);
    if (len < size_t{2} + s) return 0;
    out->assign(reinterpret_cast<const char*>(data + 2), s);
    return 2 + size_t{s};
}

}  // namespace

size_t parseValue(const uint8_t* data, size_t len, Value* out) {
    if (len < 1 || !out) return 0;
    const uint8_t marker = data[0];
    size_t off = 1;
    switch (marker) {
        case kMarkerNumber: {
            if (len < off + 8) return 0;
            uint8_t rev[8];
            for (int i = 0; i < 8; ++i) rev[i] = data[off + 7 - i];
            double v;
            std::memcpy(&v, rev, 8);
            out->type = Type::Number;
            out->num = v;
            return off + 8;
        }
        case kMarkerBoolean: {
            if (len < off + 1) return 0;
            out->type = Type::Boolean;
            out->boolean = data[off] != 0;
            return off + 1;
        }
        case kMarkerString: {
            std::string s;
            const size_t n = readShortString(data + off, len - off, &s);
            if (n == 0) return 0;
            out->type = Type::String;
            out->str = std::move(s);
            return off + n;
        }
        case kMarkerNull:
            out->type = Type::Null;
            return off;
        case kMarkerUndefined:
            out->type = Type::Undefined;
            return off;
        case kMarkerObject:
        case kMarkerEcmaArray: {
            out->type = (marker == kMarkerObject) ? Type::Object : Type::EcmaArray;
            if (marker == kMarkerEcmaArray) {
                if (len < off + 4) return 0;
                // associative-count 信息性，忽略
                off += 4;
            }
            while (true) {
                if (len < off + 2) return 0;
                const uint16_t key_len = readBE16(data + off);
                // object-end sentinel：key_len=0 紧跟 0x09
                if (key_len == 0) {
                    if (len < off + 3) return 0;
                    if (data[off + 2] != kMarkerObjectEnd) return 0;
                    return off + 3;
                }
                std::string key;
                const size_t sn = readShortString(data + off, len - off, &key);
                if (sn == 0) return 0;
                off += sn;
                auto v = std::make_shared<Value>();
                const size_t vn = parseValue(data + off, len - off, v.get());
                if (vn == 0) return 0;
                off += vn;
                out->obj_props.emplace_back(std::move(key), std::move(v));
            }
        }
        case kMarkerStrictArr: {
            if (len < off + 4) return 0;
            const uint32_t count = readBE32(data + off);
            off += 4;
            out->type = Type::StrictArray;
            for (uint32_t i = 0; i < count; ++i) {
                auto v = std::make_shared<Value>();
                const size_t vn = parseValue(data + off, len - off, v.get());
                if (vn == 0) return 0;
                off += vn;
                out->array.push_back(std::move(v));
            }
            return off;
        }
        default:
            return 0;  // unsupported marker
    }
}

size_t parseValues(const uint8_t* data, size_t len, std::vector<Value>* out_values) {
    if (!out_values) return 0;
    size_t off = 0;
    while (off < len) {
        Value v;
        const size_t n = parseValue(data + off, len - off, &v);
        if (n == 0) break;
        out_values->push_back(std::move(v));
        off += n;
    }
    return off;
}

}  // namespace amf0
}  // namespace rtsp
