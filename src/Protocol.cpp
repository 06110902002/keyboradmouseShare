// =============================================================================
// Protocol.cpp —— 消息构造器实现
//
// 每个构造器都用 PacketBuilder 先拼负载(类型码 + 字段),再由 frame() 加 4 字节
// 大端长度前缀产出整帧。字段顺序/宽度严格对应客户端读取器。
// =============================================================================
#include "Protocol.h"

namespace kms {
namespace proto {

std::vector<std::uint8_t> buildHello(std::uint16_t major, std::uint16_t minor) {
    // 注意:Hello 的"类型"就是 7 字节 "Synergy" 定长标识,计入长度;
    // 负载 = "Synergy"(7) + major(2) + minor(2) = 11 → 客户端强校验 len==11。
    PacketBuilder b;
    b.putFixed(kHello, 7);
    b.put16(major);
    b.put16(minor);
    return b.frame();
}

std::vector<std::uint8_t> buildQueryInfo() {
    PacketBuilder b;
    b.putType(kQueryInfo);
    return b.frame();
}

std::vector<std::uint8_t> buildSetOptions(const std::vector<std::uint32_t> &options) {
    PacketBuilder b;
    b.putType(kSetOptions);
    b.putIntVector(options); // %4I:count + 各元素
    return b.frame();
}

std::vector<std::uint8_t> buildInfoAck() {
    PacketBuilder b;
    b.putType(kInfoAck);
    return b.frame();
}

std::vector<std::uint8_t> buildKeepAlive() {
    PacketBuilder b;
    b.putType(kKeepAlive);
    return b.frame();
}

std::vector<std::uint8_t> buildEnter(std::int16_t x, std::int16_t y,
                                     std::uint32_t seq, std::uint16_t mask) {
    PacketBuilder b;
    b.putType(kEnter);
    b.put16(static_cast<std::uint16_t>(x));
    b.put16(static_cast<std::uint16_t>(y));
    b.put32(seq);
    b.put16(mask);
    return b.frame();
}

std::vector<std::uint8_t> buildLeave() {
    PacketBuilder b;
    b.putType(kLeave);
    return b.frame();
}

std::vector<std::uint8_t> buildMouseMove(std::int16_t x, std::int16_t y) {
    PacketBuilder b;
    b.putType(kMouseMove);
    b.put16(static_cast<std::uint16_t>(x));
    b.put16(static_cast<std::uint16_t>(y));
    return b.frame();
}

std::vector<std::uint8_t> buildMouseRelMove(std::int16_t dx, std::int16_t dy) {
    PacketBuilder b;
    b.putType(kMouseRelMove);
    b.put16(static_cast<std::uint16_t>(dx));
    b.put16(static_cast<std::uint16_t>(dy));
    return b.frame();
}

std::vector<std::uint8_t> buildMouseDown(std::uint8_t button) {
    PacketBuilder b;
    b.putType(kMouseDown);
    b.put8(button);
    return b.frame();
}

std::vector<std::uint8_t> buildMouseUp(std::uint8_t button) {
    PacketBuilder b;
    b.putType(kMouseUp);
    b.put8(button);
    return b.frame();
}

std::vector<std::uint8_t> buildMouseWheel(std::int16_t xDelta, std::int16_t yDelta) {
    PacketBuilder b;
    b.putType(kMouseWheel);
    b.put16(static_cast<std::uint16_t>(xDelta));
    b.put16(static_cast<std::uint16_t>(yDelta));
    return b.frame();
}

std::vector<std::uint8_t> buildKeyDown(std::uint16_t id, std::uint16_t mask, std::uint16_t button) {
    PacketBuilder b;
    b.putType(kKeyDown);
    b.put16(id);
    b.put16(mask);
    b.put16(button);
    return b.frame();
}

std::vector<std::uint8_t> buildKeyUp(std::uint16_t id, std::uint16_t mask, std::uint16_t button) {
    PacketBuilder b;
    b.putType(kKeyUp);
    b.put16(id);
    b.put16(mask);
    b.put16(button);
    return b.frame();
}

std::vector<std::uint8_t> buildKeyRepeat(std::uint16_t id, std::uint16_t mask,
                                         std::uint16_t count, std::uint16_t button) {
    PacketBuilder b;
    b.putType(kKeyRepeat);
    b.put16(id);
    b.put16(mask);
    b.put16(count);
    b.put16(button);
    // 不追加 lang 串:客户端 KeyRepeat 读取器只读上面 4 个 short。
    return b.frame();
}

std::vector<std::uint8_t> buildClose() {
    PacketBuilder b;
    b.putType(kClose);
    return b.frame();
}

} // namespace proto
} // namespace kms
