# WuwaUID — 鸣潮隐藏 UID（ReShade add-on / DX12）

[![Read in English](https://img.shields.io/badge/lang-English-2ea043?style=flat-square)](README.md) [![阅读中文版](https://img.shields.io/badge/lang-%E7%AE%80%E4%BD%93%E4%B8%AD%E6%96%87-0969da?style=flat-square)](README.zh-CN.md)

[![完全由 AI 编写](https://img.shields.io/badge/%E5%AE%8C%E5%85%A8%E7%94%B1-AI%20%E7%BC%96%E5%86%99%20%E2%80%94%20%E6%97%A0%E4%BA%BA%E7%B1%BB%E4%BD%9C%E8%80%85-ff007f?style=for-the-badge)](#-完全由-ai-编写)
[![反作弊风险](https://img.shields.io/badge/%E5%8F%8D%E4%BD%9C%E5%BC%8A%E9%A3%8E%E9%99%A9-%E6%9C%AA%E7%9F%A5-red?style=for-the-badge)](#-反作弊风险未知)

[![License: MIT](https://img.shields.io/badge/license-MIT-blue?style=flat-square)](LICENSE)
[![ReShade API](https://img.shields.io/badge/ReShade%20add--on%20API-20-blueviolet?style=flat-square)](#四安装)
[![平台](https://img.shields.io/badge/%E5%B9%B3%E5%8F%B0-Windows%20%7C%20DirectX%2012-lightgrey?style=flat-square)](#四安装)
[![依赖](https://img.shields.io/badge/%E4%BE%9D%E8%B5%96-%E6%97%A0-brightgreen?style=flat-square)](#一这是什么)

---

> [!CAUTION]
> ### 🔴 完全由 AI 编写
>
> **本项目的每一行代码 —— ReShade 集成、绘制指纹、运行时学习逻辑、以及这份文档本身 ——
> 都由 AI 编写，没有任何人类作者。** 没有人对代码做过正确性或安全性审查。
> 它只被「跑起来验证」过：正式版确实隐藏了 UID，`status` 文件里有对应数据
> （见 [第五节](#五验证记录)）；但它**没有**经过人工审阅。
>
> 请据此评估是否使用。运行前请自己读源码 —— 整个 add-on 就是一个 867 行的文件，
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

从 **v2.0.0** 起，目标不再写死在二进制里，而是**运行时学出来的**。add-on 会观察游戏
绘制 UI 文本的各批绘制，把「几乎一直在屏幕上」的那一批隐藏掉 —— UID 就是这种，
按钮不是。因此游戏更新后**不需要重新测量指纹**，也**没有配置文件要改**、**没有热键**。
原理见 [第六节](#六工作原理)。

![preview](docs/preview.jpg)

## 二、v1.0.0 的已知问题

**如果你还在用 v1.0.0，请先读这一节。** v1 能正确隐藏 UID，也从未「失灵」过，
但它有一个缺陷；这个缺陷是**用户报告**出来的，不是测试出来的：

> [!IMPORTANT]
> ### ⚪ 白框：按钮变成空白
>
> v1 在隐藏 UID 的同时，**把按钮上的文字也一起隐藏了。** 按钮的圆角底板照常绘制，
> 但叠在上面的文字没有画出来 —— 于是凡是游戏在按钮上绘制文字的界面，按钮都会变成
> **空白的白色色块**。最典型的是挑战结算界面上那两个领取按钮。
>
> 其余一切正常：UID 稳定隐藏，其他 UI 元素渲染如常，也没有崩溃。

**为什么会这样。** v1 匹配的是一条**手工测量一次、然后写死进二进制**的指纹：

```
ps=5B44B3683F03BAE3  count=156  inst=1  first=0  voff=0  indexed=1
```

外加第五个字段：索引缓冲偏移 `ibo`。这些字段里，只有 `ps`、`count` 和 `ibo`
在不同批次之间会变 —— 其余对本游戏而言是常量。如果 UID 和按钮文字是从
**同一个 UI 文本池**里分配的，它们就共享同一个 `ibo`，于是规则匹配到的是
**整个池**：UID 被隐藏，池里的每个按钮文字也一起被隐藏。

**v2.0.0 已修复。** v2 不再匹配任何指纹。它按「在屏幕上出现的比例」给候选批次排序，
只抑制占主导的那几个。UID 几乎每一帧都在画；按钮文字只在其所在界面打开时才画。
实测一次会话：UID 批次占目标绘制总数的 **96.9%**，第二名只有 **2.7%** ——
这就是按钮不再被误伤的原因。

> [!NOTE]
> v1.0.0 仍保留在 [它的 release](../../releases/tag/v1.0.0) 和 `v1.0.0` tag 中，
> 供查阅，也供想研究「当初那条指纹是怎么测出来的」的人参考。**不建议继续使用。**

## 三、下载

每个 [release](../../releases) 只附带一个文件：

| 文件 | 大小 | 说明 |
|---|---|---|
| **`WuwaUID.addon64`** | ~225 KB | 正式版。装上即可，无需任何操作。 |

v1.0.0 的 release 里另有一个 `WuwaUID-debug-hotkeys.addon64`（二分定位工具）。
**它已经不再需要、也不再发布** —— v2 不需要人工二分。详见
[第二节](#二v100-的已知问题)。

v2.0.0 的 SHA-256：

```
2990199780AAB5BDDD41CA9C625CD07D2E3653EDFC5FFAAB50DDAF577A802910  WuwaUID.addon64
```

CI 会从带 tag 的提交重新构建，因此新构建的字节可能不同但行为一致。
**以源码为准，而不是以哈希为准。**

## 四、安装

把**一个文件**复制进 ReShade 搜索 add-on 的目录，通常就是游戏主程序所在目录：

```
<游戏目录>\Client\Binaries\Win64\
    ├── Client-Win64-Shipping.exe
    ├── dxgi.dll                 ← ReShade
    └── WuwaUID.addon64          ← 本 add-on
```

也可以直接运行 `install.bat`：它会先探测常见的 Steam / Epic / 独立安装布局，
只有都匹配不上时才询问路径。也可以显式指定目录：

```
install.bat "D:\Wuthering Waves\Wuthering Waves Game\Client\Binaries\Win64"
```

ReShade 会加载以 `.addon64` 结尾的 64 位 add-on。装好后看一眼 `ReShade.log`，
应该有这几行：

```
Searching for add-ons (*.addon, *.addon64) in '...\Binaries\Win64' ...
Loading add-on from '...\WuwaUID.addon64' ...
Registered add-on "WuwaUID" v0.0.0.0 using ReShade API version 20.
```

**不需要放 `.ini`，也不需要保留任何 `.dll`。**

## 五、验证记录

add-on 会在自己旁边写一个 **`WuwaUID.status.txt`**，大约每秒刷新两次：

```
WuwaUID v2 live status
======================
mode                : automatic (target learned at runtime, no hotkeys)
frames              : 12420
draws seen          : 6417088
draws suppressed    : 657
  of which from .ini: 0
manual rules        : 0

--- target learning ---
target              : ps=5B44B3683F03BAE3 count=156
draws on target     : 5554
state               : locked on
suppressing         : inst=1 first=0 voff=0 indexed=1 ibo=14403584

candidates by decayed share (time constant 64 target draws):
  [0] inst=1 first=0 voff=0 indexed=1 ibo=14403584   share 100%  draws 3456  seen 2101..5554   <-- suppressed
  [1] inst=1 first=0 voff=0 indexed=1 ibo=24367104   share   0%  draws 1525  seen  900..2100

--- diagnostics ---
pipelines seen      : 5189 (with pixel shader: 2878)
pipeline binds      : 2881051 (PS hash resolved: 2838984)
command lists       : 72
draws w/o bound PSO : 0
```

| 看到什么 | 说明 |
|---|---|
| `state : locked on` | 已选定目标并正在抑制 |
| `state : warming up` | 目标绘制次数还太少，暂不判断 —— 刚进游戏的头几秒属正常 |
| `state : no candidate yet` | 当前没有批次过线，UID 处于可见状态 |
| 被抑制那行的 `share 100%` | 选中的批次几乎每帧都在屏幕上 —— UID 就该是这样 |
| `draws w/o bound PSO` 不为 `0` | ReShade 没有上报管线绑定，请开 issue |
| `with pixel shader` 为 `0` | 装错版本了 |

**头几秒的可见是这套方案的代价。** add-on 在看到足够多的绘制之前不会抑制任何东西，
所以刚进游戏或刚换场景时，UID 会短暂露出来：

| 场景 | 隐藏所需时间 |
|---|---|
| 只有单一文本批次主导（例如副本内） | 约 0.7 秒 |
| 同时有多个文本批次在画（例如大世界） | 约 2.5 秒 |

## 六、工作原理

全部通过 ReShade 官方文档里的 add-on API 完成。

1. **给管线上索引。** 每个新 PSO 都会触发 `addon_event::init_pipeline`。
   像素着色器字节码用 FNV-1a 哈希后存进 `PSO 句柄 → 哈希` 表。

2. **跟踪状态。** DX12 的绘制命令不携带状态，必须自己跟踪：
   `addon_event::bind_pipeline` 记录每个命令列表绑定了哪个 PSO，
   `bind_index_buffer` 记录索引缓冲偏移，`reset_command_list` 清空两者。

   > **唯一一处反直觉的地方。** 这里**不能**按 `pipeline_stage` 过滤。
   > DX12 一次调用就绑定整个 PSO，而 ReShade 并不保证给出 `pixel_shader` 位；
   > 一旦用这个掩码过滤，所有图形管线绑定都会被静默丢弃，每条绘制看起来都
   > 「没有像素着色器」。**这正是当初把第一版搞坏的那个 bug。**

3. **取指纹。** 每次 `draw` / `draw_indexed` 得到

   ```
   DrawKey { PS 哈希, 索引数, 实例数, 起始索引, 基准顶点, 是否索引绘制, ibo }
   ```

4. **选出目标。** 只有两个字段被假定为常量 —— 像素着色器哈希和索引数。其余全靠学习：

   - 每个匹配这两个字段的批次称为一个**候选**，用其余字段标识。
   - 每个候选带一个**指数加权**的绘制计数，时间常数 64 次目标绘制。
     候选的**占比** = 它的权重 ÷ 所有候选权重之和。
   - 占比过线的候选会被抑制。门槛有两道，用哪一道决定了响应速度：

     | 何时 | 门槛 | 为什么 |
     |---|---|---|
     | 从第 24 次目标绘制起 | **占比 85%** | 只有「几乎独占全部绘制」的批次才过得去 —— 副本场景里那种唯一文本批次正是如此。用不到一秒给出答案是值得的：样本少时偶尔会被假象骗过，但错误答案会在一个时间常数内自己衰减掉。 |
     | 第 90 次目标绘制之后 | **占比 38%** | 用于文本被拆到多个批次里的场景。实测：在大世界里 UID 占 43%，第二名占 27%。 |

   - 同时最多抑制 **2** 个批次。允许不止一个是刻意的 —— 在文本被拆分的场景里，
     如果坚持只认一个赢家，UID 会因为「没有任何一个批次占主导」而存活下来。
     这个上限的作用是防止它变成「把整个 UI 都抑制掉」。

   这里**完全不需要「帧」这个概念**。用帧来计数听起来自然，但是错的：
   present 回调每次真实帧的触发次数并不固定（两个会话里分别实测到 2.9 倍和 5.7 倍），
   于是一个**每帧都画**的批次曾被算成「占 17% 的帧」。所以占比是拿候选自己的绘制数
   除以目标的绘制总数 —— 这个分母不会漂。

5. **抑制。** 回调返回 `true`。ReShade 官方文档原文：

   > *To prevent this command from being executed, return `true`, otherwise
   > return `false`.*

   这条绘制命令从未进入命令队列。这是官方支持的 API 行为，不是 hack。

## 七、可调参数

第六节第 4 步里的门槛是 [`src/main.cpp`](src/main.cpp) 顶部的 `constexpr`，就是留给改的：

```cpp
constexpr uint64_t kEmaShift         = 6;     // 时间常数 = 2^6 = 64 次目标绘制
constexpr uint64_t kEarlyDraws       = 24;    // 最早允许开始判断的绘制数
constexpr uint64_t kEarlyCoveragePct = 85;    // 早期门槛
constexpr uint64_t kLearnWarmupDraws = 90;    // 超过此数后启用常规门槛
constexpr uint64_t kLearnCoveragePct = 38;    // 常规门槛
constexpr size_t   kMaxTargets       = 2;     // 同时最多抑制几个批次
```

调低 `kEarlyDraws` 或 `kEarlyCoveragePct` 会让 UID 消失得更快，同时更容易在最初
短暂地抑制错一个批次；调高 `kLearnCoveragePct` 会让判断更保守。
status 文件就是反馈回路 —— 它实时打印每个候选的占比。

## 八、什么情况下会失效

v2 没有写死的指纹，所以「游戏更新打乱 shader 或重建 UI 文本池」不会让它失效 ——
最坏情况只是收敛慢几秒，status 里的 `ps=` 值会变。

真正会让它失效的是**问题结构本身的改变**，而不是其中常量的改变：如果游戏不再把 UID
作为独立的一批绘制，或者开始把按钮和 UID 画在同一批里，那么再怎么排占比也无法区分
两者。**这就是本方案已知的结构性弱点** —— 它依赖「UID 与按钮是可分离的批次」。
如果白框症状重新出现，那就是这种情况；请附上 status 文件开 issue。

## 九、附加规则

可选：add-on 旁边的 `WuwaUID.ini` 可以**追加**固定规则，用于无条件抑制某些批次。
它只在加载时读一次，且**永远不会被写入** —— 删掉它不改变任何行为。

```ini
[WuwaUID]
Blocked=1
K0=5B44B3683F03BAE3,156,1,0,0,1
```

值的格式是 `ps,count,inst,first,voff,indexed`。注意这些是**精确指纹**，
用这种方式钉死的规则在游戏改变内存布局后就不再匹配 —— 它是逃生通道，
不是学习逻辑的替代品。

## 十、从源码构建

需要 CMake ≥ 3.20 和 MSVC（VS 2022 或 Build Tools）。

```powershell
# 1. 取 ReShade 头文件（任意你想针对的版本）
git clone --depth 1 https://github.com/crosire/reshade.git

# 2. 配置 —— include 路径必须是 ASCII，非 ASCII 路径会导致头文件查找失败
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 `
      "-DRESHADE_INCLUDE_DIR=$PWD\reshade\include"

# 3. 构建
cmake --build build --config Release

# 4. 产物
#    build\Release\WuwaUID.addon64
```

本 add-on 针对 ReShade 修订版 `aae2b7ec`（API 版本 20）构建，在 ReShade 6.8.0 上测试。

## 十一、卸载

删掉 `WuwaUID.addon64`（以及可选的 `WuwaUID.status.txt`）即可。
它不碰任何别的东西 —— 没有注册表项、没有其他目录里的文件、游戏目录里不留痕迹。

## 十二、目标是怎么找到的

这一节记录 v1 那条指纹当初是怎么测出来的。**v2 完全不需要这些** ——
保留它是因为方法本身有记录价值，而且 `v1.0.0` tag 里仍然带着那个工具。

目标是通过**二分游戏的绘制调用**逐个缩小范围找到的。候选列表有数千条，
逐条试不现实；每次砍一半则只需 `log2(n)` 步。

| 键 | 动作 |
|---|---|
| `F7` | 撤销上一次收窄（屏幕看错了时用） |
| `F8` | 重建候选表，范围重置为 `[0, 全部)` |
| `F9` | 抑制当前范围的左半部分 |
| `F10` | UID 消失了 → 保留左半 |
| `F11` | UID 还在 → 保留右半 |
| `F12` | 清除已学习规则（内置规则不受影响） |

当范围收敛到单条候选时会自动提交。它用 `log2(2115) ≈ 12` 轮收敛，
而记录的撤销深度恰好是 `12` —— 每一轮都答对了。

**两个值得记下来的失误：**

1. **第一次收敛到了错误的目标** —— `count=3`，一个三角形，不可能是 UID。
   12 轮里有一轮被看错了，而当时没有撤销功能。`F7` 就是为此而加的。

2. **每帧绘制数上限设得太紧。** 最初阈值 2.5 砍掉了太多东西；一串多字形 HUD 文本
   可能是**每个字符一次绘制**，所以合理上限要高得多（实际用了 32）。

**候选筛选条件**，供想重做这件事的人参考：出现 ≥ 3 次、`inst == 1`、
`count ∈ (0, 200000]`、每帧绘制 ≤ 32 次，按 `|每帧次数 − 1|` 升序排列 ——
一个每帧恰好绘制一次的 HUD 元素会自然排到最前面。

它测出来的指纹，也就是 v1 写死进去的那条：

```
ps=5B44B3683F03BAE3  count=156  inst=1  first=0  voff=0  indexed=1
```

156 个索引是 52 个三角形，正好对上一小串数字加其底板的几何量。

**而二分法真正搞错的地方**记录在 [第二节](#二v100-的已知问题)：
测量本身是对的，但那条指纹描述的是一**池**批次，而不是 UID 本身，
而整个流程没有任何环节会暴露这一点。**找到目标从来不是难点 ——
把它和用同样方式绘制的东西区分开才是。**

## Credits

- [ReShade](https://reshade.me/) by crosire —— 本项目的 API 基础
- [WuwaTFR](https://github.com/wuwatfr/wuthering-waves-transparency-filter-remover)
  —— 同一款游戏的同类 add-on；在梳理 ReShade DX12 事件模型时读过它的源码，
  本项目的安装布局也沿用了它

## License

[MIT](LICENSE)。本文档的英文版见 [README.md](README.md)。
