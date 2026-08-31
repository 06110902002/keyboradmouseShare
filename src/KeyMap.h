// =============================================================================
// KeyMap.h —— macOS 虚拟键码 → deskflow KeyID / button 的映射
//
// 背景(来自对不可改动客户端 InputShare_101227 的逐字节核对):
//   * 客户端用一张 keyTranslation 表把 deskflow KeyID 映射到 Linux 键码。
//     - 可打印字符:KeyID = 该字符的 Unicode 码点(大小写共用同一物理键,靠单独
//       转发的 Shift 键还原大小写);因此服务端应发送"未按 Shift 时"的基础码点。
//     - 特殊键(方向/功能/回车/退格/修饰键等):KeyID = deskflow 约定的固定十进制值,
//       这些值已按客户端实际支持的集合核对(见下方 .mm 中的表)。
//   * 客户端忽略 mask;keyUp 通过 button 反查按下时的 KeyID,故 button 必须 <256、
//     且同一物理键在 down/up 间保持一致 —— 恰好可用 macOS 虚拟键码(0x00–0x7F)。
//
// 本类只做"纯映射",不接触事件流;取基础码点用 UCKeyTranslate(当前键盘布局、
// 无修饰),因此大小写/符号能正确交给客户端 + 转发的修饰键还原。
// =============================================================================
#pragma once

#include <cstdint>

namespace kms {

class KeyMap {
public:
    // 映射结果。
    struct Result {
        std::uint16_t id = 0;      // deskflow KeyID
        std::uint16_t button = 0;  // 用 macOS 虚拟键码充当,满足 <256 且 down/up 一致
        bool isModifier = false;   // 是否为修饰键(Shift/Ctrl/Alt/Cmd/Caps)
        bool ok = false;           // 是否成功映射(false 表示应丢弃该键)
    };

    // 把 macOS 虚拟键码映射为发往客户端的 KeyID + button。
    // commandAsControl:true 时把 Command 键当作 Control 发送(安卓无 Command 键)。
    Result map(std::uint16_t macKeycode, bool commandAsControl) const;
};

} // namespace kms
