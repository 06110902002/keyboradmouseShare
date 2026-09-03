# keyboradmouseShare

在 **Mac 上作为主控端(server)**,把本机的键盘与鼠标通过网络共享给安卓设备
(**InputShare** 客户端 `InputShare_101227`)。网络传输协议与 deskflow/Synergy
**逐字节一致**,因此任何标准 Synergy 客户端都能连接。

默认走 **绝对鼠标模式**(`mouse_mode=absolute`):服务端直接下发目标绝对坐标,
安卓端用"数位板式绝对定位"注入,**光标落点就是服务端发出的坐标** —— 按撞边高度
比例进入精确、移动无漂移、屏幕任意像素可达。该模式需要客户端支持绝对定位注入
(本仓库配套的 `InputShare_101227` 已实现);连接其它未改造的客户端时改用
`mouse_mode=relative` 即可(见下方「双端回退」)。

- 单进程、无 UI、无 Qt 依赖;所有 I/O 挂在主线程 CFRunLoop 上,单线程无锁,无额外性能开销。
- 仅实现 **主控端**:采集本机键鼠并发送;不做注入端。
- 仅支持 **macOS**。
- 功能范围:**键盘 + 鼠标**(不含剪贴板)。

## 工作原理

服务端严格复刻 deskflow 服务端 → 客户端的握手与消息格式:

```
TCP 连接 → 发 Hello → 收 HelloBack(客户端名)
        → 发 QINF → 收 DINF(安卓屏幕尺寸)
        → 发 CIAK + DSOP(鼠标模式选项)→ 就绪
        → 周期性发 CALV 保活
```

> `DSOP` 是模式开关:`mouse_mode=absolute` 时**不带任何选项**(客户端保持默认的
> 绝对语义);`mouse_mode=relative` 时带 `kOptionRelativeMouseMoves=1`。

就绪后默认停留在 **主屏**(Mac 正常使用)。当:

- 鼠标**撞到配置的触发边**(默认左边缘,`switch_mode=edge`),或
- 按下**切换热键**(默认 `Ctrl+5`)

即跨入 **次屏**(控制安卓):此时本机键鼠事件被"吞掉"并转发给安卓,系统光标被
解耦冻结并隐藏。

**跨入位置按比例对齐**:光标撞 Mac 左边缘时的竖直比例(撞在 Mac 高度的 X% 处),
会被换算成安卓右边缘同样 X% 的位置,光标"从这一侧滑进来"。绝对模式下服务端把
换算结果直接放进 `CINN`,客户端一次落位、零误差;相对模式下客户端只能相对注入,
落点会被系统不可知的指针加速增益放大,比例会有偏差(这正是默认用绝对模式的原因)。

进入次屏后:

- 绝对模式:每次移动发 `DMMV`(服务端维护的虚拟光标坐标,已夹在屏内);
- 相对模式:每次移动发 `DMRM`(原始增量),可快速甩到安卓屏幕任意位置。

**返回主屏**有两种方式:

- 再次按**切换热键**;或
- **撞返回边自动切回**(`auto_return=true`,模仿 deskflow 绝对模式):服务端在次屏期间维护
  一份"虚拟光标",当它一路移到与 Mac 相邻的**返回边**(即进入边的对边,默认进入边为左、
  故返回边为安卓右边缘)并越出时,自动切回 Mac——就像两块屏幕左右相邻、光标滑回来一样。
  客户端不回报光标位置,该判定始终由服务端按累积增量估算;`edge=none` 时此功能失效
  (无几何布局),仅靠热键返回。

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
| `mouse_mode` | `absolute`(默认,精确落位)/ `relative`(兼容未改造客户端) | `absolute` |
| `auto_return` | 撞返回边(进入边对边)自动切回主屏 | `true` |
| `wheel_scale` | 滚轮增量缩放 | `1.0` |
| `android_width` / `android_height` | 安卓分辨率兜底(握手时会被覆盖) | `1200` / `2670` |
| `verbose` | 打印 DEBUG 日志 | `false` |
| `server_name` | 日志展示用名字 | `mac` |

## 双端回退(绝对 → 相对)

绝对模式是**双端成对**的能力,回退也要成对改,两处缺一会退化:

1. 服务端 `keyboradmouseShare.conf`:`mouse_mode = relative`(改完重启,无需重编)。
2. 客户端 `Injection.java`:`ABSOLUTE_POINTER_ENABLED = false`(改完重打 APK)。

改完两处即完全等同于历史的纯相对注入行为。只改一处的后果:

- 只改服务端(客户端仍开着绝对设备):客户端把 `DMRM` 增量自己累加成绝对坐标后落位,
  仍然精确,只是"快甩"手感变成线性(无系统加速曲线)。可用。
- 只改客户端(服务端仍是绝对模式):客户端收 `DMMV` 却只能相对注入,每次移动都要
  "先归零再走",落点被不可知增益放大,可能被钳到屏幕角落。**不要这样配**。

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
