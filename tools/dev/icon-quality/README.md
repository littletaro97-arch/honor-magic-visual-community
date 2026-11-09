# 图标质量隔离检查

在 Windows x64 的 Visual Studio Developer PowerShell 中运行，需要 .NET 10 SDK：

```powershell
./tools/dev/check-icon-quality.ps1
```

如果已从当前源码构建原生桌面渲染器，可以同时验证缓存和请求调度路径：

```powershell
./tools/dev/check-icon-quality.ps1 -NativeRenderer ./artifacts/native/MagicDesktopComposition.exe
```

`-Dotnet`、`-Compiler` 可指定工具路径；`-OutputDirectory` 必须是当前源码树 `artifacts` 下的新目录。报告、合成 ICO、PNG 对比及发布后的测试程序写入该目录；SDK 构建中间文件使用测试项目的 `bin`、`obj` 目录。

检查内容：

- 同一合成图标的传统大图标提取与高分辨率 Shell 提取、透明度和编码大小；完全不透明图标可编码，菜单默认透明度规则保持不变。
- 托盘采集保留 16/32/48/64 像素来源，128 像素来源限制为 64；重复采集和空图标的 GDI 资源检查。
- 原生渲染器的 `--check-icon-cache <report.json>` 使用实际生产缓存及请求路径，验证 61/128 个 256 像素来源图标在 8 MB 以内常驻、重复刷新不淘汰其他可见项、长宽比和小图不放大，以及 128 个图标在 8 批响应后停止请求。

测试只使用自己生成的图标、内存位图和一个自有消息窗口，不附加 Explorer 托盘线程、不点击真实托盘图标、不接管桌面、不读取用户布局。自动检查通过不代替真实桌面和多屏交互验收。

托盘共享内存协议升至 v2；后台与 `MagicTrayCapture.dll` 必须一起重新构建和部署。
