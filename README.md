# Magic 视界社区版

Windows 桌面社区项目，提供统一桌面网格、虚拟大文件夹、Windows 文件操作与通用拖放、顶栏弹窗，以及声明式卡片入口和管理。

Windows 桌面宿主由独立 Win32/.NET 程序实现。桌面 Widget 的当前构建仍使用 `@seelen-ui/lib` 2.8.4，并复用部分上游样式；它们及仓库保留的其他上游内容继续适用各自许可证，详见 [`NOTICE.md`](NOTICE.md) 和 [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)。

项目优先级是**低占用、低延迟、低续航影响**。当前仍为预览阶段；自动检查与真实 Windows 桌面验收分开记录。

## 当前预览版

当前公开预发布版为 **v0.5.9**，适用于 Windows 11 x64。[下载 v0.5.9 与查看发行说明](https://github.com/littletaro97-arch/honor-magic-visual-community/releases/tag/v0.5.9) · [所有版本](https://github.com/littletaro97-arch/honor-magic-visual-community/releases)。

在发行页的 **Assets** 下载 `MagicCommunity-Setup-0.5.9-x64-unsigned.exe`，或下载 `MagicCommunity-Setup-0.5.9-x64.zip` 并解压后运行安装器。ZIP 附带安装与验收说明；可参考发行页的 `SHA256SUMS.txt` 校验下载文件。

这是未签名的预发布版，无需导入证书。安装向导会正常请求 Windows 管理员权限，请勿关闭系统安全提示。实际桌面拖放、多屏 Dock 和覆盖升级仍待实机验收，详见发行说明。

## 目录

| 目录 | 用途 |
| --- | --- |
| src/MagicCommunity | C# 桌面适配器、文件操作、布局、反馈与卡片管理 |
| src/desktop-guard | 原生桌面恢复守护 |
| widgets/desktop | 自定义桌面前端、菜单和卡片宿主 |
| widgets/shared | 共享视觉与交互资源 |
| experiments/native-host-patch | Seelen UI 上游源码与社区修改，含上游许可证 |
| tools | 构建、安装、运行时维护与集成脚本 |
| profiles | 组件配置与资源 |
| examples | 声明式卡片示例 |
| resources | 动画、卡片数据和界面资源 |

## 从源码构建

构建入口为 `tools/build-preview.ps1`，桌面 Widget 由 `widgets/desktop/build.mjs` 构建，主程序项目为 `src/MagicCommunity/MagicCommunity.csproj`。Widget 依赖及锁定版本记录在 `experiments/p0/seelen-widget/package-lock.json`。

构建需要 Windows x64、Node.js、.NET 10 SDK；独立原生后台构建还需要 Visual Studio C++ 工具链与 Rust 工具链。SDK、工具缓存、构建产物和用户运行数据不包含在源码仓库中。发行包的自动构建检查不等同于真实桌面或多屏验收。

## 许可证

项目原创软件代码采用 **GNU Affero General Public License v3.0-only（AGPL-3.0-only）**，许可全文见根目录 [`LICENSE`](LICENSE)。此许可不覆盖带有独立许可或权利声明的第三方组件、HONOR 素材、商标和其他品牌资产；具体范围见 [`NOTICE.md`](NOTICE.md) 与 [`THIRD-PARTY-NOTICES.md`](THIRD-PARTY-NOTICES.md)。HONOR 素材依项目负责人确认取得的单独再分发授权提供，不因项目源代码许可而改变权利归属。

