// =============================================================================
// main.mm —— 程序入口:加载配置 → 检查授权 → 组装网络/采集/编排 → 跑主 RunLoop
//
// 整个程序单进程、单线程:所有 I/O(TCP、键鼠采集、CALV 定时器、退出信号)都挂在
// 主线程的 CFRunLoop 上,彼此串行执行,因此无锁、无线程同步开销。
// =============================================================================
#include <CoreFoundation/CoreFoundation.h> // CFRunLoopRun / CFRunLoopStop / CFRunLoopGetMain
#include <dispatch/dispatch.h>

#include <csignal>
#include <string>

#include "Config.h"
#include "Controller.h"
#include "Log.h"
#include "MacCapture.h"
#include "NetServer.h"

using namespace kms;

int main(int argc, char **argv) {
    @autoreleasepool {
        // 配置文件路径:命令行第一个参数指定,否则用当前目录下的默认名。
        std::string cfgPath = (argc > 1) ? argv[1] : "keyboradmouseShare.conf";

        Config cfg;
        cfg.loadFromFile(cfgPath); // 文件不存在时用内置默认值继续
        g_verbose = cfg.verbose;   // 打开/关闭 DEBUG 日志
        cfg.logSummary();

        // 采集全局键鼠需要"辅助功能"授权;首次运行会弹出系统授权引导。
        if (!MacCapture::ensureAccessibility(true)) {
            KMS_ERROR("未获得辅助功能授权。请在 系统设置 → 隐私与安全性 → 辅助功能 "
                      "中勾选本程序(或其所在终端),然后重新运行。");
            return 1;
        }

        // 组装三大部件并互相接线(Controller 同时是网络与采集的回调对象)。
        NetServer net(cfg.listen_port);
        MacCapture cap(cfg);
        Controller ctrl(cfg, net, cap);
        net.setDelegate(&ctrl);
        cap.setDelegate(&ctrl);

        if (!net.start()) return 1;
        if (!cap.start()) { net.stop(); return 1; }
        ctrl.start(); // 启动 CALV 保活定时器

        // 优雅退出:用 GCD 信号源在主队列里处理 SIGINT/SIGTERM,停掉主 RunLoop。
        // (信号源比在信号处理函数里直接调用 CF 更安全。)
        signal(SIGINT, SIG_IGN);
        signal(SIGTERM, SIG_IGN);
        dispatch_source_t sigint = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(sigint, ^{ CFRunLoopStop(CFRunLoopGetMain()); });
        dispatch_resume(sigint);
        dispatch_source_t sigterm = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(sigterm, ^{ CFRunLoopStop(CFRunLoopGetMain()); });
        dispatch_resume(sigterm);

        KMS_INFO("keyboradmouseShare 运行中。按 Ctrl+C 退出。");
        CFRunLoopRun(); // 阻塞在这里,直到收到退出信号

        KMS_INFO("正在退出,清理资源…");
        ctrl.stop();
        cap.stop();
        net.stop();
    }
    return 0;
}
