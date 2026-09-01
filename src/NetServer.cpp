// =============================================================================
// NetServer.cpp —— TCP 服务端实现(CFSocket + 主 CFRunLoop,单线程无锁)
// =============================================================================
#include "NetServer.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "Log.h"

namespace kms {

namespace {
// 把 fd 设为非阻塞。返回是否成功。
bool setNonBlocking(int fd) {
    int fl = ::fcntl(fd, F_GETFL, 0);
    if (fl < 0) return false;
    return ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}
// 设置一个 int 型 socket 选项(值恒为 1),忽略失败(非致命)。
void setSockOpt1(int fd, int level, int opt) {
    int one = 1;
    ::setsockopt(fd, level, opt, &one, sizeof(one));
}
// 获取本机对外 IPv4 地址(第一个非回环地址)。找不到时返回空串。
std::string localIPv4() {
    std::string ip;
    ifaddrs *ifap = nullptr;
    if (::getifaddrs(&ifap) != 0) return ip;
    for (ifaddrs *ifa = ifap; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) continue;
        auto *sin = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
        char buf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
        if (std::string(buf).rfind("127.", 0) == 0) continue; // 跳过回环地址
        ip = buf;
        break;
    }
    ::freeifaddrs(ifap);
    return ip;
}
} // namespace

NetServer::~NetServer() { stop(); }

// -----------------------------------------------------------------------------
// 启动监听:创建 socket → 复用地址 → 绑定 0.0.0.0:port → listen → 挂 CFRunLoop。
// -----------------------------------------------------------------------------
bool NetServer::start() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        KMS_ERROR("创建监听 socket 失败: %s", std::strerror(errno));
        return false;
    }
    // SO_REUSEADDR:重启进程时避免 TIME_WAIT 占用端口导致 bind 失败。
    setSockOpt1(listenFd_, SOL_SOCKET, SO_REUSEADDR);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY); // 监听所有网卡,便于手机经 Wi‑Fi 连入
    addr.sin_port = htons(static_cast<uint16_t>(port_));
    if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        KMS_ERROR("绑定端口 %d 失败: %s", port_, std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    if (::listen(listenFd_, 1) != 0) { // 只服务单客户端,backlog=1 足够
        KMS_ERROR("listen 失败: %s", std::strerror(errno));
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }

    // 用 CFSocket 包裹监听 fd,注册 accept 回调。info 指向本对象用于回调转发。
    CFSocketContext ctx{};
    ctx.info = this;
    listenSock_ = CFSocketCreateWithNative(kCFAllocatorDefault, listenFd_,
                                           kCFSocketAcceptCallBack,
                                           &NetServer::listenCallback, &ctx);
    if (!listenSock_) {
        KMS_ERROR("CFSocketCreateWithNative(监听) 失败");
        ::close(listenFd_);
        listenFd_ = -1;
        return false;
    }
    // 关闭 CloseOnInvalidate:fd 生命周期由本类统一管理,避免重复 close。
    CFOptionFlags fl = CFSocketGetSocketFlags(listenSock_);
    fl &= ~kCFSocketCloseOnInvalidate;
    CFSocketSetSocketFlags(listenSock_, fl);

    listenSrc_ = CFSocketCreateRunLoopSource(kCFAllocatorDefault, listenSock_, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), listenSrc_, kCFRunLoopCommonModes);

    const std::string localIP = localIPv4();
    KMS_INFO("正在监听 %s:%d,等待客户端连接…",
             localIP.empty() ? "0.0.0.0" : localIP.c_str(), port_);
    return true;
}

// -----------------------------------------------------------------------------
// 停止:先清客户端,再摘除监听源、关闭监听 socket。可重复调用。
// -----------------------------------------------------------------------------
void NetServer::stop() {
    teardownClient(); // 静默清理,不回调 delegate(通常是进程退出场景)

    if (listenSrc_) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), listenSrc_, kCFRunLoopCommonModes);
        CFRelease(listenSrc_);
        listenSrc_ = nullptr;
    }
    if (listenSock_) {
        CFSocketInvalidate(listenSock_);
        CFRelease(listenSock_);
        listenSock_ = nullptr;
    }
    if (listenFd_ >= 0) {
        ::close(listenFd_);
        listenFd_ = -1;
    }
}

