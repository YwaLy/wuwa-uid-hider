# WuwaUID — 鸣潮隐藏 UID（ReShade add-on / DX12）

## 一、这是什么

一个纯 ReShade add-on。它在 **DX12** 下通过「让某一次绘制命令不执行」来隐藏屏幕上的 UID。

- 不需要 XXMI / WWMI / 3DMigoto
- 不改 shader 字节码、不读游戏内存、不注入进程、不做 vtable hook
- 只用 ReShade 官方的 add-on API 事件（`init_pipeline` / `bind_pipeline` / `draw` / `draw_indexed` / `present`）
- **无热键、无配置、无运行时搜索** —— 装上就生效，加载即隐藏
- 与 WuwaTFR（反虚化）互不干扰，可以共存

## 二、安装状态

已经装好，无需再操作：

```
E:\Game\Wuthering Waves Game\Client\Binaries\Win64\
    ├── WuwaUID.addon64        211,456 B   ← 本 addon
    ├── WuwaTFR.addon64        253,952 B   ← 反虚化（已有）
    ├── WuwaTFR.dxcompiler.dll
    ├── WuwaTFR.ini
    └── DLSS5-...-Bilibili.addon64
```

SHA256(WuwaUID.addon64) = `9FABE6BCAC754966BA83DC9D2654C3C391E31D9DF3FC510CBB69004E4086E654`

游戏目录里**没有** `WuwaUID.ini`，这是刻意的 —— 规则已经编译进二进制，不需要外部文件。

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
2. 需要重新二分定位（见附录），改 `kBuiltinRules` 后重新编译。

哈希是「像素着色器字节码的 FNV-1a」，游戏换版本、换画质档、驱动重编着色器都可能让它变。

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

## 十一、重新编译

```powershell
$cmake = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
# 注意：必须用纯 ASCII 构建目录，中文路径会让 cmake 找不到 reshade.hpp
Copy-Item src\main.cpp E:\_wuwa_uid_build\src\ -Force
& $cmake --build E:\_wuwa_uid_build\build --config Release
# 产物：E:\_wuwa_uid_build\build\Release\WuwaUID.addon64
```

---

## 附录：这个目标当初是怎么找到的

定位发生在 2026-10-02，用的是一版**带热键的临时工具**（已从正式版移除，代码不在本仓库中）。方法留档，方便日后重做：

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
