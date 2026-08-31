// =============================================================================
// MacCapture.mm —— MacCapture 的 macOS 实现(CGEventTap + 光标控制)
// =============================================================================
#include "MacCapture.h"

#include "Log.h"

namespace kms {

MacCapture::~MacCapture() { stop(); }

// -----------------------------------------------------------------------------
// 辅助功能授权检查。CGEventTap 采集全局键鼠需要"辅助功能"权限。
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
// -----------------------------------------------------------------------------
void MacCapture::beginRemoteCursor() {
    CGEventRef e = CGEventCreate(nullptr);
    savedCursor_ = CGEventGetLocation(e);
    CFRelease(e);

    CGAssociateMouseAndMouseCursorPosition(false);
    CGDisplayHideCursor(kCGDirectMainDisplay);
}

// 退出次屏:恢复光标关联 → 移回原位 → 显示。
void MacCapture::endRemoteCursor() {
    CGAssociateMouseAndMouseCursorPosition(true);
    CGWarpMouseCursorPosition(savedCursor_);
    CGDisplayShowCursor(kCGDirectMainDisplay);
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

} // namespace kms
