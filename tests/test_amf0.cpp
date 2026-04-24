// AMF0 encode/decode round-trip 测试
#include "amf0_codec.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>

using namespace rtsp;

static void test_number() {
    std::vector<uint8_t> buf;
    amf0::encodeNumber(buf, 3.1415926);
    assert(buf.size() == 9);
    assert(buf[0] == 0x00);

    amf0::Value v;
    size_t n = amf0::parseValue(buf.data(), buf.size(), &v);
    assert(n == buf.size());
    assert(v.type == amf0::Type::Number);
    assert(std::fabs(v.num - 3.1415926) < 1e-9);
}

static void test_bool_string_null() {
    std::vector<uint8_t> buf;
    amf0::encodeBool(buf, true);
    amf0::encodeString(buf, "hello");
    amf0::encodeNull(buf);

    std::vector<amf0::Value> vs;
    size_t n = amf0::parseValues(buf.data(), buf.size(), &vs);
    assert(n == buf.size());
    assert(vs.size() == 3);
    assert(vs[0].type == amf0::Type::Boolean && vs[0].boolean);
    assert(vs[1].type == amf0::Type::String  && vs[1].str == "hello");
    assert(vs[2].type == amf0::Type::Null);
}

static void test_object() {
    auto obj = amf0::Value::makeObject();
    obj->addString("app", "live");
    obj->addString("tcUrl", "rtmp://h/app");
    obj->addNumber("capabilities", 239);
    obj->addBool("fpad", false);

    std::vector<uint8_t> buf;
    amf0::encode(buf, *obj);

    amf0::Value out;
    size_t n = amf0::parseValue(buf.data(), buf.size(), &out);
    assert(n == buf.size());
    assert(out.type == amf0::Type::Object);
    assert(out.obj_props.size() == 4);

    auto* app = out.getProp("app");
    assert(app && app->asString() == "live");
    auto* cap = out.getProp("capabilities");
    assert(cap && cap->asNumber() == 239);
    auto* fpad = out.getProp("fpad");
    assert(fpad && fpad->type == amf0::Type::Boolean && !fpad->boolean);
}

static void test_connect_command() {
    // 模拟一条 RTMP connect command 的完整 payload：字符串 "connect" + tx id 1 +
    // object（app + flashVer + tcUrl）
    std::vector<uint8_t> buf;
    amf0::encode(buf, *amf0::Value::makeString("connect"));
    amf0::encode(buf, *amf0::Value::makeNumber(1));
    auto obj = amf0::Value::makeObject();
    obj->addString("app", "live");
    obj->addString("flashVer", "FMLE/3.0");
    obj->addString("tcUrl", "rtmp://127.0.0.1/live");
    amf0::encode(buf, *obj);

    std::vector<amf0::Value> vs;
    amf0::parseValues(buf.data(), buf.size(), &vs);
    assert(vs.size() == 3);
    assert(vs[0].asString() == "connect");
    assert(vs[1].asNumber() == 1);
    assert(vs[2].type == amf0::Type::Object);
    assert(vs[2].getProp("app")->asString() == "live");
}

static void test_ecma_array() {
    amf0::Value arr;
    arr.type = amf0::Type::EcmaArray;
    arr.addNumber("width", 1920);
    arr.addNumber("height", 1080);
    arr.addString("videocodecid", "hvc1");

    std::vector<uint8_t> buf;
    amf0::encode(buf, arr);

    amf0::Value out;
    size_t n = amf0::parseValue(buf.data(), buf.size(), &out);
    assert(n == buf.size());
    assert(out.type == amf0::Type::EcmaArray);
    assert(out.getProp("width")->asNumber() == 1920);
    assert(out.getProp("videocodecid")->asString() == "hvc1");
}

int main() {
    test_number();
    test_bool_string_null();
    test_object();
    test_connect_command();
    test_ecma_array();
    std::cout << "amf0 tests passed\n";
    return 0;
}
