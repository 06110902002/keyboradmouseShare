// =============================================================================
// Protocol.h —— deskflow / Synergy 1.x 线上协议编解码(与客户端逐字节一致)
//
// 传输格式(与 deskflow 完全相同):
//   每条消息 = [4 字节大端长度 length][length 字节负载]
//   length = 负载字节数 = len(类型码) + len(字段数据)
//   普通消息类型码为 4 个 ASCII 字节;唯独 Hello/HelloBack 的标识 "Synergy"
//   为 7 字节(定长、无长度前缀),这 7 字节本身计入 length。
//
// 关键约束(来自对不可改动客户端 InputShare_101227 的逐字节核对):
//   * 客户端直接从流里按字段读取,不做整帧缓冲。因此服务端发出的字节数必须与
//     客户端读取器"分毫不差"——多一个字节都会错位、污染下一帧。
//   * deskflow 的 DKRP 带一个尾随 lang 字符串(%s),但客户端的 KeyRepeat 读取器
//     只读 4 个 short、不读该串。所以本实现的 buildKeyRepeat 只发 4 个 short。
//   * 整型均为网络字节序(大端)。坐标/增量按有符号 16 位;键 id/mask/button 按
//     无符号 16 位(位型一致,写低 16 位即可)。
// =============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace kms {
namespace proto {

// ---- 4 字节消息类型码(与客户端 MessageType 枚举一致)----
constexpr char kHello[]       = "Synergy"; // 握手标识(7 字节)
constexpr char kNoop[]        = "CNOP";
constexpr char kClose[]       = "CBYE";
constexpr char kEnter[]       = "CINN";
constexpr char kLeave[]       = "COUT";
constexpr char kResetOptions[]= "CROP";
constexpr char kInfoAck[]     = "CIAK";
constexpr char kKeepAlive[]   = "CALV";
constexpr char kKeyDown[]     = "DKDN";
constexpr char kKeyRepeat[]   = "DKRP";
constexpr char kKeyUp[]       = "DKUP";
constexpr char kMouseDown[]   = "DMDN";
constexpr char kMouseUp[]     = "DMUP";
constexpr char kMouseMove[]   = "DMMV";
constexpr char kMouseRelMove[]= "DMRM";
constexpr char kMouseWheel[]  = "DMWM";
constexpr char kInfo[]        = "DINF";
constexpr char kSetOptions[]  = "DSOP";
constexpr char kQueryInfo[]   = "QINF";

// deskflow 选项码:kOptionRelativeMouseMoves = OPTION_CODE("MDLT")。
// OPTION_CODE 把 4 个字符拼成一个大端 32 位整数。这里预先算好常量并给出注释。
// 'M'=0x4D 'D'=0x44 'L'=0x4C 'T'=0x54 → 0x4D444C54 = 1296515924。
constexpr std::uint32_t kOptionRelativeMouseMoves = 0x4D444C54u;

// =============================================================================
// PacketBuilder —— 累积负载,最后加 4 字节大端长度前缀,产出可直接发送的整帧字节。
// =============================================================================
class PacketBuilder {
public:
    // 写 4 字节类型码(不含结尾 '\0')。用于普通消息起始。
    void putType(const char *code4) { raw(reinterpret_cast<const std::uint8_t *>(code4), 4); }

    // 写定长原始字符串(不带长度前缀),用于 "Synergy" 这类 7 字节标识。
    void putFixed(const char *s, std::size_t n) { raw(reinterpret_cast<const std::uint8_t *>(s), n); }

    // 写 1/2/4 字节大端整数(%1i / %2i / %4i)。
    void put8(std::uint8_t v) { buf_.push_back(v); }
    void put16(std::uint16_t v) {
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }
    void put32(std::uint32_t v) {
        buf_.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
        buf_.push_back(static_cast<std::uint8_t>(v & 0xFF));
    }

    // 写变长字符串(%s):4 字节大端长度 + 原始字节。
    void putString(const std::string &s) {
        put32(static_cast<std::uint32_t>(s.size()));
        raw(reinterpret_cast<const std::uint8_t *>(s.data()), s.size());
    }

    // 写整型向量(%4I):4 字节大端元素个数 + 每个元素 4 字节大端。
    void putIntVector(const std::vector<std::uint32_t> &v) {
        put32(static_cast<std::uint32_t>(v.size()));
        for (auto x : v) put32(x);
    }

