# 代理流量监控 · SplitMeter

> 送给因代理流量焦虑的朋友。

一款不足 1 MB 的 Windows 悬浮窗，将代理与直连流量分开显示。
不抓包、不加驱动、不改任何客户端设置。

![折叠模式](docs/preview-collapsed.png)

左键点一下变高，看上下行拆分和本次开机的累计：

![展开模式](docs/preview-expanded.png)

---

## 它解决了什么

开着梯子的时候，你是否在焦虑？

- 眼前这点网速，是走了节点，还是漏成了直连？
- 这个月烧掉的那几十 GB，有多少真的过了代理？
- 关掉代理之后，是不是还有程序在偷偷吃流量？

流量监控软件只给你一个总数，代理客户端只给你节点延迟。两边都不告诉你**哪些字节走了哪条路**。

SplitMeter 只干这一件事。

## 两种形态

**折叠模式**（默认）—— 242 × 26，只有一行，像任务栏边上的一个小胶囊。挂着不占地方，一眼能看到代理和直连各自跑了多少。

**展开模式** —— 点一下变高到 242 × 62，多出上下行各自的速度，以及本次开机以来这一类流量总共用了多少。

切换有 180ms 的缓动，不是硬跳。两种形态宽度一致，不会左右晃。

## 特性

- **分开计量**：代理 / 直连各自的实时速率与本次开机累计，互不混淆
- **免配置**：直接读 Clash Verge 的命名管道。不需要打开 TCP 控制端口，不用填 secret，改完配置不用重启
- **极轻**：单文件 876 KB，静态链接，无运行库依赖，绿色免安装
- **折叠模式**：默认只占一行，需要细节时点开，不需要时它不吵你
- **开机累计**：按开机时刻裁剪，跨重启保留，内核重启不会把数字清零
- **iOS 观感**：深色面板、系统色分层（代理蓝 / 直连灰 / 总量浅灰）、hairline 分隔

## 用法

下载 `SplitMeter.exe` 双击即可，没有安装步骤。

- **拖动**：左键按住移动，位置会记住
- **展开 / 收起**：左键点一下
- **右键**：开机自启 / 置顶 / 清零统计 / 退出
- 累计数据存在 `%APPDATA%\SplitMeter\state.ini`

## 它怎么做到的

不抓包，不加驱动，改读代理内核自己的账本。

mihomo 系内核（Clash Verge Rev 用的就是它）有一条本地控制接口，`/connections` 会返回每一条活跃连接的：

- `upload` / `download`：该连接至今的累计字节
- `chains`：这条连接的代理链路 —— 走直连时是 `["DIRECT"]`，走节点时是 `["节点名", "策略组"]`

拿两个时刻的累计值相减就是速率，按 `chains` 归类就是分流。内核同时给出 `downloadTotal` / `uploadTotal` 作为总量校验。

**关键点：Clash Verge Rev 默认关闭了 TCP 控制端口（`external-controller: ''`），只开命名管道。**
所以本程序直接枚举 `\\.\pipe\`，找 `verge-mihomo-*` 这一类的管道名，全程走命名管道通信。

### 三个绕不开的坑

**一、不要用网卡计数器。**
`GetIfTable2` 的 `InOctets` 看着最权威，实测在本项目开发机上，一块 Wi-Fi 网卡的 `OutOctets` 是坏的 —— 上传 3 MB，它只涨了 927 字节。
退一步说，就算它正常，网卡层也只给你总数，天生没有「代理 / 直连」这个维度。真正的数据只有内核知道。

**二、同一张网卡在系统里会出现 8 个副本。**
`WLAN`、`WLAN-QoS`、`WLAN-WFP` … 它们的 MAC 和计数完全一样。谁要是「把所有网卡加起来」，流量直接翻 6 倍。

**三、不能长连接订阅 `/traffic`。**
那条流会把内核的管道服务占满，后续所有请求都会失败。累计量从 `/connections` 的 `uploadTotal` / `downloadTotal` 取就行，一次请求同时拿到总量和明细。

## 支持范围

| 客户端 | 状态 |
|---|---|
| Clash Verge Rev（默认配置） | ✅ 免配置，自动发现管道 |
| Clash Verge Rev（开启 TCP 控制端口） | ✅ 可用 |
| mihomo 裸内核 | ⚠️ 需自行确认管道名 |
| v2rayN / sing-box | ❌ 未适配 |

## 已知限制

- **只认 mihomo 系内核**。v2rayN、sing-box 的原生 API 结构不同，需要单独适配
- **不统计绕过内核的流量**。如果某个程序直接连网、不经代理，它既不在代理里也不在直连里
- **首次运行的累计是估算值**。内核只保留「自内核启动以来」的总量，没有历史分流记录。第一次运行时会按当前活跃连接的比例把历史总量拆开，之后才是精确累计
- **宽度固定 242 px**。总量字符串长度是变化的，所以数值偏短时右侧会留一点空白

## 关于签名

这个 exe **没有代码签名** —— 作者没钱买证书，大伙凑活着用。

所以第一次运行时 Windows SmartScreen 会拦一下，点「更多信息」→「仍要运行」就行。
要是心里还是发虚，那就别信我，自己看源码编译，`build.bat` 一句话的事。

## 从源码构建

需要 MinGW-w64（g++ 与 windres），没有其他依赖。

```bash
build.bat
```

或手动：

```bash
windres -i res/app.rc -o build/app_res.o
g++ -std=c++17 -O2 -s -municode -mwindows -static \
    -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -fno-exceptions -fno-rtti \
    src/core.cpp src/main.cpp build/app_res.o \
    -o build/SplitMeter.exe \
    -lgdiplus -lshell32 -luser32 -lgdi32 -ladvapi32 -lole32 -luuid
```

### 调试

设置环境变量 `SPLITMETER_DUMP=1`，程序每帧会把渲染缓冲导出成 `%TEMP%\splitmeter_frame.bmp`。
分层窗口截屏不可靠，直接看缓冲才是真相。`tools/bmp2png.py` 可以把它转成 PNG：

```bash
python tools/bmp2png.py %TEMP%\splitmeter_frame.bmp out.png 62 3
```

`tools/measure.cpp` 是个独立的小工具，用来测量字符串在真实字体下的墨迹宽度 —— 布局里的每个数字都是它量出来的。

## 致谢

**Vibe Coding** —— 这个项目是人机协作的产物：人负责提需求、判断效果、挑毛病，AI 负责把它写成代码。
致谢 [DeepSeek](https://www.deepseek.com/) 与梁圣（梁文锋）。

**技术** —— 分流的思路来自 mihomo 的 `/connections` 接口，以及 [Clash Verge Rev](https://github.com/clash-verge-rev/clash-verge-rev) 把管道名暴露得足够清楚。

## License

[MIT](LICENSE)
