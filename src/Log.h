// =============================================================================
// Log.h —— 极简日志工具
//
// 设计目标:零依赖、线程无关(本程序单线程)、开销极低。提供 4 个等级的宏,
// 统一输出到 stderr,并带上时间戳与等级前缀,方便真机联调时定位问题。
//
// 为什么不用第三方日志库:本项目要求"单进程、结构清晰、无性能问题",一个
// 头文件足矣,避免引入额外依赖与构建复杂度。
// =============================================================================
#pragma once

#include <cstdio>
#include <ctime>

namespace kms {

// 运行期日志开关:INFO 及以上始终打印;DEBUG 仅在 g_verbose=true 时打印。
// 由 main() 根据配置/命令行设置。热路径(鼠标移动)里只用 KMS_DEBUG,默认关闭,
// 因此正常运行时不会因日志产生任何可感知开销。
inline bool g_verbose = false;

// 打印带时间戳的一行日志。level 为等级字符串。
inline void log_line(const char *level, const char *msg) {
    // 用本地时间输出 HH:MM:SS,足够联调使用,且不引入 <chrono> 格式化开销。
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&t, &tmv);
    std::fprintf(stderr, "[%02d:%02d:%02d][%s] %s\n",
                 tmv.tm_hour, tmv.tm_min, tmv.tm_sec, level, msg);
}

} // namespace kms

// 变参格式化宏:先格式化到栈上定长缓冲,再交给 log_line。
// 256 字节对日志足够;超长会被安全截断(snprintf 保证不越界)。
#define KMS_LOGF(level, fmt, ...)                                              \
    do {                                                                       \
        char _kms_buf[256];                                                    \
        std::snprintf(_kms_buf, sizeof(_kms_buf), fmt, ##__VA_ARGS__);         \
        ::kms::log_line(level, _kms_buf);                                      \
    } while (0)

#define KMS_INFO(fmt, ...)  KMS_LOGF("INFO",  fmt, ##__VA_ARGS__)
#define KMS_WARN(fmt, ...)  KMS_LOGF("WARN",  fmt, ##__VA_ARGS__)
#define KMS_ERROR(fmt, ...) KMS_LOGF("ERROR", fmt, ##__VA_ARGS__)
// DEBUG 宏在关闭时几乎零成本(仅一次 bool 判断)。
#define KMS_DEBUG(fmt, ...)                                                    \
    do {                                                                       \
        if (::kms::g_verbose) KMS_LOGF("DBG", fmt, ##__VA_ARGS__);             \
    } while (0)
