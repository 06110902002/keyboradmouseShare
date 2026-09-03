// =============================================================================
// MacCapture.h —— 用 CGEventTap 采集本机键鼠事件(单线程,挂主 CFRunLoop)
//
// 两种运行态(由 Controller 通过 setControlling 切换):
//   * 主屏态(controlling=false):事件原样放行,Mac 本机正常使用。仅在此态下
//     监视"光标撞触发边"(用于自动跨入安卓)。
//   * 次屏态(controlling=true):吞掉所有本机键鼠事件(回调返回 NULL),并把
//     鼠标相对增量 / 按键 / 滚轮转发给 Controller 发往安卓;同时把系统光标
//     解耦冻结 + 隐藏,这样既拿得到原始 delta,本机光标又不会乱动/可见。
//
// 切换热键在两种态下都生效(相对模式下无法靠撞边从安卓返回,故返回依赖热键)。
// =============================================================================
#pragma once

#include <ApplicationServices/ApplicationServices.h> // CGEventTap / CGEvent / 光标 API

#include <cstdint>
#include <unordered_set>

#include "Config.h"

namespace kms {

// 采集事件的语义回调。由 Controller 实现,全部在主线程回调,无需加锁。
class MacCaptureDelegate {
public:
    virtual ~MacCaptureDelegate() = default;
    // 次屏:鼠标相对移动增量(像素)。
    virtual void onMouseRelative(int dx, int dy) = 0;
    // 次屏:鼠标按键。cgButton:0=左 1=右 2=中/其它。down=按下。
    virtual void onMouseButton(int cgButton, bool down) = 0;
    // 次屏:滚轮行增量(vx 横向、vy 纵向;向上为正,遵循 CG 约定)。
    virtual void onScroll(int vx, int vy) = 0;
    // 次屏:普通(非修饰)键 down/up;isRepeat 表示系统自动重复产生的按下。
    virtual void onKey(std::uint16_t macKeycode, bool down, bool isRepeat) = 0;
    // 次屏:修饰键(Shift/Ctrl/Alt/Cmd/Caps)状态变化。
    virtual void onModifier(std::uint16_t macKeycode, bool down) = 0;
    // 主屏:光标撞到配置的触发边(仅 switch_mode=Edge 时上报),请求跨入安卓。
    virtual void onEdgeHit() = 0;
    // 切换热键被按下(任意方向);由 Controller 决定切入/切回。
    virtual void onToggleHotkey() = 0;
};

class MacCapture {
public:
    explicit MacCapture(const Config &cfg) : cfg_(cfg) {}
    ~MacCapture();

    MacCapture(const MacCapture &) = delete;
    MacCapture &operator=(const MacCapture &) = delete;

    void setDelegate(MacCaptureDelegate *d) { delegate_ = d; }

    // 检查辅助功能(Accessibility)授权。prompt=true 时弹出系统授权引导。
    // 未授权时 CGEventTap 无法采集,应提示用户到"系统设置→隐私与安全性→辅助功能"授权。
    static bool ensureAccessibility(bool prompt);

    // 创建事件 tap 并挂到当前 CFRunLoop。需在主线程调用。失败返回 false。
    bool start();
    void stop();

    // 进入/退出"控制安卓"态:切换吞事件行为,并做光标解耦/隐藏或还原。
    void setControlling(bool on);
    bool isControlling() const { return controlling_; }

    /**
     * 获取当前光标位置
     * @return 返回当前光标位置
     */
    CGPoint getCursorPos() const;

    CGSize getScreenSize() const;


private:
    // CGEventTap 的 C 回调 → 转发到成员。userInfo 传 this。
    static CGEventRef tapCallback(CGEventTapProxy proxy, CGEventType type,
                                  CGEventRef event, void *userInfo);
    // 核心事件处理:返回原事件=放行,返回 NULL=吞掉。
    CGEventRef handleEvent(CGEventType type, CGEventRef event);

    // 判断某个 keyDown 是否命中切换热键(键码匹配 + 修饰键精确匹配)。
    bool matchesHotkey(CGEventRef event, std::uint16_t keycode) const;
    // 把 CGEventFlags 归一化成 Config 的 ModBits(仅 Shift/Ctrl/Alt/Cmd 四位)。
    unsigned normalizeMods(CGEventFlags flags) const;

    // 光标状态控制。
    void beginRemoteCursor(); // 解耦 + 隐藏 + 记录原位
    void endRemoteCursor();   // 还原关联 + 显示 + 回到原位

    const Config &cfg_;
    MacCaptureDelegate *delegate_ = nullptr;

    CFMachPortRef tapPort_ = nullptr;      // 事件 tap
    CFRunLoopSourceRef tapSource_ = nullptr; // 对应 RunLoop 源
    bool controlling_ = false;             // 当前是否处于次屏(控制安卓)态

    // 当前按下的修饰键(按 macOS 虚拟键码)。FlagsChanged 靠"是否已在集合中"判定
    // 本次是按下还是抬起,规避左右修饰共享同一 flag 掩码的歧义。
    std::unordered_set<std::uint16_t> pressedMods_;

    CGPoint savedCursor_{0, 0}; // 进入次屏前的光标位置,退出时还原
};

} // namespace kms
