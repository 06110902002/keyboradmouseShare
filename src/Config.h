// =============================================================================
// Config.h —— 运行期配置(来自纯文本配置文件,key=value)
//
// 本程序是"单进程、无 UI"的服务端,所有可调项都从一个配置文件读取,改动端口/
// 跨屏方式/热键/方向等都无需重新编译。Config 负责:定义配置结构 + 默认值,
// 解析配置文件,以及把 "ctrl+5" 这类热键字符串解析成(修饰键掩码 + 虚拟键码)。
// =============================================================================
#pragma once

#include <string>

namespace kms {

// 光标从 Mac 跨入安卓所使用的"触发边":撞到该边即自动跨越。
// 用户默认选择 Left(安卓摆在 Mac 左侧)。None 表示不启用撞边(只用热键)。
enum class Edge { None, Left, Right, Top, Bottom };

// 跨屏方式:Edge=撞边自动跨越;Hotkey=按热键切换。两种都实现,由此项选默认启用哪种。
// 注意:相对模式下客户端不会把光标位置回报给服务端,因此"从安卓返回 Mac"总是依赖热键。
enum class SwitchMode { Edge, Hotkey };

// 鼠标移动模式:Relative=发相对增量 DMRM(推荐,快甩可达且不困于矩形,已被验证);
// Absolute=发绝对坐标 DMMV(备用)。
enum class MouseMode { Relative, Absolute };

// 自定义修饰键位(自有位定义,避免在纯 C++ 层引入 CoreGraphics 头文件)。
// MacCapture 会把系统事件的 flags 归一化成这套位再和热键比较。
enum ModBits : unsigned {
    Mod_Shift   = 1u << 0,
    Mod_Control = 1u << 1,
    Mod_Alt     = 1u << 2, // macOS 的 Option 键
    Mod_Command = 1u << 3,
};

// 一个热键 = 一组修饰键 + 一个主键(macOS 虚拟键码)。keycode<0 表示未设置。
struct Hotkey {
    unsigned mods = 0;
    int keycode = -1;
};

// 全部运行期配置。默认值即"开箱即用"的推荐配置。
struct Config {
    int listen_port = 24800;                 // 监听端口(与 deskflow/客户端默认一致)
    Edge edge = Edge::Left;                  // 默认撞左边缘跨入安卓
    SwitchMode switch_mode = SwitchMode::Edge; // 默认启用撞边;可改为 hotkey
    Hotkey hotkey{Mod_Control, 0x17};        // 默认切换热键 = Control+5(0x17=kVK_ANSI_5)
    bool map_command_to_control = true;      // 把 Mac 的 Command 映射为 Control(安卓无 Command,
                                             // 且复制/粘贴等在安卓用 Ctrl,更实用)
    MouseMode mouse_mode = MouseMode::Relative; // 默认相对模式
    double wheel_scale = 1.0;                // 滚轮增量缩放
    int android_width = 1200;                // 安卓宽(兜底;握手时会被客户端 DINF 覆盖)
    int android_height = 2670;               // 安卓高(兜底;同上)
    bool verbose = false;                    // 是否打印 DEBUG 日志
    std::string server_name = "mac";         // 仅用于日志展示

    // 从文件加载配置。文件不存在时返回 false(调用方可选择用默认值继续)。
    // 解析尽量宽容:无法识别的键会告警但不致命,保证少写一项也能跑起来。
    bool loadFromFile(const std::string &path);

    // 把当前配置摘要打印到日志,便于启动时确认。
    void logSummary() const;
};

// 把形如 "ctrl+5"、"control+shift+f1"、"cmd+space" 的字符串解析为 Hotkey。
// 解析失败返回 false 且不修改 out。供 Config 解析和单元自检复用。
bool parseHotkey(const std::string &s, Hotkey &out);

} // namespace kms
