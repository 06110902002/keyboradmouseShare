// =============================================================================
// Controller.cpp —— 编排中枢实现
// =============================================================================
#include "Controller.h"

#include <CoreFoundation/CoreFoundation.h> // CFRunLoopTimer(CALV 保活)

#include <cmath>

#include "Log.h"
#include "Protocol.h"

namespace kms {

namespace {

// 把 long 夹到 int16 范围,供坐标/增量/滚轮打包。
std::int16_t clamp16(long v) {
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return static_cast<std::int16_t>(v);
}

// CG 鼠标按钮编号(0=左1=右2=中)→ deskflow 按钮(1=左2=中3=右)。
// 说明:客户端实际会把所有按钮塌缩成左键,这里仍做忠实映射以贴合协议。
int mapButton(int cg) {
    switch (cg) {
        case 0: return 1; // 左
        case 1: return 3; // 右
        case 2: return 2; // 中
        default: return cg + 1;
    }
}

// CFRunLoopTimer 回调 → Controller::sendKeepAlive。
void keepAliveTimerCb(CFRunLoopTimerRef, void *info) {
    static_cast<Controller *>(info)->sendKeepAlive();
}

} // namespace

Controller::~Controller() { stop(); }

// -----------------------------------------------------------------------------
// 启动 CALV 保活定时器:每 1 秒触发一次(仅 Active 时真正发送)。
// deskflow 客户端依赖周期性 CALV 判定服务端存活,收不到会主动断开。
// -----------------------------------------------------------------------------
void Controller::start() {
    CFRunLoopTimerContext ctx = {0, this, nullptr, nullptr, nullptr};
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 1.0, 1.0 /*间隔秒*/,
        0, 0, &keepAliveTimerCb, &ctx);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopCommonModes);
    kaTimer_ = t;
}

void Controller::stop() {
    if (kaTimer_) {
        CFRunLoopTimerRef t = static_cast<CFRunLoopTimerRef>(kaTimer_);
        CFRunLoopTimerInvalidate(t);
        CFRelease(t);
        kaTimer_ = nullptr;
    }
}

void Controller::sendKeepAlive() {
    if (state_ == State::Active) net_.send(proto::buildKeepAlive());
}

// =============================================================================
// NetServerDelegate:连接 / 来包 / 断开
// =============================================================================

// TCP 连接建立:复位状态,服务端先发 Hello 启动握手。
void Controller::onClientConnected() {
    state_ = State::Handshaking;
    optionsSent_ = false;
    onRemote_ = false;
    heldKeys_.clear();
    heldButtons_.clear();
    androidW_ = cfg_.android_width;
    androidH_ = cfg_.android_height;
    // 版本号(1,6):客户端不校验服务端版本、且用固定读取器解析,此处仅作握手标识。
    net_.send(proto::buildHello(1, 6));
    KMS_INFO("已发送 Hello,等待客户端 HelloBack…");
}

void Controller::onFrame(const std::vector<std::uint8_t> &payload) {
    // HelloBack:以 7 字节 "Synergy" 开头(与普通 4 字节类型码区分)。
    if (proto::typeIs(payload, proto::kHello, 7)) {
        // 布局:"Synergy"(7)+major(2)+minor(2)+name(len4+字节)。解析出客户端名仅作日志。
        std::string name;
        if (payload.size() >= 15) {
            std::uint32_t nlen = proto::rd32(payload.data() + 11);
            if (payload.size() >= 15 + nlen)
                name.assign(reinterpret_cast<const char *>(payload.data() + 15), nlen);
        }
        KMS_INFO("收到 HelloBack,客户端名=\"%s\";发送 QINF 查询屏幕信息", name.c_str());
        net_.send(proto::buildQueryInfo());
        return;
    }

    if (payload.size() < 4) return;

    // DINF:客户端屏幕信息。布局 "DINF"+7 个 i16(x,y,w,h,warp,mx,my)=18 字节。
    if (proto::typeIs(payload, proto::kInfo, 4)) {
        if (payload.size() >= 18) {
            int w = proto::rd16(payload.data() + 8);
            int h = proto::rd16(payload.data() + 10);
            if (w > 0 && h > 0) { androidW_ = w; androidH_ = h; }
        }
        net_.send(proto::buildInfoAck()); // CIAK:应答屏幕信息
        if (!optionsSent_) {
            // DSOP:开启相对鼠标模式(生产主路径)。收到此帧客户端即完成握手并可接收输入。
            std::vector<std::uint32_t> opts;
            if (cfg_.mouse_mode == MouseMode::Relative)
                opts = {proto::kOptionRelativeMouseMoves, 1u};
            net_.send(proto::buildSetOptions(opts));
            optionsSent_ = true;
            state_ = State::Active;
            KMS_INFO("握手完成:安卓屏=%dx%d,鼠标模式=%s。就绪。",
                     androidW_, androidH_,
                     cfg_.mouse_mode == MouseMode::Relative ? "相对" : "绝对");
            KMS_INFO("提示:%s;热键=切换主/次屏。",
                     cfg_.switch_mode == SwitchMode::Edge ? "撞触发边自动跨入安卓" : "按热键跨入安卓");
        }
        return;
    }

    if (proto::typeIs(payload, proto::kKeepAlive, 4)) return; // 客户端回显的 CALV,忽略
    if (proto::typeIs(payload, proto::kNoop, 4)) return;
    if (proto::typeIs(payload, proto::kClose, 4)) {           // 客户端请求关闭
        KMS_INFO("客户端发送 CBYE,断开");
        net_.disconnectClient();
        return;
    }
    // 其它消息(如客户端不会主动发的类型)忽略;调试时可看类型码。
    KMS_DEBUG("忽略未处理消息: %c%c%c%c", payload[0], payload[1], payload[2], payload[3]);
}

