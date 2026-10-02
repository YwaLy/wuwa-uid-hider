# WuwaUID — 鸣潮隐藏 UID（ReShade add-on / DX12）

[![Read in English](https://img.shields.io/badge/lang-English-2ea043?style=flat-square)](README.md) [![阅读中文版](https://img.shields.io/badge/lang-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87-0969da?style=flat-square)](README.zh-CN.md)

[![完全由 AI 编写](https://img.shields.io/badge/%E5%AE%8C%E5%85%A8%E7%94%B1-AI%20%E7%BC%96%E5%86%99%20%E2%80%94%20%E6%97%A0%E4%BA%BA%E7%B1%BB%E4%BD%9C%E8%80%85-ff007f?style=for-the-badge)](#-完全由-ai-编写)
[![反作弊风险](https://img.shields.io/badge/%E5%8F%8D%E4%BD%9C%E5%BC%8A%E9%A3%8E%E9%99%A9-%E6%9C%AA%E7%9F%A5-red?style=for-the-badge)](#-反作弊风险未知)

[![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)
[![ReShade API](https://img.shields.io/badge/ReShade%20add--on%20API-20-blueviolet?style=flat-square)](#二安装)
[![平台](https://img.shields.io/badge/%E5%B9%B3%E5%8F%B0-Windows%20%7C%20DirectX%2012-lightgrey?style=flat-square)](#二安装)
[![依赖](https://img.shields.io/badge/%E4%BE%9D%E8%B5%96-%E6%97%A0-brightgreen?style=flat-square)](#一这是什么)

---

> [!CAUTION]
> ### 🔴 完全由 AI 编写
>
> **本项目的每一行代码 —— ReShade 集成、绘制指纹、二分定位工具、以及这份文档本身 ——
> 都由 AI 编写，没有任何人类作者。** 没有人对代码做过正确性或安全性审查。
> 它只被「跑起来验证」过：正式版确实隐藏了 UID，`status` 文件里有对应数据
> （见 [第四节](#四验证记录)）；但它**没有**经过人工审阅。
>
> 请据此评估是否使用。运行前请自己读源码 —— 整个 add-on 就是一个 421 行的文件，
> 每个发布产物都能由带 tag 的 CI 重新构建。

> [!WARNING]
> ### 🟠 反作弊风险：未知
>
> 鸣潮有反作弊系统。**本 add-on 是否会被检测、是否会导致封号，没有人知道。**
> 这里给不出安全答案，也没有人拿它对抗过封号潮。
>
> 能精确说明的是它**做**什么和**不做**什么：它只调用 ReShade 官方文档里的 add-on API，
> **不**修改游戏文件、**不**读取游戏内存、**不**向游戏进程写内存、**不**注入代码、
> **不**做任何 vtable hook。抑制用的返回值是官方文档明确支持的。
>
> 但 **ReShade 本身会往游戏进程里注入 DLL**，有些游戏仅凭这一点就判定为违反用户协议，
> 与那个 DLL 之后干什么无关。同类项目 WuwaTFR 的作者对同一个问题的回答只有一个词：
> **"Unknown."**
>
> **风险自负。不要用在你不舍得失去的账号上。**

## 一、这是什么

一个纯 ReShade add-on。它在 **DX12** 下通过「让某一次绘制命令不执行」来隐藏屏幕上的 UID。

本项目发布**两个版本**：

| 文件 | 大小 | 用途 |
|---|---|---|
| **`WuwaUID.addon64`** | ~211 KB | 正式版。装了就生效，无需任何操作。 |
| `WuwaUID-debug-hotkeys.addon64` | ~249 KB | 调试版。带 `F7`–`F12` 热键，用于游戏更新后重新定位目标。 |

> [!NOTE]
> **一次只装一个。** 两个版本同时存在会争抢同一批 draw。

正式版（`WuwaUID.addon64`）的特性：

- 不需要 XXMI / WWMI / 3DMigoto
- 不改 shader 字节码、不读游戏内存、不注入进程、不做 vtable hook
- 只用 ReShade 官方的 add-on API 事件（`init_pipeline` / `bind_pipeline` / `draw` / `draw_indexed` / `present`）
- **无热键、无配置、无运行时搜索** —— 装上就生效，加载即隐藏
- 与 WuwaTFR（反虚化）互不干扰，可以共存
- **不导入 `user32`** —— 正式版只链接 `KERNEL32`，看一眼导入表就能确认装的是哪一版

## 二、安装

把**一个文件**复制进 ReShade 搜索 add-on 的目录，通常就是游戏主程序所在目录：

```
<游戏目录>\Client\Binaries\Win64\
    ├── Client-Win64-Shipping.exe
    ├── dxgi.dll                 ← ReShade
    └── WuwaUID.addon64          ← 本 addon
```

也可以直接跑 `install.bat` —— 它会先探测几个常见安装位置，找不到才让你手填。

ReShade 从 `*.addon64` 加载 64 位 add-on。装好后看 `ReShade.log`，应该出现：

```
Searching for add-ons (*.addon, *.addon64) in '...\Binaries\Win64' ...
Loading add-on from '...\WuwaUID.addon64' ...
Registered add-on "WuwaUID" v0.0.0.0 using ReShade API version 20.
```

**不需要放 `.ini`，也不需要额外的 `.dll`** —— 单文件，装上就生效，规则已编译进二进制。

## 三、目标（已写死）

```cpp
constexpr DrawKey kBuiltinRules[] = {
	// ps                     count  inst  first  voff  indexed
	{ 0x5B44B3683F03BAE3ull,    156,    1,     0,    0,        1 },
};
```

即 `ps=5B44B3683F03BAE3 count=156 inst=1 first=0 voff=0 indexed=1` —— 156 个索引
（52 个三角形）正是 UID 那串数字的几何量。这条规则 2026-10-02 由二分法实测锁定。

它在 `DllMain` 里于 `load_rules()` **之后**无条件追加，所以：

- `WuwaUID.ini` 缺失 → 照常隐藏
- `WuwaUID.ini` 内容过期 → 内置规则仍然生效
- 手改 `.ini` 只能**追加**规则，删不掉内置这条

## 四、验证记录

**2026-10-03 00:11 实测成功。** 游戏内截图确认 UID 不再绘制，同时 `WuwaUID.status.txt`：

```
mode                : automatic (target baked in, no hotkeys)
frames              : 8310
draws seen          : 6724048
draws suppressed    : 1832
  of which built-in : 1832
active rules        : 1

--- diagnostics ---
pipelines seen      : 4391 (with pixel shader: 2324)
pipeline binds      : 2300225 (PS hash resolved: 2278362)
command lists       : 46
draws w/o bound PSO : 0

active rules:
  [0] ps=5B44B3683F03BAE3 count=156 inst=1 first=0 voff=0 indexed=1   (built-in)
```

值得记一笔的是 `draws suppressed` 和 `of which built-in` **完全相等** —— 当时游戏目录里
根本没有 `WuwaUID.ini`，所以这 1832 次抑制 100% 来自编译进二进制的规则。这既证明了
写死生效，也证明了隐藏不再依赖任何外部文件。

另外 `1832 / 8310 ≈ 0.22 次每帧`，说明 UI 层并非每帧重绘那串数字（很可能是按 UI 刷新
节奏或仅在数值变化时重画），属于正常现象，不是漏抑制。

## 五、怎么看它是否在工作

同一个目录下的 **`WuwaUID.status.txt`**（约每 30 帧刷新一次）：

```
WuwaUID live status
===================
mode                : automatic (target baked in, no hotkeys)
frames              : 31273
draws seen          : 30784980
draws suppressed    : 11575
  of which built-in : 11575
active rules        : 1

--- diagnostics ---
pipelines seen      : 3781 (with pixel shader: 1779)
pipeline binds      : 9966470 (PS hash resolved: 9897373)
command lists       : 45
draws w/o bound PSO : 0

active rules:
  [0] ps=5B44B3683F03BAE3 count=156 inst=1 first=0 voff=0 indexed=1   (built-in)
```

读数要点：

| 现象 | 含义 |
|---|---|
| `of which built-in` 持续增长 | 规则正在命中，UID 应该看不到 |
| 一直是 `0`，UID 仍在 | PS 哈希对不上了（见第五节） |
| `draws w/o bound PSO` 不为 0 | ReShade 没上报绑定，属于异常，把文件发我 |
| `with pixel shader` 是 0 | 同上传错了版本 |

## 六、如果 UID 又出现了

唯一的原因是**游戏更新动了着色器**，那个 `ps=` 哈希不再匹配。此时：

1. 看 `WuwaUID.status.txt` 的 `of which built-in` —— 停在 0 就是确认了这一点；
2. 把调试版 `WuwaUID-debug-hotkeys.addon64` 换上去，用第十二节的热键重新二分定位（细节见附录）；
3. 定位完成后指纹会自动写进 `WuwaUID.ini`，立刻生效；想固化进二进制则填进 `kBuiltinRules` 重新编译。

哈希是「像素着色器字节码的 FNV-1a」，游戏换版本、换画质档、驱动重编着色器都可能让它变。

> [!NOTE]
> **一次只装一个。** 换版本时先把正式版 `WuwaUID.addon64` 移出目录，再放入调试版 ——
> 两个版本共存会争抢同一批 draw。

## 七、工作原理

1. **建索引**：`init_pipeline` 事件对每个新建的 PSO 触发。取出它的像素着色器字节码，用 FNV-1a 算哈希，存进 `PSO句柄 → PS哈希` 表。
2. **跟状态**：DX12 的绘制命令本身不带状态，得自己跟。`bind_pipeline` 记下「这个命令列表当前绑的是哪个 PSO」，`reset_command_list` 清掉。
   > ⚠️ 这里**不能**按 `pipeline_stage` 过滤。DX12 一次绑定整个 PSO，ReShade 上报时不保证带 `pixel_shader` 位，一过滤就把所有图形绑定丢掉了（这是第一版失败的原因）。
3. **指纹**：每次 `draw_indexed` / `draw` 组合出
   `DrawKey{ PS哈希, 索引数, 实例数, 起始索引, baseVertex, 是否索引绘制 }`
4. **判断**：指纹落在 `g_blocked` 里就抑制。列表只有个位数条目，线性扫描比哈希表更快。
5. **抑制**：回调**返回 `true`**。ReShade 官方文档原文：

   > To prevent this command from being executed, return `true`, otherwise return `false`.

   那次绘制就真的不进入命令队列 —— 这是 API 支持的行为，不是 hack。

## 八、加规则

`WuwaUID.ini` 可选，格式 `ps,count,inst,first,voff,indexed`：

```ini
[WuwaUID]
Blocked=1
K0=5B44B3683F03BAE3,156,1,0,0,1
```

要**永久**加规则就写进 `kBuiltinRules` 重新编译 —— `.ini` 只是给你临时试的。

## 九、卸载

删掉 `WuwaUID.addon64`（和可选的 `WuwaUID.status.txt`）即可，不留任何痕迹。

## 十、风险

- **反作弊**：鸣潮有反作弊系统。本 addon 走的是 ReShade 官方 API，不改游戏文件、不读游戏内存，但 ReShade 本身注入游戏进程这件事在部分游戏里是被判定为违规的。WuwaTFR 作者在 README 里对「会不会封号」的回答就是 **"Unknown."**。用不用自己判断，风险自负。
- **ReShade 版本**：本 addon 按 ReShade API rev `aae2b7ec` 编译，本机是 ReShade 6.8.0，匹配。
- **性能**：每个 draw 一次哈希查表（带一把 mutex），实测开销远低于一帧预算。

## 十一、从源码编译

需要 CMake ≥ 3.20 和 MSVC（VS 2022 或 Build Tools）。

```powershell
# 1. 拉 ReShade 头文件
git clone --depth 1 https://github.com/crosire/reshade.git

# 2. 配置 —— 头文件路径必须是纯 ASCII，中文路径会让 cmake 找不到 reshade.hpp
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      "-DRESHADE_INCLUDE_DIR=$PWD\reshade\include"

# 3. 编译（默认同时编两个 target）
cmake --build build --config Release

# 4. 产物
#    build\Release\WuwaUID.addon64                正式版（无热键）
#    build\Release\WuwaUID-debug-hotkeys.addon64  调试版（F7–F12）
#
#    配置时加 -DBUILD_HOTKEY_DEBUG=OFF 可只编正式版
```

本 addon 按 ReShade rev `aae2b7ec`（API version 20）编译，实测环境是 ReShade 6.8.0。

## 十二、热键（仅调试版）

**正式版 `WuwaUID.addon64` 没有任何热键** —— 这正是它的设计目标。
下表只适用于 **`WuwaUID-debug-hotkeys.addon64`**：

| 键 | 作用 |
|---|---|
| `F7` | 撤销上一轮（看走眼时退回） |
| `F8` | 重建候选表，区间重置成 `[0, 全部)` |
| `F9` | 试探左半区 —— 抑制当前区间左半的所有 draw |
| `F10` | UID 消失了 → 保留左半区 |
| `F11` | UID 还在 → 保留右半区 |
| `F12` | 清空学到的规则（内置规则保留） |

调试版与正式版的其它差异：

- 调试版**始终保留内置规则**，所以你一边找新目标，UID 一边还是隐藏的
- 调试版会把学到的东西**写进** `WuwaUID.ini`，并输出更详细的 `WuwaUID-debug.status.txt`
  （约每秒 4 次刷新，含候选表、二分区间、撤销深度、筛选拒绝统计）
- 调试版链接 `user32`（`GetAsyncKeyState` 是唯一用途）

定位完成后，指纹会自动写进 `WuwaUID.ini`，之后一直有效；想固化进二进制就把它填进
`kBuiltinRules` 重新编译（见 [第三节](#三目标已写死)）。

---

## 附录：这个目标当初是怎么找到的

定位发生在 2026-10-02，用的是**带热键的调试版**。它的源码在
[`hotkey-debug/src/main.cpp`](hotkey-debug/src/main.cpp)，二进制随每个 release 一起发布。
方法留档，方便日后重做：

**二分法**：候选表有两三千项，逐个试不现实。

| 键 | 作用 |
|---|---|
| F7 | 撤销上一轮（看走眼时退回） |
| F8 | 重建候选表，区间重置成 `[0, 全部)` |
| F9 | 试探左半区 —— 抑制当前区间左半的所有 draw |
| F10 | UID 消失了 → 保留左半区 |
| F11 | UID 还在 → 保留右半区 |
| F12 | 清空学到的规则（内置规则保留） |

缩到只剩 1 个候选时自动锁定。`log2(2115) ≈ 12` 轮收敛，实测 `undo depth = 12`，说明每轮都答对了。

**两次踩过的坑**：

1. **第一次收敛到了错误目标**（`count=3`，只有一个三角形，不可能是 UID）。原因是 12 轮里看错了一轮，而当时没有撤销功能 —— 所以后来加了 F7。
2. **候选筛选的每帧上限不能卡太死**。最初设 2.5，把 UID 之外的很多东西误杀了；放宽到 32 才合理，因为多字形 HUD 字符串可能是**每字符一次 draw**。

**候选筛选条件**（供重做时参考）：出现 ≥3 次、`inst == 1`、`count ∈ (0, 200000]`、每帧次数 ≤ 32。排序键用 `|per_frame − 1|` 升序 —— UID 这类整帧只画一次的东西天然排在前面。
