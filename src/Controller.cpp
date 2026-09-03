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

// 触发自动切回前,虚拟光标需先"内向"离开返回边的最小像素数。
// 作用:防止"从返回边进入次屏的那一瞬间"因方向抖动被立刻判定越界而弹回主屏。
// 取值很小(相对安卓分辨率可忽略),仅用于消除进入瞬间的抖动,不影响正常切回手感。
constexpr int kReturnArmZone = 24;

// 返回边 = 进入边的对边。安卓摆在 Mac 左侧(edge=left,光标撞 Mac 左边缘进入安卓)时,
// 安卓的右边缘与 Mac 相邻,故从安卓右边缘向右越出即回到 Mac。其余方向同理镜像。
Edge oppositeEdge(Edge e) {
    switch (e) {
        case Edge::Left:   return Edge::Right;
        case Edge::Right:  return Edge::Left;
        case Edge::Top:    return Edge::Bottom;
        case Edge::Bottom: return Edge::Top;
        default:           return Edge::None;
    }
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
    returnEdge_ = oppositeEdge(cfg_.edge); // 预先算好与 Mac 相邻的返回边
    returnArmed_ = false;
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

void Controller::enterRemote(bool viaEdge) {
    ++seq_;
    CGPoint p = cap_.getCursorPos();
    float m_w = cap_.getScreenSize().width;
    float m_h = cap_.getScreenSize().height;
    double curX_ = p.x;
    double curY = p.y;
    double curX_fra = (curX_ + 0.5) / m_w;
    double curY_fra = (curY + 0.5) / m_h;

    // 进入次屏时给"虚拟光标"设初值 = 固定锚点(方案 B)。
    // 为什么用固定锚点、而不是"按 Mac 撞边高度比例进入":
    //   安卓客户端只能相对注入(EV_REL),且系统对注入位移施加【基于速度的指针加速】,
    //   一次性大增量会被非线性放大 → 无法把光标可靠放到某个绝对比例高度(实测总过冲/落右下角)。
    //   因此放弃比例进入,改用可预测的固定锚点:落在与 Mac 相邻"返回边"的【竖直中点】。
    //   返回边所在的那个轴(如右边缘的 x)靠边界钳制到达,与加速无关,精确可靠;
    //   竖直中点是稳定且对称的选择,进入后继续相对移动、撞回返回边即自动切回。
    //   热键进入 → 落在屏幕中心。
    if (viaEdge && returnEdge_ != Edge::None) {
        switch (returnEdge_) {
            case Edge::Right:  absX_ = androidW_ - 1; absY_ = androidH_ / 2;  break;
            case Edge::Left:   absX_ = 0;             absY_ = androidH_ / 2;  break;
            case Edge::Bottom: absX_ = androidW_ / 2; absY_ = androidH_ - 1;  break;
            case Edge::Top:    absX_ = androidW_ / 2; absY_ = 0;              break;
            default:           absX_ = androidW_ / 2; absY_ = androidH_ / 2;  break;
        }
    } else {
        absX_ = androidW_ / 2;
        absY_ = androidH_ / 2;
    }
    returnArmed_ = false; // 进入后需先内向离开返回边 kReturnArmZone 像素才武装,防瞬间弹回
    // CINN:光标进入客户端。相对模式下坐标意义不大,发送虚拟初值即可。
    KMS_INFO("当前主屏坐标 x = %f  y = %f,"
             "进入次屏,虚拟光标=%d,%d   x 比例:%f y 比例:%f", curX_,curY,absX_, absY_,curX_fra, curY_fra);
    net_.send(proto::buildEnter(clamp16(absX_), clamp16(absY_), seq_, 0));
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

// -----------------------------------------------------------------------------
// 自动切回主屏(模仿 deskflow 绝对模式:光标在次屏累积移动,越过与主屏相邻的边即切回)。
// 相对模式下客户端不回报光标位置,故服务端用 absX_/absY_ 维护一份"虚拟光标":
//   * 调用前 absX_/absY_ 已累加本次 delta;
//   * 只有配置的"返回边"(与 Mac 相邻的一侧)越界才切回,其余三边由调用方夹住(无邻居→不跨越);
//   * 必须先"武装"(内向离开返回边 kReturnArmZone 像素)才允许触发,避免进入瞬间弹回。
// 返回 true 表示已调用 leaveRemote(调用方应立即结束本次事件处理)。
// deskflow 对应逻辑:Server::onMouseMoveSecondary() 的累加 + 方向判定 + switchScreen/夹边。
// -----------------------------------------------------------------------------
bool Controller::updateAutoReturn() {
    const int aw = androidW_, ah = androidH_;
    int inward;   // 到返回边的"内向"距离(越大 = 越深入次屏)
    bool crossed; // 是否已越过返回边(与 deskflow 的 m_x>ax+aw-1 等判定一致)
    switch (returnEdge_) {
        case Edge::Right:  inward = (aw - 1) - absX_; crossed = absX_ > aw - 1; break;
        case Edge::Left:   inward = absX_;            crossed = absX_ < 0;      break;
        case Edge::Bottom: inward = (ah - 1) - absY_; crossed = absY_ > ah - 1; break;
        case Edge::Top:    inward = absY_;            crossed = absY_ < 0;      break;
        default:           return false; // 无返回边(edge=none):不自动切回
    }
    if (!returnArmed_ && inward >= kReturnArmZone) returnArmed_ = true;
    if (crossed && returnArmed_) {
        KMS_INFO("虚拟光标越过返回边 → 自动切回主屏");
        leaveRemote();
        return true;
    }
    return false;
}

// =============================================================================
// MacCaptureDelegate:本机事件 → 协议消息(仅在次屏态转发)
// =============================================================================

void Controller::onMouseRelative(int dx, int dy) {
    if (!onRemote_) return;

    // 相对模式:把原始增量直接发给客户端(客户端做相对注入,快甩可达任意位置)。
    if (cfg_.mouse_mode == MouseMode::Relative)
        net_.send(proto::buildMouseRelMove(clamp16(dx), clamp16(dy)));

    // 维护服务端"虚拟光标":相对模式靠它判断何时自动切回主屏;绝对模式还用它作为发送坐标。
    absX_ += dx;
    absY_ += dy;

    // 自动切回:虚拟光标越过与 Mac 相邻的"返回边"且已武装 → 回主屏(见 updateAutoReturn)。
    if (autoReturnActive() && updateAutoReturn()) return;

    // 未触发返回:按 deskflow 方式把虚拟光标夹回屏内(四边都夹,防无界漂移)。
    if (absX_ < 0) absX_ = 0; else if (absX_ > androidW_ - 1) absX_ = androidW_ - 1;
    if (absY_ < 0) absY_ = 0; else if (absY_ > androidH_ - 1) absY_ = androidH_ - 1;

    // 绝对模式:发送夹取后的绝对坐标 DMMV。
    if (cfg_.mouse_mode == MouseMode::Absolute)
        net_.send(proto::buildMouseMove(clamp16(absX_), clamp16(absY_)));
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
    if (state_ == State::Active && !onRemote_) enterRemote(true); // 撞边进入
}

void Controller::onToggleHotkey() {
    if (state_ != State::Active) return;
    if (onRemote_) leaveRemote();
    else enterRemote(false); // 热键进入(落在屏幕中心)
}

} // namespace kms