void Controller::onClientDisconnected() {
    if (onRemote_) { onRemote_ = false; cap_.setControlling(false); } // 还原本机光标
    heldKeys_.clear();
    heldButtons_.clear();
    state_ = State::Disconnected;
    optionsSent_ = false;
    KMS_INFO("连接已断开,等待重新连接…");
}

// =============================================================================
// 主/次屏切换
// =============================================================================

void Controller::enterRemote() {
    ++seq_;
    // CINN:光标进入客户端。相对模式下 x/y 意义不大,给屏幕中心即可。
    net_.send(proto::buildEnter(clamp16(androidW_ / 2), clamp16(androidH_ / 2), seq_, 0));
    absX_ = androidW_ / 2;
    absY_ = androidH_ / 2;
    onRemote_ = true;                 // 先置位,确保随后采集到的事件会被转发
    cap_.setControlling(true);        // 解耦/隐藏本机光标,开始吞事件
}

void Controller::leaveRemote() {
    releaseAllHeld();                 // 防止安卓侧卡键/卡按钮
    net_.send(proto::buildLeave());   // COUT:光标离开
    onRemote_ = false;
    cap_.setControlling(false);       // 还原本机光标
}

void Controller::releaseAllHeld() {
    for (auto &kv : heldKeys_) net_.send(proto::buildKeyUp(kv.second, 0, kv.first));
    heldKeys_.clear();
    for (int b : heldButtons_) net_.send(proto::buildMouseUp(static_cast<std::uint8_t>(mapButton(b))));
    heldButtons_.clear();
}

// =============================================================================
// MacCaptureDelegate:本机事件 → 协议消息(仅在次屏态转发)
// =============================================================================

void Controller::onMouseRelative(int dx, int dy) {
    if (!onRemote_) return;
    if (cfg_.mouse_mode == MouseMode::Relative) {
        net_.send(proto::buildMouseRelMove(clamp16(dx), clamp16(dy)));
    } else {
        // 绝对模式:本地累计虚拟坐标并夹到屏内,发 DMMV。
        absX_ += dx;
        absY_ += dy;
        if (absX_ < 0) absX_ = 0; if (absX_ > androidW_ - 1) absX_ = androidW_ - 1;
        if (absY_ < 0) absY_ = 0; if (absY_ > androidH_ - 1) absY_ = androidH_ - 1;
        net_.send(proto::buildMouseMove(clamp16(absX_), clamp16(absY_)));
    }
}

void Controller::onMouseButton(int cgButton, bool down) {
    if (!onRemote_) return;
    auto db = static_cast<std::uint8_t>(mapButton(cgButton));
    if (down) { net_.send(proto::buildMouseDown(db)); heldButtons_.insert(cgButton); }
    else { net_.send(proto::buildMouseUp(db)); heldButtons_.erase(cgButton); }
}

void Controller::onScroll(int vx, int vy) {
    if (!onRemote_) return;
    // CG 行增量 → deskflow 以 120 为一格的滚轮单位;再乘配置缩放。
    long x = std::lround(vx * 120.0 * cfg_.wheel_scale);
    long y = std::lround(vy * 120.0 * cfg_.wheel_scale);
    if (x == 0 && y == 0) return;
    net_.send(proto::buildMouseWheel(clamp16(x), clamp16(y)));
}

void Controller::onKey(std::uint16_t macKeycode, bool down, bool isRepeat) {
    if (!onRemote_) return;
    sendKey(macKeycode, down, isRepeat);
}

void Controller::onModifier(std::uint16_t macKeycode, bool down) {
    if (!onRemote_) return;
    sendKey(macKeycode, down, false); // 修饰键没有"重复"语义
}

void Controller::sendKey(std::uint16_t macKeycode, bool down, bool repeat) {
    KeyMap::Result r = keymap_.map(macKeycode, cfg_.map_command_to_control);
    if (!r.ok) { KMS_DEBUG("丢弃无法映射的键 kc=%u", macKeycode); return; }
    // mask 统一发 0:客户端忽略 mask,靠单独转发的修饰键还原组合键与大小写。
    if (down) {
        if (repeat) {
            net_.send(proto::buildKeyRepeat(r.id, 0, 1, r.button));
        } else {
            net_.send(proto::buildKeyDown(r.id, 0, r.button));
            heldKeys_[r.button] = r.id; // 记录以便离开时统一抬起
        }
    } else {
        net_.send(proto::buildKeyUp(r.id, 0, r.button));
        heldKeys_.erase(r.button);
    }
}

void Controller::onEdgeHit() {
    if (state_ == State::Active && !onRemote_) enterRemote();
}

void Controller::onToggleHotkey() {
    if (state_ != State::Active) return;
    if (onRemote_) leaveRemote();
    else enterRemote();
}

} // namespace kms
