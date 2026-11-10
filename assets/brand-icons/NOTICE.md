# 品牌图标库

厂商品牌标识，供顶栏/菜单的「选择品牌图标」使用。当前 41 个厂商（含 HONOR 荣耀之环、机械革命、火影、攀升）、117 个 SVG。

## 手工维护的六家

- `honor-ring/` — 荣耀 2026 年 7 月启用的图形标「荣耀之环」。
  Iconify 全集都还没有它。现在的版本从用户提供的官方组合标 `honor-2.svg`
  （环 + HONOR 字标竖排）中拆出环形符号的两个子路径，采样为精确多边形。
- `mechrevo/` — 机械革命，用户提供的官方横排组合标 PNG（图形 + 机械革命 + MECHREVO），
  用 OpenCV 把左侧图形抠出矢量化。
- `firebat/`、`firebat-word/` — 火影（Firebat），用户提供官网首页
  （firebat.com.cn）的组合标：头图通过七牛缩放参数取到 1120×456 高清版，
  按行剖面拆出图案（深棕折角方 + 红色斜刃）与 FIREBAT 字标，
  用 potracer 描迹矢量化；孔洞（如 R、B 的内圈）合并为单路径 evenodd。
- `ipason/` — 攀升（iPASON），官网 CSS 里找到的组合标 PNG
  （`/img/logo/140x25_1.png`，665×220），蓝圆 + 三座 A 形山峰镂空，potracer 描迹矢量化。
- `ipason-word/` — 攀升盖子上印的「iPASON」铭牌：产品页照片分辨率不足（约 110×75），
  铭牌几何（圆角矩形、约 8° 斜度）照照片量取，字母轮廓用 Arial Black 重建，
  非描迹产物。
- 以上手工条目 — 用户提供的官方素材，来源已记录在 `manifest.json`。
- `realme/` — realme 字标，取自 worldvectorlogo（`realme-1.svg`），按本库的双档灰度重着色。
  simple-icons 从未收录 realme，Iconify 搜索只命中 arcticons 的应用图标。

这些都在 `fetch-brand-icons.py` 的 `LOCAL_ONLY` / `DIRECT` 白名单里，重跑不会覆盖。

## 每家的三种风格

| 文件 | 用途 | 来源 |
|---|---|---|
| `color.svg` | 官方彩色标（多色，原色填充） | `thesvg-color` / `logos` |
| `mono-light.svg` | 单色，白色填充，用于深色顶栏 | `simple-icons` 重着色 |
| `mono-dark.svg` | 单色，黑色填充，用于浅色顶栏 | `simple-icons` 重着色 |

单色两档是同一条路径重着色而来，所以切换深浅色不会变形。彩色标优先取官方版本，
没有官方彩色版的厂商只有单色两档（如 Apple、realme）。

例外：`honor/color.svg` — 官方彩标是 2018 年的「Moving Color」渐变字标
（品红 → 蓝紫 → 天蓝 → 青，左上向右下过渡）。可用 CDN 都只给单色版，
所以用单色字标路径加官方色板渐变（`#FF00D0 → #4E73FF → #00B1FF → #00FFFF`，
logotyp.us 收录的品牌色）手工上色。

## 已收录

手机：Apple、Samsung、Huawei、HONOR、Xiaomi、OPPO、vivo、OnePlus、realme、Meizu、ZTE、Nokia、Motorola、Sony、Google、TECNO
笔电/整机：Microsoft、Lenovo、ThinkPad、ASUS、ASUS ROG、Acer、掠夺者 Predator、Dell、Alienware、HP、MSI、机械革命、火影（图案 + 字标）、攀升（图案 + 字标）、Razer、Toshiba、Fujitsu、LG、Sharp、BlackBerry、Framework、System76

已移除：HTC —— 早已退出电脑市场，那个标不会再出现在任何一台机器的 BIOS 里。

### 手工裁切的三处

厂家给的成套标识里常常把「图案 + 字标」做在同一条路径里，缩到 24×24 就成了糊在一起的一团。
这三处是**从官方成套标识里裁掉字标、只留图案**，没有重画：

- `lg/*` — 只保留圆形笑脸；右侧的 L、G 两个字标（x ≥ 11.4）裁掉。
- `rog/*` — 原来那份是 `REPUBLIC OF GAMERS` 长字标，落在 24×24 里几乎看不见，也没有单色版，
  于是整组换成 simple-icons 的 ROG 之眼（`#FF0029`，另配单色两档）。
- `predator/*` — 从 logoquake 的官方成套标识里裁出左右两只爪（x ≤ 260），字标不要。
  这个标厂商只有单色版本，所以只有 `mono-light` / `mono-dark`，没有 `color`。

## 未收录（Iconify 现有图标集里确实没有）

`redmi`、`iqoo`、`gigabyte`、`nothing`、`infinix`、`tcl`

说明：搜索 `redmi` 只会返回 `redmine`（项目管理软件），名字前缀接近但不是同一个牌子，
因此抓取脚本只做**同名精确匹配**，宁可缺也不要不相关的标。这 6 家要补只能换来源。

## 授权

来自 [Iconify](https://iconify.design)，各图标集授权：

- `simple-icons` — CC0 1.0
- `thesvg` / `thesvg-color` — MIT
- `logos` — CC0

图标集本身是宽松授权，但**厂商标识同时受商标法保护**：可以自用、随软件展示，
不能拿来暗示官方背书，也不能把某个厂商标当作产品自己的标识。若要对外分发，
建议在设置里保留"用户自行选择"的语义（这也是我们做成可选图标而不是默认品牌的原因）。

## 历史版本（旧标）

Iconify 这几个集只收**现行**标识，没有历史版本。要新旧并存只能另找来源：

1. **Wikimedia Commons** — 历史 logo 最全，但需要按文件名逐个取，且每张图的授权/来源
   标注不一致，得逐条记录。API：`commons.wikimedia.org/w/api.php?action=query&list=search`。
2. **SVG Logos / seeklogo / worldvectorlogo** — 覆盖较广，但多数站点许可不明，仅适合本地
   自用，不适合随包分发。

如果要做"新旧切换"，建议给每家补一个 `legacy/` 子目录并把来源与许可写进 `manifest.json`
的 `provenance` 字段，不要混进当前目录。

## 新增厂商

编辑 `source/tools/dev/fetch-brand-icons.py` 的 `VENDORS`，slug 必须与图标 id 完全一致，
然后：

```bash
python source/tools/dev/fetch-brand-icons.py
```

脚本会跳过已存在的文件（断点续传），并对 429 做退避重试。