    // 产出整帧:[4 字节大端长度=负载长度][负载]。
    std::vector<std::uint8_t> frame() const {
        std::vector<std::uint8_t> out;
        out.reserve(4 + buf_.size());
        std::uint32_t n = static_cast<std::uint32_t>(buf_.size());
        out.push_back(static_cast<std::uint8_t>((n >> 24) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(n & 0xFF));
        out.insert(out.end(), buf_.begin(), buf_.end());
        return out;
    }

private:
    void raw(const std::uint8_t *p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }
    std::vector<std::uint8_t> buf_;
};

// =============================================================================
// 消息构造器:返回"整帧"字节(含长度前缀),可直接交给 socket 发送。
// 每个函数上方注明其线上字节布局,便于与客户端读取器对照。
// =============================================================================

// Hello: [len=11]["Synergy"(7)][major i16][minor i16]。客户端强校验 len==11。
std::vector<std::uint8_t> buildHello(std::uint16_t major, std::uint16_t minor);

// QueryInfo: [len=4]["QINF"]。询问客户端屏幕信息(客户端回 DINF)。
std::vector<std::uint8_t> buildQueryInfo();

// SetOptions: ["DSOP"][count i32][options...]。收到此帧客户端即完成握手并激活屏幕。
// 传入的 options 为 [码,值,码,值...] 序列;可传空(仅发 count=0)。
std::vector<std::uint8_t> buildSetOptions(const std::vector<std::uint32_t> &options);

// InfoAck: [len=4]["CIAK"]。对客户端 DINF 的应答(deskflow 行为,幂等且无害)。
std::vector<std::uint8_t> buildInfoAck();

// KeepAlive: [len=4]["CALV"]。服务端周期性发送;客户端会原样回发一条。
std::vector<std::uint8_t> buildKeepAlive();

// Enter: ["CINN"][x i16][y i16][seq i32][mask i16]。光标进入安卓时发送。
std::vector<std::uint8_t> buildEnter(std::int16_t x, std::int16_t y,
                                     std::uint32_t seq, std::uint16_t mask);

// Leave: [len=4]["COUT"]。光标离开安卓(返回 Mac)时发送。
std::vector<std::uint8_t> buildLeave();

// 绝对移动: ["DMMV"][x i16][y i16]。
std::vector<std::uint8_t> buildMouseMove(std::int16_t x, std::int16_t y);

// 相对移动: ["DMRM"][dx i16][dy i16]。生产主路径。
std::vector<std::uint8_t> buildMouseRelMove(std::int16_t dx, std::int16_t dy);

// 鼠标按下/抬起: ["DMDN"/"DMUP"][button i8]。注意客户端会把所有按键塌缩成左键。
std::vector<std::uint8_t> buildMouseDown(std::uint8_t button);
std::vector<std::uint8_t> buildMouseUp(std::uint8_t button);

// 滚轮: ["DMWM"][xDelta i16][yDelta i16]。
std::vector<std::uint8_t> buildMouseWheel(std::int16_t xDelta, std::int16_t yDelta);

// 键盘按下/抬起: ["DKDN"/"DKUP"][id u16][mask u16][button u16]。
std::vector<std::uint8_t> buildKeyDown(std::uint16_t id, std::uint16_t mask, std::uint16_t button);
std::vector<std::uint8_t> buildKeyUp(std::uint16_t id, std::uint16_t mask, std::uint16_t button);

// 键盘重复: ["DKRP"][id u16][mask u16][count u16][button u16]。
// 只发 4 个 short,不带 deskflow 的尾随 lang 串,以严格匹配客户端读取器。
std::vector<std::uint8_t> buildKeyRepeat(std::uint16_t id, std::uint16_t mask,
                                         std::uint16_t count, std::uint16_t button);

// Close: [len=4]["CBYE"]。
std::vector<std::uint8_t> buildClose();

// =============================================================================
// 读取辅助:从负载缓冲里按大端取整数(带偏移)。调用方负责保证越界安全。
// =============================================================================
inline std::int16_t rd16(const std::uint8_t *p) {
    return static_cast<std::int16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
inline std::uint16_t rdu16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(p[0]) << 8) | p[1]);
}
inline std::uint32_t rd32(const std::uint8_t *p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | static_cast<std::uint32_t>(p[3]);
}

// 判断负载是否以某类型码开头(cmpN 指定比较字节数)。
inline bool typeIs(const std::vector<std::uint8_t> &payload, const char *code, std::size_t n) {
    return payload.size() >= n && std::memcmp(payload.data(), code, n) == 0;
}

// =============================================================================
// FrameReader —— 累积字节流,按 [4B 长度][负载] 切出整帧。单线程使用,无锁。
// =============================================================================
class FrameReader {
public:
    // 追加收到的原始字节。
    void feed(const std::uint8_t *p, std::size_t n) { buf_.insert(buf_.end(), p, p + n); }

    // 尝试取出下一条完整帧的负载(类型码+数据)。成功返回 true 并填充 out。
    // 数据不足返回 false。若发现异常长度(疑似错位)置 error=true,调用方应断开。
    bool pop(std::vector<std::uint8_t> &out, bool &error) {
        error = false;
        if (buf_.size() < 4) return false;
        std::uint32_t len = rd32(buf_.data());
        // 合法帧长上限:本协议消息都很小(DINF/HelloBack 均 < 100 字节)。
        // 超过上限几乎必然是流错位,直接报错让上层重置连接,避免异常分配。
        if (len > kMaxFrame) { error = true; return false; }
        if (buf_.size() < 4 + len) return false;
        out.assign(buf_.begin() + 4, buf_.begin() + 4 + len);
        buf_.erase(buf_.begin(), buf_.begin() + 4 + len);
        return true;
    }

    void reset() { buf_.clear(); }

private:
    static constexpr std::uint32_t kMaxFrame = 64 * 1024; // 64KB 足够,远超任何正常消息
    std::vector<std::uint8_t> buf_;
};

} // namespace proto
} // namespace kms
