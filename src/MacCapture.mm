// =============================================================================
// MacCapture.mm —— MacCapture 的 macOS 实现(CGEventTap + 光标控制)
// =============================================================================
#include "MacCapture.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

#include "Log.h"

namespace kms {

MacCapture::~MacCapture() { stop(); }

// -----------------------------------------------------------------------------
// 辅助功能授权检查。CGEventTap 采集全局键鼠需要"辅助功能"权限。
// 1. AXIsProcessTrusted():只查询当前进程是否已被授权,不弹窗。
// 2. 未授权且 prompt == true 时,通过 AXIsProcessTrustedWithOptions + kAXTrustedCheckOptionPrompt 主动调起系统授权引导(系统设置 → 隐私与安全性 → 辅助功能)。
// 3. 授权是静态的,所以在 main.mm 启动流程里最先调用,失败就直接退出并提示。
// -----------------------------------------------------------------------------
bool MacCapture::ensureAccessibility(bool prompt) {
    if (AXIsProcessTrusted()) return true;
    if (prompt) {
        const void *keys[] = {kAXTrustedCheckOptionPrompt};
        const void *vals[] = {kCFBooleanTrue};
        CFDictionaryRef opts = CFDictionaryCreate(
            kCFAllocatorDefault, keys, vals, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        bool ok = AXIsProcessTrustedWithOptions(opts);
        CFRelease(opts);
        return ok;
    }
    return false;
}

// -----------------------------------------------------------------------------
// 创建事件 tap。kCGEventTapOptionDefault=可修改/吞事件(次屏态需要吞掉本机输入)。
// -----------------------------------------------------------------------------
bool MacCapture::start() {
    CGEventMask mask =
        CGEventMaskBit(kCGEventMouseMoved) | CGEventMaskBit(kCGEventLeftMouseDown) |
        CGEventMaskBit(kCGEventLeftMouseUp) | CGEventMaskBit(kCGEventRightMouseDown) |
        CGEventMaskBit(kCGEventRightMouseUp) | CGEventMaskBit(kCGEventOtherMouseDown) |
        CGEventMaskBit(kCGEventOtherMouseUp) | CGEventMaskBit(kCGEventLeftMouseDragged) |
        CGEventMaskBit(kCGEventRightMouseDragged) | CGEventMaskBit(kCGEventOtherMouseDragged) |
        CGEventMaskBit(kCGEventScrollWheel) | CGEventMaskBit(kCGEventKeyDown) |
        CGEventMaskBit(kCGEventKeyUp) | CGEventMaskBit(kCGEventFlagsChanged);

    tapPort_ = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap,
                                kCGEventTapOptionDefault, mask,
                                &MacCapture::tapCallback, this);
    if (!tapPort_) {
        KMS_ERROR("创建 CGEventTap 失败(通常是未授予辅助功能权限)");
        return false;
    }
    tapSource_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tapPort_, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), tapSource_, kCFRunLoopCommonModes);
    CGEventTapEnable(tapPort_, true);
    KMS_INFO("键鼠采集已启动(CGEventTap)");
    return true;
}

void MacCapture::stop() {
    if (controlling_) { endRemoteCursor(); controlling_ = false; }
    if (tapSource_) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), tapSource_, kCFRunLoopCommonModes);
        CFRelease(tapSource_);
        tapSource_ = nullptr;
    }
    if (tapPort_) {
        CGEventTapEnable(tapPort_, false);
        CFMachPortInvalidate(tapPort_);
        CFRelease(tapPort_);
        tapPort_ = nullptr;
    }
}

CGEventRef MacCapture::tapCallback(CGEventTapProxy, CGEventType type,
                                   CGEventRef event, void *userInfo) {
    return static_cast<MacCapture *>(userInfo)->handleEvent(type, event);
}

// 把 CGEventFlags 归一化成 Config 的 ModBits(仅四个主修饰位)。
unsigned MacCapture::normalizeMods(CGEventFlags flags) const {
    unsigned m = 0;
    if (flags & kCGEventFlagMaskShift) m |= Mod_Shift;
    if (flags & kCGEventFlagMaskControl) m |= Mod_Control;
    if (flags & kCGEventFlagMaskAlternate) m |= Mod_Alt;
    if (flags & kCGEventFlagMaskCommand) m |= Mod_Command;
    return m;
}

// keyDown 是否命中切换热键:主键码相等 + 四个修饰位精确一致。
bool MacCapture::matchesHotkey(CGEventRef event, std::uint16_t keycode) const {
    if (cfg_.hotkey.keycode < 0) return false;
    if (keycode != static_cast<std::uint16_t>(cfg_.hotkey.keycode)) return false;
    return normalizeMods(CGEventGetFlags(event)) == cfg_.hotkey.mods;
}

