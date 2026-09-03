// =============================================================================
// Config.cpp —— 配置文件解析实现
// =============================================================================
#include "Config.h"
#include "Log.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace kms {

namespace {

// 去除字符串首尾空白(空格/制表/回车换行)。
std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 转小写,便于大小写不敏感地比较键名/枚举值。
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

// 解析布尔:true/1/yes/on 视为真;其余为假。
bool parseBool(const std::string &v) {
    std::string x = lower(trim(v));
    return x == "true" || x == "1" || x == "yes" || x == "on";
}

// 主键名 → macOS 虚拟键码(kVK_*)。这里直接用数值常量,避免让纯 C++ 的 Config
// 依赖 Carbon 头文件;数值取自 <Carbon/HIToolbox/Events.h>,已在注释中标注含义。
const std::unordered_map<std::string, int> &keyNameTable() {
    static const std::unordered_map<std::string, int> t = {
        // 字母(kVK_ANSI_A..Z)
        {"a",0x00},{"s",0x01},{"d",0x02},{"f",0x03},{"h",0x04},{"g",0x05},
        {"z",0x06},{"x",0x07},{"c",0x08},{"v",0x09},{"b",0x0B},{"q",0x0C},
        {"w",0x0D},{"e",0x0E},{"r",0x0F},{"y",0x10},{"t",0x11},{"o",0x1F},
        {"u",0x20},{"i",0x22},{"p",0x23},{"l",0x25},{"j",0x26},{"k",0x28},
        {"n",0x2D},{"m",0x2E},
        // 数字(kVK_ANSI_0..9)
        {"0",0x1D},{"1",0x12},{"2",0x13},{"3",0x14},{"4",0x15},
        {"5",0x17},{"6",0x16},{"7",0x1A},{"8",0x1C},{"9",0x19},
        // 功能键
        {"f1",0x7A},{"f2",0x78},{"f3",0x63},{"f4",0x76},{"f5",0x60},{"f6",0x61},
        {"f7",0x62},{"f8",0x64},{"f9",0x65},{"f10",0x6D},{"f11",0x67},{"f12",0x6F},
        // 常用命名键
        {"space",0x31},{"return",0x24},{"enter",0x24},{"tab",0x30},{"escape",0x35},{"esc",0x35},
    };
    return t;
}

} // namespace

// -----------------------------------------------------------------------------
// 热键字符串解析:用 '+' 分隔,最后一个非修饰 token 作主键,其余作修饰键。
// 例:"ctrl+5" → mods=Control, keycode=0x17;"control+shift+f1" 亦可。
// -----------------------------------------------------------------------------
bool parseHotkey(const std::string &s, Hotkey &out) {
    Hotkey hk;
    std::stringstream ss(s);
    std::string tok;
    bool gotKey = false;
    while (std::getline(ss, tok, '+')) {
        std::string t = lower(trim(tok));
        if (t.empty()) continue;
        if (t == "ctrl" || t == "control") { hk.mods |= Mod_Control; continue; }
        if (t == "shift") { hk.mods |= Mod_Shift; continue; }
        if (t == "alt" || t == "opt" || t == "option") { hk.mods |= Mod_Alt; continue; }
        if (t == "cmd" || t == "command" || t == "super" || t == "win") { hk.mods |= Mod_Command; continue; }
        // 非修饰 token = 主键。若出现多个主键,以最后一个为准。
        auto it = keyNameTable().find(t);
        if (it == keyNameTable().end()) {
            KMS_WARN("热键主键无法识别: '%s'", t.c_str());
            return false;
        }
        hk.keycode = it->second;
        gotKey = true;
    }
    if (!gotKey) return false;
    out = hk;
    return true;
}

// -----------------------------------------------------------------------------
// 逐行解析 key=value 配置文件。'#' 起始为注释;空行忽略。
// -----------------------------------------------------------------------------
bool Config::loadFromFile(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        KMS_WARN("未找到配置文件 %s,使用内置默认值", path.c_str());
        return false;
    }
    std::string line;
    int lineNo = 0;
    while (std::getline(in, line)) {
        ++lineNo;
        std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        auto eq = s.find('=');
        if (eq == std::string::npos) {
            KMS_WARN("配置第 %d 行缺少 '=',忽略: %s", lineNo, s.c_str());
            continue;
        }
        std::string key = lower(trim(s.substr(0, eq)));
        std::string val = trim(s.substr(eq + 1));

        if (key == "port") {
            listen_port = std::atoi(val.c_str());
        } else if (key == "edge") {
            std::string v = lower(val);
            if (v == "left") edge = Edge::Left;
            else if (v == "right") edge = Edge::Right;
            else if (v == "top") edge = Edge::Top;
            else if (v == "bottom") edge = Edge::Bottom;
            else if (v == "none") edge = Edge::None;
            else KMS_WARN("edge 取值无法识别: %s", val.c_str());
        } else if (key == "switch_mode") {
            std::string v = lower(val);
            if (v == "edge") switch_mode = SwitchMode::Edge;
            else if (v == "hotkey") switch_mode = SwitchMode::Hotkey;
            else KMS_WARN("switch_mode 取值无法识别: %s", val.c_str());
        } else if (key == "hotkey") {
            Hotkey hk;
            if (parseHotkey(val, hk)) hotkey = hk;
            else KMS_WARN("hotkey 解析失败,保留默认: %s", val.c_str());
        } else if (key == "map_command_to_control") {
            map_command_to_control = parseBool(val);
        } else if (key == "mouse_mode") {
            std::string v = lower(val);
            if (v == "relative") mouse_mode = MouseMode::Relative;
            else if (v == "absolute") mouse_mode = MouseMode::Absolute;
            else KMS_WARN("mouse_mode 取值无法识别: %s", val.c_str());
        } else if (key == "auto_return") {
            auto_return = parseBool(val);
        } else if (key == "wheel_scale") {
            wheel_scale = std::atof(val.c_str());
        } else if (key == "android_width") {
            android_width = std::atoi(val.c_str());
        } else if (key == "android_height") {
            android_height = std::atoi(val.c_str());
        } else if (key == "verbose") {
            verbose = parseBool(val);
        } else if (key == "server_name") {
            server_name = val;
        } else {
            KMS_WARN("未知配置项(忽略): %s", key.c_str());
        }
    }
    return true;
}

void Config::logSummary() const {
    const char *edgeStr = edge == Edge::Left ? "left" : edge == Edge::Right ? "right"
                          : edge == Edge::Top ? "top" : edge == Edge::Bottom ? "bottom" : "none";
    const char *modeStr = switch_mode == SwitchMode::Edge ? "edge(撞边)" : "hotkey(热键)";
    const char *mouseStr = mouse_mode == MouseMode::Relative ? "relative(相对)" : "absolute(绝对)";
    KMS_INFO("配置: 端口=%d 跨屏=%s 触发边=%s 鼠标=%s Cmd→Ctrl=%s 自动切回=%s",
             listen_port, modeStr, edgeStr, mouseStr,
             map_command_to_control ? "是" : "否",
             auto_return ? "开" : "关");
}

} // namespace kms
