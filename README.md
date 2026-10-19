<p align="center"><img src="assets/htune-icon-source.png" width="128" alt="HTune 图标"></p>

# HTune

**Windows 11 原生音乐视觉伴侣。** 播放器负责放歌，HTune 把歌词、封面和声音变成桌面上的音乐舞台。

采用 C++20、Win32、Direct3D 11、Direct2D、DirectWrite、Media Foundation、WASAPI 和 WinHTTP。运行时不需要 Electron、WebView 或 Node.js。

当前源码版本：**0.7.14**。项目持续开发中，播放器兼容性和同步表现受客户端版本、数据来源及系统环境影响。

## 功能

- 无边框窗口、全屏和融入桌面三种显示形态。
- 逐字歌词与逐句歌词、翻译显示、本地歌词导入和时间偏移调整。
- 歌曲封面、本地图片和视频背景，粒子、光影与音频响应效果。
- 内置舞台风格、本地主题及动效强度调节。
- 网易云音乐、QQ 音乐、酷狗音乐与汽水音乐的歌曲信息和各自歌词来源接入。
- 进程音频采集和本地实时分析；软件本身不提供音乐下载或播放服务。

## 下载

在 [GitHub Releases](https://github.com/ht070309zzz/HTune/releases) 下载 Windows 11 x64 版本：

- `HTune-Setup-<版本>-x64.exe`：安装版，安装到当前用户目录，可创建桌面快捷方式。
- `HTune-<版本>-windows-x64.zip`：解压即用版，打开其中的 `HTune.exe`。
- `SHA256SUMS.txt`：上述两个文件的 SHA-256 校验值。

两种版本共用原有用户数据目录。更新或卸载前请从托盘正常退出 HTune；卸载器保留配置、主题和缓存。安装包暂未进行代码签名，安装与卸载流程尚未实机验收。

## 播放器接入范围

| 播放器 | 接入方式与当前限制 |
| --- | --- |
| 网易云音乐 | 已适配的 3.1.41.205529 二进制可通过原生只读链路取得当前歌曲身份和进度，不以 SMTC 或窗口标题为必要条件；其他二进制不沿用未经核验的偏移。 |
| QQ 音乐 | 使用 Windows 系统媒体会话获取歌曲及时间轴；22.61 的已识别二进制还可读取当前歌曲 ID，并与媒体信息交叉核验。原生解析 QRC / LRC。 |
| 酷狗音乐 | 使用系统媒体元数据；20.1.51.27967 的已识别二进制提供原生进度。歌词按严格搜索候选匹配，原生解析 KRC / LRC。 |
| 汽水音乐 | 已在 3.8.0 上观察接入；使用系统媒体会话及本地目录候选匹配，原生解析其歌词数据。目录候选不等于播放器直接提供的当前歌曲 ID。 |

SMTC 是 Windows 的系统媒体传输控制接口。QQ、酷狗和汽水当前仍依赖其提供部分数据。播放器升级后，原生读取可能暂停或降级；精确指纹、身份、时效与切歌检查会拒绝不可靠的数据。上表是适配范围，**不代表所有后台、暂停、拖动、托盘和长期同步场景均已验证**。

## 从源码构建

需要 Windows 11 x64，以及 Visual Studio 2022 或更新版本中的：

- “使用 C++ 的桌面开发”工作负载与 MSVC x64 C++20 工具链。
- Windows 11 SDK（含 `fxc.exe`）。
- “用于 Windows 的 C++ CMake 工具”（CMake 3.24+ 和 Ninja）。

在源码根目录打开 PowerShell：

```powershell
./scripts/build.ps1 -Configuration Release -Jobs 2
```

脚本通过 Visual Studio Installer 的 `vswhere` 定位工具链。着色器离线编译并嵌入程序，第三方头文件及解压源码已随仓库提供，无需 npm 或额外的播放器辅助服务。

构建成功后程序位于 `build/bin/0.7.14/HTune.exe`。Debug 构建使用 `-Configuration Debug`，输出在 `build/bin/debug/0.7.14/HTune.exe`。构建日志及输入摘要保存在 `build/provenance/`，不提交到仓库。

构建脚本只编译，不启动应用、不安装软件，也不执行运行时测试。

## 制作发行包

完成 Release 构建后，使用 [Inno Setup 6.3 或更新版本](https://jrsoftware.org/isinfo.php) 制作安装包和解压即用版：

```powershell
./scripts/package-release.ps1
# 非默认位置可传入 -IsccPath 'D:\Tools\Inno Setup 6\ISCC.exe'
```

输出位于 `dist/releases/<版本>/<本次标识>/assets/`，仅上传该目录下的安装包、ZIP 与校验文件。脚本核对成功构建的源码摘要、x64 程序版本、文件哈希及 ZIP 内容，采用明确的发行文件清单，不收录本机日志、构建收据或用户数据；不会启动安装包或应用。

## 使用

1. 启动受支持的播放器并播放音乐。
2. 打开 `HTune.exe`，应用会自动跟随可用的播放源；同时运行多个播放器时，可先暂停其他播放器。
3. 使用底部控件切换舞台、歌词布局、全屏与桌面模式。
4. 歌词版本不匹配时使用歌词修正、本地导入或时间偏移设置。

HTune 以前名为 MusicStage。为延续旧版配置，内部窗口身份与用户数据目录仍保留原标识：`%LOCALAPPDATA%\MusicStageNativePrototype`。配置、主题、缓存和本地诊断日志保存在该目录；自选图片、视频和导入文件仍由用户保管。

歌词和封面查询会向相应音乐平台发送歌曲 ID 或匹配信息。音频用于本机实时分析，不上传音频。兼容性读取采用只读方式，不向播放器注入代码。

## 已知限制

- 播放器接口或内部结构更新可能导致识别、进度或歌词获取失效；不保证未列出的客户端版本。
- 严格匹配可能找不到候选，网络失败或平台无歌词也可能导致无歌词显示。
- 已观察到歌词缓存写入失败，尚未完整解决；离线复用不能作为稳定能力承诺。
- 桌面融合依赖 Windows Explorer 的窗口结构，不同系统版本、多屏和 Explorer 重启后的行为需要分别核对。
- 未完成全场景回归和长期同步误差测量。编译成功不等于实际功能已验证。

## 源码结构

| 目录 | 内容 |
| --- | --- |
| `src/`、`include/` | 原生窗口、播放器适配、歌词、音频分析、渲染、设置与缓存 |
| `src/shaders/` | Direct3D 舞台着色器 |
| `include/third_party/` | 已注明来源和许可证的第三方组件 |
| `assets/` | HTune 图标源文件与 Windows ICO |
| `scripts/` | 本地编译及构建记录辅助脚本 |

欢迎提交 Issue 和 Pull Request。报告问题时请注明 HTune、Windows 和播放器版本、显示模式及复现步骤；分享日志或截图前请移除个人信息。提交代码请同时说明编译情况和实际验证范围。

## 许可证与致谢

HTune 使用 [MIT 许可证](LICENSE)。第三方组件沿用各自的版权与许可声明，详见 [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md)。音乐、歌词与在线封面等第三方内容不属于本仓库的授权范围。

视觉与交互参考 [Folia](https://github.com/chthollyphile/folia-major) 和 [Mineradio](https://github.com/XxHuberrr/Mineradio-paused)。本仓库为独立 C++ 实现，不包含这两个项目的 Electron 运行时、代码、着色器或素材。