// -----------------------------------------------------------------------------
// 核心事件处理。返回原事件=放行;返回 NULL=吞掉。
// -----------------------------------------------------------------------------
CGEventRef MacCapture::handleEvent(CGEventType type, CGEventRef event) {
    // tap 因超时/被用户输入打断而失效时,重新启用,保证采集不中断。
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        if (tapPort_) CGEventTapEnable(tapPort_, true);
        return event;
    }

    // ---- 修饰键:两种态都要维护 pressedMods_,以保证 down/up 判定一致 ----
    if (type == kCGEventFlagsChanged) {
        auto kc = static_cast<std::uint16_t>(
            CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
        bool down;
        auto it = pressedMods_.find(kc);
        if (it == pressedMods_.end()) { down = true; pressedMods_.insert(kc); }
        else { down = false; pressedMods_.erase(it); }
        if (controlling_) {
            if (delegate_) delegate_->onModifier(kc, down);
            return nullptr; // 次屏吞掉
        }
        return event; // 主屏放行
    }

    // ---- 切换热键:仅在真实按下(非自动重复)时触发,任意态生效 ----
    if (type == kCGEventKeyDown) {
        bool repeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
        auto kc = static_cast<std::uint16_t>(
            CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
        if (!repeat && matchesHotkey(event, kc)) {
            if (delegate_) delegate_->onToggleHotkey();
            return nullptr; // 吞掉热键本身
        }
    }

    if (controlling_) {
        // ===================== 次屏态:吞掉本机输入并转发 =====================
        switch (type) {
            case kCGEventMouseMoved:
            case kCGEventLeftMouseDragged:
            case kCGEventRightMouseDragged:
            case kCGEventOtherMouseDragged: {
                // 相对增量取事件字段(event-tap 架构下唯一可靠来源)。
                int dx = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
                int dy = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
                if ((dx || dy) && delegate_) delegate_->onMouseRelative(dx, dy);
                return nullptr;
            }
            case kCGEventLeftMouseDown:  if (delegate_) delegate_->onMouseButton(0, true);  return nullptr;
            case kCGEventLeftMouseUp:    if (delegate_) delegate_->onMouseButton(0, false); return nullptr;
            case kCGEventRightMouseDown: if (delegate_) delegate_->onMouseButton(1, true);  return nullptr;
            case kCGEventRightMouseUp:   if (delegate_) delegate_->onMouseButton(1, false); return nullptr;
            case kCGEventOtherMouseDown: {
                int b = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber));
                if (delegate_) delegate_->onMouseButton(b, true);
                return nullptr;
            }
            case kCGEventOtherMouseUp: {
                int b = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventButtonNumber));
                if (delegate_) delegate_->onMouseButton(b, false);
                return nullptr;
            }
            case kCGEventScrollWheel: {
                int v1 = static_cast<int>(CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1)); // 纵向
                int v2 = static_cast<int>(CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis2)); // 横向
                if ((v1 || v2) && delegate_) delegate_->onScroll(v2, v1);
                return nullptr;
            }
            case kCGEventKeyDown: {
                bool repeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
                auto kc = static_cast<std::uint16_t>(
                    CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
                if (delegate_) delegate_->onKey(kc, true, repeat);
                return nullptr;
            }
            case kCGEventKeyUp: {
                auto kc = static_cast<std::uint16_t>(
                    CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
                if (delegate_) delegate_->onKey(kc, false, false);
                return nullptr;
            }
            default:
                return nullptr; // 次屏态其它事件一律吞掉
        }
    }

    // ========================= 主屏态:仅监视撞边,其余放行 =========================
    if (cfg_.switch_mode == SwitchMode::Edge && cfg_.edge != Edge::None &&
        (type == kCGEventMouseMoved || type == kCGEventLeftMouseDragged ||
         type == kCGEventRightMouseDragged || type == kCGEventOtherMouseDragged)) {
        CGPoint p = CGEventGetLocation(event);
        int dx = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaX));
        int dy = static_cast<int>(CGEventGetIntegerValueField(event, kCGMouseEventDeltaY));
        CGRect b = CGDisplayBounds(CGMainDisplayID());
        bool hit = false;
        switch (cfg_.edge) {
            // 撞边判定:贴着该边(坐标到达极值)且仍朝该方向移动,避免静止时误触。
            case Edge::Left:   hit = (p.x <= b.origin.x + 0.5) && dx < 0; break;
            case Edge::Right:  hit = (p.x >= b.origin.x + b.size.width - 1.5) && dx > 0; break;
            case Edge::Top:    hit = (p.y <= b.origin.y + 0.5) && dy < 0; break;
            case Edge::Bottom: hit = (p.y >= b.origin.y + b.size.height - 1.5) && dy > 0; break;
            default: break;
        }
        if (hit) {
            if (delegate_) delegate_->onEdgeHit(); // Controller 可能在此同步切入次屏
            if (controlling_) return nullptr;      // 已切入则吞掉这一帧,光标随即隐藏
        }
    }
    return event; // 主屏放行,本机正常使用
}

