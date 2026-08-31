# keyboradmouseShare

在 **Mac 上作为主控端(server)**,把本机的键盘与鼠标通过网络共享给安卓设备
(未修改的 **InputShare** 客户端 `InputShare_101227`)。参考 deskflow 的
"相对鼠标模式"跨设备共享原理,网络传输协议与 deskflow 完全一致,因此无需改动客户端。

- 单进程、无 UI、无 Qt 依赖;所有 I/O 挂在主线程 CFRunLoop 上,单线程无锁,无额外性能开销。
- 仅实现 **主控端**:采集本机键鼠并发送;不做注入端。
- 仅支持 **macOS**。
- 功能范围:**键盘 + 鼠标**(不含剪贴板)。

## 工作原理

服务端严格复刻 deskflow 服务端 → 客户端的握手与消息格式:

```
TCP 连接 → 发 Hello → 收 HelloBack(客户端名)
        → 发 QINF → 收 DINF(安卓屏幕尺寸)
        → 发 CIAK + DSOP(开启相对鼠标模式)→ 就绪
        → 周期性发 CALV 保活
```

就绪后默认停留在 **主屏**(Mac 正常使用)。当:

- 鼠标**撞到配置的触发边**(默认左边缘,`switch_mode=edge`),或
- 按下**切换热键**(默认 `Ctrl+5`)

即跨入 **次屏**(控制安卓):此时本机键鼠事件被"吞掉"并转发给安卓,系统光标被
解耦冻结并隐藏,鼠标以相对增量(`DMRM`)发送——因此可快速甩到安卓屏幕任意位置。
再次按热键返回主屏(相对模式下客户端不回报光标位置,返回只能靠热键)。

## 构建

需要 macOS + CMake(≥3.20)+ Xcode 命令行工具(`xcode-select --install`)。

```bash
cd keyboradmouseShare
cmake -S . -B build
cmake --build build
```

产物:`build/keyboradmouseShare`。

## 授权(重要)

采集全局键鼠需要 **辅助功能(Accessibility)** 权限。首次运行会弹出系统授权引导:

打开 **系统设置 → 隐私与安全性 → 辅助功能**,勾选本程序(若从终端运行,勾选你的
终端 App,如 Terminal / iTerm),然后重新运行。

## 运行

```bash
./build/keyboradmouseShare [配置文件路径]
```

不带参数时读取当前目录下的 `keyboradmouseShare.conf`;文件缺失则使用内置默认值。

在安卓 InputShare 客户端里填入 Mac 的局域网 IP 和端口(默认 `24800`)连接即可。
按 `Ctrl+C` 退出。

## 配置

所有可调项都在 `keyboradmouseShare.conf`(`key=value`,`#` 为注释),改后重启生效、无需重新编译:

| 键 | 说明 | 默认 |
|---|---|---|
| `port` | 监听端口(与客户端一致) | `24800` |
| `switch_mode` | 跨屏方式:`edge`(撞边)/ `hotkey` | `edge` |
| `edge` | 触发边:`left`/`right`/`top`/`bottom`/`none` | `left` |
| `hotkey` | 切换热键,如 `ctrl+5`、`cmd+space` | `ctrl+5` |
| `map_command_to_control` | 把 Command 当 Control 发送 | `true` |
| `mouse_mode` | `relative`(推荐)/ `absolute` | `relative` |
| `wheel_scale` | 滚轮增量缩放 | `1.0` |
| `android_width` / `android_height` | 安卓分辨率兜底(握手时会被覆盖) | `1200` / `2670` |
| `verbose` | 打印 DEBUG 日志 | `false` |
| `server_name` | 日志展示用名字 | `mac` |

## 代码结构

```
main.mm            入口:加载配置 → 检查授权 → 组装部件 → 跑 CFRunLoop
src/Config.*       配置文件解析、热键字符串解析
src/Log.h          极简日志(零依赖,DEBUG 默认关闭无开销)
src/Protocol.*     deskflow/Synergy 线上协议编解码(与客户端逐字节一致)
src/NetServer.*    单客户端 TCP 服务端(CFSocket 集成主 RunLoop,非阻塞收发)
src/KeyMap.*       macOS 虚拟键码 → deskflow KeyID / button
src/MacCapture.*   CGEventTap 采集 + 光标解耦/隐藏 + 撞边/热键检测
src/Controller.*   握手状态机 + 主/次屏切换 + 事件→协议消息分发
```

## 与客户端的协议要点

- 帧格式:`[4 字节大端长度][负载]`;长度 = 类型码 + 字段。
- `DKRP`(按键重复)只发 4 个 short,**不带** deskflow 的尾随 lang 串,以匹配客户端读取器。
- 客户端忽略 `mask`,靠单独转发的修饰键还原组合键/大小写;故 KeyID 发送"未按 Shift 的基础码点"。
- Control 的 KeyID 统一用 `61412`(客户端仅识别该值);Command 默认映射为 Control;Escape 客户端未支持会被丢弃。
