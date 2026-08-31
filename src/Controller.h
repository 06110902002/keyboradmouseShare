// =============================================================================
// Controller.h —— 编排中枢:握手状态机 + 主/次屏切换 + 事件→协议消息分发
//
// 同时实现两个回调接口:
//   * NetServerDelegate:处理与客户端的握手与来包(HelloBack/DINF/CALV/CBYE)。
//   * MacCaptureDelegate:把本机键鼠事件转成 deskflow 消息发往客户端。
//
// 握手时序(服务端主动,严格复刻 deskflow 服务端 → 客户端流程):
//   连接 → 发 Hello → 收 HelloBack(客户端名) → 发 QINF → 收 DINF(屏幕尺寸)
//        → 发 CIAK + 发 DSOP(开启相对鼠标) → 进入 Active,周期发 CALV 保活。
// Active 后默认停留在主屏;撞触发边(Edge 模式)或按切换热键才跨入次屏(控制安卓)。
// 相对模式下客户端不回报光标位置,故"从安卓返回主屏"只能靠热键。
// =============================================================================
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Config.h"
#include "KeyMap.h"
#include "MacCapture.h"
#include "NetServer.h"

namespace kms {

class Controller : public NetServerDelegate, public MacCaptureDelegate {
public:
    Controller(const Config &cfg, NetServer &net, MacCapture &cap)
        : cfg_(cfg), net_(net), cap_(cap),
          androidW_(cfg.android_width), androidH_(cfg.android_height) {}
    ~Controller();

    Controller(const Controller &) = delete;
    Controller &operator=(const Controller &) = delete;

    void start(); // 启动 CALV 保活定时器
    void stop();

    // ---- NetServerDelegate ----
    void onClientConnected() override;
    void onFrame(const std::vector<std::uint8_t> &payload) override;
    void onClientDisconnected() override;

    // ---- MacCaptureDelegate ----
    void onMouseRelative(int dx, int dy) override;
    void onMouseButton(int cgButton, bool down) override;
    void onScroll(int vx, int vy) override;
    void onKey(std::uint16_t macKeycode, bool down, bool isRepeat) override;
    void onModifier(std::uint16_t macKeycode, bool down) override;
    void onEdgeHit() override;
    void onToggleHotkey() override;


    enum class State { Disconnected, Handshaking, Active };

    void enterRemote();       // 跨入次屏:发 CINN + 切换采集为吞事件/转发
    void leaveRemote();       // 返回主屏:释放已按键 + 发 COUT + 还原采集
    void releaseAllHeld();    // 把当前按住的键/鼠标按钮在客户端侧全部抬起,防卡键
    void sendKey(std::uint16_t macKeycode, bool down, bool repeat); // 统一的键发送
    void sendKeepAlive();     // 定时器回调里发 CALV

    const Config &cfg_;
    NetServer &net_;
    MacCapture &cap_;
    KeyMap keymap_;

    State state_ = State::Disconnected;
    bool optionsSent_ = false; // 是否已发过 DSOP(避免重复)
    bool onRemote_ = false;    // 当前是否处于次屏(控制安卓)
    std::uint32_t seq_ = 0;    // CINN 序号,每次进入自增
    int androidW_;             // 安卓屏宽(启动用配置兜底,收到 DINF 后更新)
    int androidH_;             // 安卓屏高
    int absX_ = 0, absY_ = 0;  // 绝对模式下维护的虚拟坐标(相对模式不用)

    std::unordered_map<std::uint16_t, std::uint16_t> heldKeys_; // button(键码) → KeyID
    std::unordered_set<int> heldButtons_;                        // 按住的鼠标按钮(cg 编号)

    void *kaTimer_ = nullptr; // CFRunLoopTimerRef(保活定时器)
};

} // namespace kms
