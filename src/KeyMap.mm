// =============================================================================
// KeyMap.mm —— KeyMap 的 macOS 实现(Carbon UCKeyTranslate + 固定特殊键表)
// =============================================================================
#include "KeyMap.h"

#import <Carbon/Carbon.h> // UCKeyTranslate / TIS* / kVK_* / LMGetKbdType

namespace kms {

namespace {

// deskflow 特殊键 KeyID 常量(十进制,已按客户端实际支持集合核对)。
// 这些值等价于 deskflow KeyID.h 中的 kKey*,此处直接用数值并注明含义,
// 避免引入 deskflow 头文件。
enum : std::uint16_t {
    kID_Backspace = 61192, // 退格(Mac 的 Delete 键 kVK_Delete)
    kID_Tab       = 61193,
    kID_Return    = 61197, // 回车 / 小键盘回车
    kID_Escape    = 61211, // 注意:客户端表中被注释掉,发了也会被丢弃
    kID_Home      = 61264,
    kID_Left      = 61265,
    kID_Up        = 61266,
    kID_Right     = 61267,
    kID_Down      = 61268,
    kID_PageUp    = 61269,
    kID_PageDown  = 61270,
    kID_End       = 61271,
    kID_Insert    = 61283, // Mac 的 Help 键位
    kID_ShiftL    = 61409,
    kID_ShiftR    = 61410,
    kID_Control   = 61412, // 客户端仅识别 61412,左右 Control 都用它
    kID_CapsLock  = 61413,
    kID_AltL      = 61417,
    kID_AltR      = 61418,
    kID_Delete    = 61439, // 前向删除(kVK_ForwardDelete)
    kID_F1        = 61374, // F1..F12 连续:61374..61385
};

// 用当前键盘布局把虚拟键码翻译成"无修饰时"的基础 Unicode 码点。
// 取基础码点的原因见头文件:大小写/符号靠客户端 + 转发的 Shift 还原。
// 失败(死键 / 无输出 / 控制字符)返回 0。
std::uint16_t baseCodepoint(std::uint16_t macKeycode) {
    TISInputSourceRef src = TISCopyCurrentKeyboardLayoutInputSource();
    if (!src) return 0;
    CFDataRef data = static_cast<CFDataRef>(
        TISGetInputSourceProperty(src, kTISPropertyUnicodeKeyLayoutData));
    std::uint16_t cp = 0;
    if (data) {
        const UCKeyboardLayout *layout =
            reinterpret_cast<const UCKeyboardLayout *>(CFDataGetBytePtr(data));
        UInt32 deadKeyState = 0;
        UniChar chars[8] = {0};
        UniCharCount len = 0;
        // modifierKeyState = 0 → 不带任何修饰,得到基础字符。
        OSStatus st = UCKeyTranslate(layout, macKeycode, kUCKeyActionDown,
                                     0, LMGetKbdType(),
                                     kUCKeyTranslateNoDeadKeysBit,
                                     &deadKeyState, 8, &len, chars);
        if (st == noErr && len >= 1) {
            UniChar c = chars[0];
            // 只接受可打印字符(含空格 0x20);控制字符交给特殊键表处理。
            if (c >= 0x20 && c != 0x7F) cp = c;
        }
    }
    CFRelease(src);
    return cp;
}

} // namespace

KeyMap::Result KeyMap::map(std::uint16_t macKeycode, bool commandAsControl) const {
    Result r;
    r.button = macKeycode; // button 恒用虚拟键码:<256 且 down/up 一致

    // ---- 先查特殊键(非可打印 / 修饰键)。命中即返回。----
    switch (macKeycode) {
        // 编辑 / 导航
        case kVK_Return:        r.id = kID_Return;   r.ok = true; return r;
        case kVK_ANSI_KeypadEnter: r.id = kID_Return; r.ok = true; return r;
        case kVK_Tab:           r.id = kID_Tab;      r.ok = true; return r;
        case kVK_Delete:        r.id = kID_Backspace;r.ok = true; return r; // Mac Delete=退格
        case kVK_ForwardDelete: r.id = kID_Delete;   r.ok = true; return r;
        case kVK_Escape:        r.id = kID_Escape;   r.ok = true; return r; // 客户端会丢弃
        case kVK_Home:          r.id = kID_Home;     r.ok = true; return r;
        case kVK_End:           r.id = kID_End;      r.ok = true; return r;
        case kVK_PageUp:        r.id = kID_PageUp;   r.ok = true; return r;
        case kVK_PageDown:      r.id = kID_PageDown; r.ok = true; return r;
        case kVK_Help:          r.id = kID_Insert;   r.ok = true; return r; // Insert 位
        // 方向键
        case kVK_LeftArrow:     r.id = kID_Left;     r.ok = true; return r;
        case kVK_RightArrow:    r.id = kID_Right;    r.ok = true; return r;
        case kVK_UpArrow:       r.id = kID_Up;       r.ok = true; return r;
        case kVK_DownArrow:     r.id = kID_Down;     r.ok = true; return r;
        // 功能键 F1..F12(KeyID 连续)
        case kVK_F1:  r.id = kID_F1 + 0;  r.ok = true; return r;
        case kVK_F2:  r.id = kID_F1 + 1;  r.ok = true; return r;
        case kVK_F3:  r.id = kID_F1 + 2;  r.ok = true; return r;
        case kVK_F4:  r.id = kID_F1 + 3;  r.ok = true; return r;
        case kVK_F5:  r.id = kID_F1 + 4;  r.ok = true; return r;
        case kVK_F6:  r.id = kID_F1 + 5;  r.ok = true; return r;
        case kVK_F7:  r.id = kID_F1 + 6;  r.ok = true; return r;
        case kVK_F8:  r.id = kID_F1 + 7;  r.ok = true; return r;
        case kVK_F9:  r.id = kID_F1 + 8;  r.ok = true; return r;
        case kVK_F10: r.id = kID_F1 + 9;  r.ok = true; return r;
        case kVK_F11: r.id = kID_F1 + 10; r.ok = true; return r;
        case kVK_F12: r.id = kID_F1 + 11; r.ok = true; return r;
        // 修饰键
        case kVK_Shift:         r.id = kID_ShiftL;  r.isModifier = true; r.ok = true; return r;
        case kVK_RightShift:    r.id = kID_ShiftR;  r.isModifier = true; r.ok = true; return r;
        case kVK_Control:       r.id = kID_Control; r.isModifier = true; r.ok = true; return r;
        case kVK_RightControl:  r.id = kID_Control; r.isModifier = true; r.ok = true; return r;
        case kVK_Option:        r.id = kID_AltL;    r.isModifier = true; r.ok = true; return r;
        case kVK_RightOption:   r.id = kID_AltR;    r.isModifier = true; r.ok = true; return r;
        case kVK_CapsLock:      r.id = kID_CapsLock;r.isModifier = true; r.ok = true; return r;
        // Command:安卓无此键。默认按配置映射为 Control(复制/粘贴等更实用)。
        case kVK_Command:
        case kVK_RightCommand:
            if (commandAsControl) { r.id = kID_Control; r.isModifier = true; r.ok = true; }
            else { r.ok = false; } // 不映射则丢弃(客户端表本就没有 Command)
            return r;
        case kVK_Function: // fn 键:客户端无对应,丢弃
            r.ok = false; return r;
        default: break;
    }

    // ---- 其余按可打印字符处理:取基础码点作为 KeyID。----
    std::uint16_t cp = baseCodepoint(macKeycode);
    if (cp != 0) {
        r.id = cp;
        r.ok = true;
        return r;
    }
    r.ok = false; // 死键 / 无法映射
    return r;
}

} // namespace kms