// ---- C 回调 → 成员方法 ----
void NetServer::listenCallback(CFSocketRef, CFSocketCallBackType type,
                               CFDataRef, const void *data, void *info) {
    if (type != kCFSocketAcceptCallBack || !data) return;
    // accept 回调的 data 指向新连接的 native fd。
    int fd = *static_cast<const CFSocketNativeHandle *>(data);
    static_cast<NetServer *>(info)->handleAccept(fd);
}

void NetServer::clientCallback(CFSocketRef, CFSocketCallBackType type,
                               CFDataRef, const void *, void *info) {
    auto *self = static_cast<NetServer *>(info);
    if (type == kCFSocketReadCallBack) self->handleReadable();
    else if (type == kCFSocketWriteCallBack) self->handleWritable();
}

// -----------------------------------------------------------------------------
// 接受新连接。本服务只保留一个客户端:若已有连接则先断开旧的再接纳新的,
// 这样手机断线重连时无需重启服务端。
// -----------------------------------------------------------------------------
void NetServer::handleAccept(int fd) {
    if (fd < 0) return;
    if (clientFd_ >= 0) {
        KMS_WARN("已有客户端连接,断开旧连接以接纳新连接");
        disconnectClient(); // 通知 delegate 复位握手状态
    }

    // 客户端 socket:非阻塞 + 关闭 Nagle(低延迟) + 屏蔽 SIGPIPE。
    setNonBlocking(fd);
    setSockOpt1(fd, IPPROTO_TCP, TCP_NODELAY);
    setSockOpt1(fd, SOL_SOCKET, SO_NOSIGPIPE); // 对端关闭后 write 返回 EPIPE 而非发信号

    CFSocketContext ctx{};
    ctx.info = this;
    // 同时注册读/写回调;写回调随后立即禁用,仅在有积压待发时按需开启。
    clientSock_ = CFSocketCreateWithNative(
        kCFAllocatorDefault, fd, kCFSocketReadCallBack | kCFSocketWriteCallBack,
        &NetServer::clientCallback, &ctx);
    if (!clientSock_) {
        KMS_ERROR("CFSocketCreateWithNative(客户端) 失败");
        ::close(fd);
        return;
    }
    clientFd_ = fd;

    // 调整标志位:读回调自动重新武装;写回调手动控制;fd 由本类关闭。
    CFOptionFlags fl = CFSocketGetSocketFlags(clientSock_);
    fl |= kCFSocketAutomaticallyReenableReadCallBack;
    fl &= ~kCFSocketAutomaticallyReenableWriteCallBack;
    fl &= ~kCFSocketCloseOnInvalidate;
    CFSocketSetSocketFlags(clientSock_, fl);
    CFSocketDisableCallBacks(clientSock_, kCFSocketWriteCallBack);
    writeCbEnabled_ = false;

    clientSrc_ = CFSocketCreateRunLoopSource(kCFAllocatorDefault, clientSock_, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), clientSrc_, kCFRunLoopCommonModes);

    reader_.reset();
    outbound_.clear();
    outboundSent_ = 0;

    // 打印对端地址便于确认。
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    if (::getpeername(fd, reinterpret_cast<sockaddr *>(&peer), &plen) == 0) {
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        KMS_INFO("客户端已连接: %s:%d", ip, ntohs(peer.sin_port));
    } else {
        KMS_INFO("客户端已连接");
    }

    if (delegate_) delegate_->onClientConnected();
}

