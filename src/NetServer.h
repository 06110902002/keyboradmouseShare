// =============================================================================
// NetServer.h —— 单客户端 TCP 服务端(集成到主线程 CFRunLoop,无锁单线程)
//
// 职责:
//   * 在 0.0.0.0:port 监听,接受一个客户端连接(安卓 InputShare)。
//   * 用 CFSocket 把"监听 socket 的 accept"和"客户端 socket 的可读/可写"事件
//     挂到主线程的 CFRunLoop 上,与 CGEventTap、CALV 定时器共用一个线程。
//     因此全程单线程、无锁、无线程同步开销——满足"无性能问题"。
//   * 收到的字节交给 FrameReader 切帧,每切出一条完整帧就回调 Delegate。
//   * 发送优先直接 write();遇 EAGAIN / 部分写入时把剩余字节入队,并按需打开
//     写回调补发,避免阻塞主线程(socket 全程非阻塞)。
//
// 为什么用 CFSocket 而不是自建 select/线程:主逻辑(采集/注入编排)本来就跑在
// 主 CFRunLoop 上,把网络 I/O 也挂上去可彻底避免跨线程共享状态,代码更简单也更快。
// =============================================================================
#pragma once

#include <CoreFoundation/CoreFoundation.h>

#include <cstdint>
#include <vector>

#include "Protocol.h"

namespace kms {

// 网络事件回调。由 Controller 实现。全部在主线程回调,实现里无需加锁。
class NetServerDelegate {
public:
    virtual ~NetServerDelegate() = default;
    // 有客户端完成 TCP 连接(尚未完成应用层握手)。
    virtual void onClientConnected() = 0;
    // 切出一条完整帧的负载(类型码 + 字段)。生命周期仅在本次回调内有效。
    virtual void onFrame(const std::vector<std::uint8_t> &payload) = 0;
    // 客户端断开(对端关闭 / 出错 / 帧错位)。
    virtual void onClientDisconnected() = 0;
};

class NetServer {
public:
    explicit NetServer(int port) : port_(port) {}
    ~NetServer();

    NetServer(const NetServer &) = delete;
    NetServer &operator=(const NetServer &) = delete;

    void setDelegate(NetServerDelegate *d) { delegate_ = d; }

    // 创建监听 socket、绑定、监听,并挂到"当前"CFRunLoop。成功返回 true。
    // 必须在将要跑 CFRunLoopRun 的那个线程(主线程)上调用。
    bool start();

    // 停止:断开客户端、关闭监听、从 RunLoop 摘除。析构时也会调用。
    void stop();

    // 是否已有已连接客户端。
    bool hasClient() const { return clientFd_ >= 0; }

    // 发送一整帧(或任意字节)。无客户端时静默丢弃。非阻塞:必要时入队补发。
    void send(const std::vector<std::uint8_t> &bytes);
    void send(const std::uint8_t *data, std::size_t n);

    // 主动断开当前客户端(例如发送 CBYE 之后)。
    void disconnectClient();

private:
    // ---- CFSocket C 回调 → 成员方法的转发 ----
    static void listenCallback(CFSocketRef s, CFSocketCallBackType type,
                               CFDataRef address, const void *data, void *info);
    static void clientCallback(CFSocketRef s, CFSocketCallBackType type,
                               CFDataRef address, const void *data, void *info);

    // 接受一个新连接(native fd 已由 CFSocket accept 出来)。
    void handleAccept(int fd);
    // 客户端 socket 可读:尽量读空,切帧回调。
    void handleReadable();
    // 客户端 socket 可写:补发未发完的出站缓冲。
    void handleWritable();

    // 尝试把出站缓冲尽量写出;仍有剩余则打开写回调,否则关闭写回调。
    void flushOutbound();
    // 关闭并清理客户端相关资源(不含 delegate 通知)。
    void teardownClient();

    int port_ = 24800;
    NetServerDelegate *delegate_ = nullptr;

    // 监听 socket 及其 RunLoop 源。
    int listenFd_ = -1;
    CFSocketRef listenSock_ = nullptr;
    CFRunLoopSourceRef listenSrc_ = nullptr;

    // 客户端 socket 及其 RunLoop 源。
    int clientFd_ = -1;
    CFSocketRef clientSock_ = nullptr;
    CFRunLoopSourceRef clientSrc_ = nullptr;
    bool writeCbEnabled_ = false; // 写回调当前是否已开启

    proto::FrameReader reader_;              // 入站切帧器
    std::vector<std::uint8_t> outbound_;     // 出站待发缓冲(FIFO,从头部消费)
    std::size_t outboundSent_ = 0;           // outbound_ 中已发送的前缀长度
};

} // namespace kms