// -----------------------------------------------------------------------------
// 进入次屏:记录当前光标位置 → 解耦(冻结)光标 → 隐藏。
// 解耦后系统光标不再随物理移动而移动,但 tap 仍能收到带 delta 的 mouseMoved 事件,
// 因此快甩也不会被屏幕边界"钳住"归零(这正是相对模式可达任意位置的关键)。
// 注意:不做 warp-to-center —— 解耦后无需靠居中防边界,且 warp 可能产生一次
// 虚假大 delta 导致进入瞬间跳一下。
//
// 隐藏光标的坑:CGDisplayHideCursor 主要对"前台应用"生效,而本程序是纯命令行
// 后台进程,直接调用会被系统忽略(表现为光标依旧可见)。因此必须先通过
// WindowServer 私有接口声明"可在后台操作光标"(SetsCursorInBackground),
// 再调用 CGDisplayHideCursor 才会真正隐藏。
// -----------------------------------------------------------------------------
// 设置/清除 "SetsCursorInBackground" 连接属性,授予后台进程操作光标的资格。
// 私有符号用 dlopen/dlsym 运行时加载:系统改版或符号缺失时静默降级(保持旧行为)。
static void setCursorHideInBackground(bool enable) {
    static void *handle = dlopen(
        "/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight", RTLD_LAZY | RTLD_LOCAL);
    if (!handle) return;
    using MainConnFn = int (*)();
    using SetPropFn = int (*)(int, int, CFStringRef, CFTypeRef);
    auto mainConn = reinterpret_cast<MainConnFn>(dlsym(handle, "CGSMainConnectionID"));
    auto setProp = reinterpret_cast<SetPropFn>(dlsym(handle, "CGSSetConnectionProperty"));
    if (!mainConn || !setProp) return;
    const int cid = mainConn();
    setProp(cid, cid, CFSTR("SetsCursorInBackground"),
            enable ? kCFBooleanTrue : kCFBooleanFalse);
}

void MacCapture::beginRemoteCursor() {
    savedCursor_ = getCursorPos();

    // 关键:后台进程先取得"后台操作光标"资格,否则下面的隐藏会被系统忽略。
    setCursorHideInBackground(true);
    /**
     * CGAssociateMouseAndMouseCursorPosition(bool associate) 是 macOS CoreGraphics 的光标解耦开关,用于把"物理鼠标移动"和"系统光标位置"之间的联动关系切断或恢复
     * 参数 true:正常模式。鼠标怎么动,系统光标就跟着怎么动(默认状态)。
     * 参数 false:解耦模式。鼠标移动不再驱动系统光标的位置,但操作系统仍然能感知到鼠标在动,并且能通过事件读到移动增量。
     * 在这个项目里为什么必须用
     * MacCapture.mm 的次屏态(控制安卓)需要把本机鼠标的相对增量转发给安卓,而不是光标位置。如果不解耦,会出两个问题:
     * 1. 增量被"钳"掉:系统光标碰到屏幕边缘就走不动了,此时即使你继续快速甩鼠标,采集到的 kCGMouseEventDeltaX/Y 会变成 0。解耦后光标被"冻结"在屏幕上,物理鼠标怎么甩都能继续产生增量——这正是相对模式"快甩可达任意位置、不困于屏幕矩形"的关键。
     * 2. 光标乱跑:次屏态下 Mac 本机不该再响应鼠标,如果光标还跟着动,屏幕上会出现一个光标到处乱飞,且我们是要隐藏它的。
     *
     */
    CGAssociateMouseAndMouseCursorPosition(false);
    CGDisplayHideCursor(kCGDirectMainDisplay);
}

// 退出次屏:显示光标 → 恢复光标关联 → 移回原位 → 收回后台光标操作资格。
void MacCapture::endRemoteCursor() {
    CGDisplayShowCursor(kCGDirectMainDisplay);
    CGAssociateMouseAndMouseCursorPosition(true);
    CGWarpMouseCursorPosition(savedCursor_);
    setCursorHideInBackground(false);
}

void MacCapture::setControlling(bool on) {
    if (on == controlling_) return;
    controlling_ = on;
    if (on) {
        beginRemoteCursor();
        KMS_INFO("→ 进入次屏:开始控制安卓");
    } else {
        endRemoteCursor();
        KMS_INFO("← 返回主屏:恢复本机控制");
    }
}

CGPoint MacCapture::getCursorPos() const {
    CGEventRef e = CGEventCreate(nullptr);
    CGPoint p = CGEventGetLocation(e);
    CFRelease(e);
    return p;
}

CGSize MacCapture::getScreenSize() const {
    CGRect b = CGDisplayBounds(CGMainDisplayID());
    return {b.size.width, b.size.height};
}

} // namespace kms