// -----------------------------------------------------------------------------
// 可读:循环 recv 直到 EAGAIN,喂给切帧器,逐帧回调 Controller。
// -----------------------------------------------------------------------------
void NetServer::handleReadable() {
    std::uint8_t buf[4096];
    for (;;) {
        ssize_t n = ::recv(clientFd_, buf, sizeof(buf), 0);
        if (n > 0) {
            reader_.feed(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) { // 对端正常关闭
            KMS_INFO("客户端关闭了连接");
            disconnectClient();
            return;
        }
        // n < 0
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break; // 读空,正常
        KMS_WARN("recv 出错: %s", std::strerror(errno));
        disconnectClient();
        return;
    }

    // 切出所有完整帧并回调。回调中 Controller 可能触发断开,故每轮校验连接仍在。
    std::vector<std::uint8_t> payload;
    bool err = false;
    while (clientFd_ >= 0 && reader_.pop(payload, err)) {
        if (delegate_) delegate_->onFrame(payload);
    }
    if (err) {
        KMS_WARN("帧长异常(疑似流错位),断开连接");
        disconnectClient();
    }
}

// 可写:CF 在触发后已自动禁用本回调,flushOutbound 会按需重新武装。
void NetServer::handleWritable() { flushOutbound(); }

// -----------------------------------------------------------------------------
// 发送:追加到出站缓冲后尝试立即写出。无客户端则丢弃。
// -----------------------------------------------------------------------------
void NetServer::send(const std::vector<std::uint8_t> &bytes) {
    send(bytes.data(), bytes.size());
}

void NetServer::send(const std::uint8_t *data, std::size_t n) {
    if (clientFd_ < 0 || n == 0) return;
    outbound_.insert(outbound_.end(), data, data + n);
    flushOutbound();
}

// -----------------------------------------------------------------------------
// 尽量把出站缓冲写出。遇 EAGAIN/部分写入则开启写回调,待可写时补发。
// 用 outboundSent_ 记录已发前缀,发完再整体清空,避免频繁 erase 头部。
// -----------------------------------------------------------------------------
void NetServer::flushOutbound() {
    if (clientFd_ < 0) return;
    while (outboundSent_ < outbound_.size()) {
        ssize_t w = ::write(clientFd_, outbound_.data() + outboundSent_,
                            outbound_.size() - outboundSent_);
        if (w > 0) {
            outboundSent_ += static_cast<std::size_t>(w);
        } else if (w < 0 && errno == EINTR) {
            continue;
        } else if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break; // 内核发送缓冲满,等下次可写
        } else {
            KMS_WARN("write 出错: %s", std::strerror(errno));
            disconnectClient();
            return;
        }
    }

    if (outboundSent_ >= outbound_.size()) {
        // 全部发完:清空缓冲并关闭写回调。
        outbound_.clear();
        outboundSent_ = 0;
        if (writeCbEnabled_) {
            CFSocketDisableCallBacks(clientSock_, kCFSocketWriteCallBack);
            writeCbEnabled_ = false;
        }
    } else {
        // 仍有积压:武装写回调(CF 触发一次后会自动禁用,故每次都需重新 enable)。
        CFSocketEnableCallBacks(clientSock_, kCFSocketWriteCallBack);
        writeCbEnabled_ = true;
    }
}

// 主动断开:清理资源并通知 delegate(与 stop 的静默清理区分开)。
void NetServer::disconnectClient() {
    if (clientFd_ < 0) return;
    teardownClient();
    if (delegate_) delegate_->onClientDisconnected();
}

// 释放客户端相关的 RunLoop 源、CFSocket、fd 与缓冲(不含 delegate 通知)。
void NetServer::teardownClient() {
    if (clientSrc_) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), clientSrc_, kCFRunLoopCommonModes);
        CFRelease(clientSrc_);
        clientSrc_ = nullptr;
    }
    if (clientSock_) {
        CFSocketInvalidate(clientSock_); // 停止回调;因已关 CloseOnInvalidate 不会关 fd
        CFRelease(clientSock_);
        clientSock_ = nullptr;
    }
    if (clientFd_ >= 0) {
        ::close(clientFd_);
        clientFd_ = -1;
    }
    reader_.reset();
    outbound_.clear();
    outboundSent_ = 0;
    writeCbEnabled_ = false;
}

} // namespace kms
