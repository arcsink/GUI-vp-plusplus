# Master ACE L1 cache transaction 规则设计

本文档定义 GUI-VP 中 master-ACE L1 cache 如何把 CPU/LSU 侧访问转换成 ACE
transaction。整理原则是：先保存每一版逻辑，再做下一轮修改。本轮整理建立在
baseline commit `dd9f4273` 之上，后续如果继续调整规则，也应先提交当前版本再改。

这套规则同时参考两层语义：

- RISC-V 侧：PMA/PBMT、AMO PMA、Reservability PMA、FENCE、LR/SC、AMO。
- ACE/CHI 侧：requester cache 先判断本地状态；状态不足时向 coherent fabric
  申请共享读权限、唯一写权限或 no-snoop 非一致性访问路径。

核心收束：

- 发射约束先决定“能不能发”，不直接选择 `ReadShared/ReadUnique` 等事务。
- 地址属性先决定“是否进入 coherent/snoop 域”。
- 读 miss 选择获取哪种共享读副本。
- 写路径选择是否已有 unique 权限；没有则先 upgrade、read-unique 或 dataless unique。
- Dirty owner 必须把最新数据通过 snoop response 或 writeback 交回 fabric。

## 变更方式

后续维护本文档时遵守：

- 每轮逻辑变更前，先把当前版本 commit 入库。
- 每轮只表达一个清晰主题：例如“补全 AMO 决策”、“替换 CHI 正式事务名”、
  “修正当前 VP 代码行为”。
- 不把历史推理写没；如果规则被替换，应在 commit diff 中可见旧规则到新规则的变化。

## 条件全集

### 地址和平台属性

这些条件来自 PMA、PTE PBMT、平台地址映射和 interconnect 配置。

| 变量            | 含义                             | 当前 VP 来源                                 |
| --------------- | -------------------------------- | -------------------------------------------- |
| `pma_memtype`   | PMA 原始 `Normal` 或 `Device`    | `pma_attributes::memory_type`                |
| `pma_cacheable` | PMA 原始 cacheable 属性          | `pma_attributes::cacheable`                  |
| `pma_coherent`  | PMA 原始 coherent 属性           | `pma_attributes::coherent`                   |
| `pbmt`          | PTE PBMT 原始属性                | `tlm_ext_pbmt::pbmt`                         |
| `cacheable`     | PBMT 覆盖后的 cacheable 派生结果 | `!pte_forces_uncached() && pma_cacheable`    |
| `shareable`     | 是否进入 shareable 域            | 当前由 PMA `coherent` 近似表达               |
| `snoopable`     | 是否进入 ACE snoop 域            | 当前由 PMA `coherent` 和 master-ACE 连接表达 |
| `amo_pma`       | AMO 支持等级                     | `PmaAmoClass`                                |
| `rsrv_pma`      | LR/SC reservability              | `PmaReservability`                           |
| `ordering`      | memory/IO ordering 属性          | `PmaOrdering`                                |

PMA 属性必须保持为原始输入，不要被 PBMT 直接改写。PBMT 只参与导出
`cacheable`、`is_coherent_addr`、ACE `domain/allocate` 等结果。这里的
`cacheable` 是独立派生变量，不覆盖 `pma_cacheable`。这样 PMA 与 PBMT 的组合逻辑
可以产生不同输出，例如：

- PMA main/cacheable/coherent + PBMT=PMA：允许 coherent cache。
- PMA main/cacheable/coherent + PBMT=NC/IO：PMA 原始属性仍保留，但派生
  结果强制 no-snoop。
- PMA IO 或 PMA non-coherent + PBMT=PMA：PBMT 不强制 uncached，但 PMA 仍不允许
  coherent cache。

当前 VP 中 `tlm_ext_pbmt::ace_cacheable()` 是这个组合逻辑入口：

```cpp
bool pte_forces_uncached =
    pbmt == NC || pbmt == IO || pbmt == RESERVED;

bool cacheable =
    !pte_forces_uncached && pma_cacheable;

bool is_coherent_addr =
    !pte_forces_uncached &&
    pma_memtype == MainMemory &&
    cacheable &&
    pma_coherent;
```

这里的“覆盖”是派生结果覆盖，不是字段覆盖：`pma_cacheable` 仍表示 PMA 原始
属性，`cacheable` 才表示 PBMT/PMA 合并后的 cacheability。

### 指令属性

这些条件来自 decode、LSU、AMO/LRSC 单元或后续 CMO/prefetch 单元。

| 变量                 | 含义                                                              |
| -------------------- | ----------------------------------------------------------------- |
| `instr_class`        | `Load / Store / LR / SC / AMO / Fence / SfenceVma / Prefetch / CMO` |
| `aq`                 | AMO/LR/SC acquire 位                                              |
| `rl`                 | AMO/SC release 位                                                 |
| `amo_kind`           | `swap/add/xor/and/or/min/max/...`                                 |
| `is_full_line_store` | store 是否覆盖整条 cache line                                     |
| `need_data_return`   | 写前是否必须读取旧 line 数据                                      |

`need_data_return` 通常由 `!is_full_line_store` 推导；partial store 和 AMO RMW
都必须拿旧数据。

### 发射和排序约束

这些条件来自 ROB、LSU barrier 状态机或当前 VP 的阻塞式 b_transport 顺序。

| 变量 | 含义 |
| --- | --- |
| `fence_block` | 当前是否被 FENCE 阻塞 |
| `aq_block` | acquire 约束是否阻止后续访问越过 |
| `rl_block` | release 约束是否要求 older 访问先可见 |
| `sfence_vma_active` | 是否正在进行地址翻译同步 |

原则：

- `FENCE / aq / rl / SFENCE.VMA` 先决定是否允许发射。
- 它们不直接把普通 load 改成 `ReadUnique`，也不直接把普通 store 改成
  `WriteNoSnoop`。
- 在当前 SystemC blocking transport 中，第一阶段可保守实现为：barrier/CMO/DVM
  前等待本地 writeback/writeclean 完成。

### 本地 L1 状态

| 变量 | 含义 |
| --- | --- |
| `local_hit` | 本地是否命中 |
| `local_state` | `I / SC / SD / UC / UD` |
| `has_local_unique_permission` | 是否已经有写权限 |
| `reservation_valid` | LR reservation 是否有效 |
| `reservation_match` | SC 目标是否命中 reservation |

状态含义：

- `I`：Invalid。
- `SC`：SharedClean。
- `SD`：SharedDirty。
- `UC`：UniqueClean。
- `UD`：UniqueDirty。

当前 `cache_ace` 用 `valid/shared/dirty` 三个字段近似表达这些状态。

### Requester/cache 能力

| 变量 | 含义 |
| --- | --- |
| `cache_support_dirty` | requester cache 是否能接收 dirty line |
| `cache_support_SD` | requester cache 是否支持 SharedDirty |

这些能力影响 load miss 选择：

- 不接 dirty：用 clean-only read path。
- 能接 dirty 但不接 SharedDirty：用 not-shared-dirty path。
- 能接 SharedDirty：可以用 fully shared read path。

当前 `cache_ace` 默认可以通过 response 的 `shared/dirty` 标志保存 line 状态，因此
功能上可走 `ReadShared` 或 `ReadNotSharedDirty`。文档保留三路选择，是为了让
规则以后能映射到更严格的 ACE/CHI requester 能力。

## 派生谓词

统一使用下列谓词描述路径选择：

```cpp
bool pte_forces_uncached =
    pbmt == NC || pbmt == IO || pbmt == RESERVED;

bool cacheable =
    !pte_forces_uncached && pma_cacheable;

bool is_coherent_addr =
    !pte_forces_uncached &&
    pma_memtype == MainMemory &&
    cacheable &&
    pma_coherent &&
    shareable &&
    snoopable;

bool has_local_unique_permission =
    local_hit &&
    (local_state == UC || local_state == UD);

bool lrsc_allowed =
    rsrv_pma != RsrvNone;

bool amo_allowed =
    amo_pma != AMONone &&
    amo_kind is covered by amo_pma;

bool issue_blocked =
    fence_block || aq_block || rl_block || sfence_vma_active;
```

注意：

- `rl_block` 是否作为发射阻塞取决于 LSU 是否已经 drain older memory operations。
  如果 LSU 已经保证 older 可见，则 `rl_block=false`。
- `amo_allowed` 必须按 PMA 能力等级判断：
  `AMONone < AMOSwap < AMOLogical < AMOArithmetic`。

## 决策顺序

路径选择必须按固定顺序执行。

### 1. PMA legality check

先做 PMA 合法性检查。失败时直接进入 `FaultPath`，不生成 ACE transaction。

覆盖内容：

- 访问宽度和 misaligned 规则。
- AMO class。
- LR/SC reservability。
- misaligned atomicity granule。
- implicit/page-table access 是否允许。
- idempotent 约束。

当前代码落点：`CombinedMemoryInterface_T::check_pma_or_raise()`。

### 2. 发射约束

| 条件 | 路径 |
| --- | --- |
| `issue_blocked` | `Stall` |

`Stall` 表示还不能把 transaction 发给 L1/fabric。

### 3. 特殊指令分流

| 条件 | 路径 |
| --- | --- |
| `instr_class == Fence` | `OrderingPath` |
| `instr_class == SfenceVma` | `TranslationFencePath` |
| `instr_class == CMO` | `CmoPath` |
| `instr_class == Prefetch` | `PrefetchPath` |

这些路径不复用普通 load/store 决策表。

### 4. 地址属性分流

| 条件 | 路径 |
| --- | --- |
| `!is_coherent_addr && instr_class == Load` | `ReadNoSnoop` |
| `!is_coherent_addr && instr_class == Store` | `WriteNoSnoop` |
| `!is_coherent_addr && instr_class == LR` | `ReadNoSnoop + reservation side effect` 或 `FaultPath` |
| `!is_coherent_addr && instr_class == SC` | `ScFailNoTxn` 或 `WriteNoSnoop`，取决于平台是否允许 non-coherent LR/SC |
| `!is_coherent_addr && instr_class == AMO` | `AtomicNoSnoopPath` 或 `FaultPath`，取决于 PMA 和平台支持 |

GUI-VP 第一阶段建议更保守：

- 普通 load/store 对 non-coherent 地址走 `ReadNoSnoop/WriteNoSnoop`。
- LR/SC/AMO 在 non-coherent 地址上必须先通过 PMA；如果没有明确平台需求，优先
  fault 或保持现有 bus-lock atomic 路径，不分配 coherent L1 line。

## 普通 Load

| 条件 | ACE 路径 | CHI/RN-F 语义 | Line 更新 |
| --- | --- | --- | --- |
| `local_hit` | `NoTxn` | 本地已满足读权限 | 不变 |
| `!local_hit && !cache_support_dirty` | `ReadClean` | 只接 clean data | 分配 clean shared/unique-clean 等可读状态 |
| `!local_hit && cache_support_dirty && !cache_support_SD` | `ReadNotSharedDirty` | 不接 SharedDirty | 分配可读状态，dirty owner 必须回数 |
| `!local_hit && cache_support_dirty && cache_support_SD` | `ReadShared` | 可接任意共享读响应 | 由 response 决定 `SC/SD/UC` |

当前 `cache_ace` 的 WriteBack load miss 在 `ReadShared` 和 `ReadNotSharedDirty`
之间切换，用于覆盖两类 read transaction；这不应改变功能语义。

## 普通 Store

| 条件 | ACE 路径 | CHI/RN-F 语义 | Line 更新 |
| --- | --- | --- | --- |
| `has_local_unique_permission` | `NoTxn` | 已有写权限 | 本地写入，line 变 `UD` |
| `local_hit && !has_local_unique_permission` | `CleanUnique` | Shared -> Unique upgrade | 成功后本地写入，line 变 `UD` |
| `!local_hit && !is_full_line_store` | `ReadUnique` | 获取唯一权限并取回旧数据 | 合并 partial bytes，line 变 `UD` |
| `!local_hit && is_full_line_store` | `MakeUnique` | dataless 获取唯一权限 | 分配 unique line，再写入 full payload，line 变 `UD` |

当前 `cache_ace` 的 WriteBack full-line store miss 是两步式：

1. 第一次循环 miss，发 `MakeUnique`，分配 `UD` line。
2. 下一次循环 hit unique，调用 `write_line()` 写入 CPU payload。

因此文档中 `MakeUnique` 的含义是“获取唯一权限”，不是“携带 CPU 写数据”。

## LR

RISC-V LR 是 load 加 reservation side effect。

| 条件 | 路径 |
| --- | --- |
| `!lrsc_allowed` | `FaultPath` |
| `lrsc_allowed && local_hit` | `NoTxn + reservation_valid=true` |
| `lrsc_allowed && !local_hit` | 按普通 Load miss 选择 `ReadClean/ReadNotSharedDirty/ReadShared`，再设置 reservation |

LR 不直接要求 `ReadUnique`。它建立 reservation；后续 SC 成功时才需要写权限。

## SC

RISC-V SC 的成功取决于 reservation 和权限获取。

| 条件 | 路径 |
| --- | --- |
| `!lrsc_allowed` | `FaultPath` |
| `!reservation_valid || !reservation_match` | `ScFailNoTxn` |
| `reservation_valid && reservation_match && has_local_unique_permission` | `NoTxn + Commit` |
| `reservation_valid && reservation_match && local_hit && !has_local_unique_permission` | `CleanUnique + Commit` |
| `reservation_valid && reservation_match && !local_hit && !is_full_line_store` | `ReadUnique + Commit` |
| `reservation_valid && reservation_match && !local_hit && is_full_line_store` | `MakeUnique + Commit` |

任何 snoop invalidation、本地 eviction、属性切换到 no-snoop，都必须清除相关
reservation。

## AMO

RISC-V AMO 是 read-modify-write。cache/coherency 视角下：

- 必须取回旧数据。
- 必须获得唯一写权限。
- 必须满足 AMO PMA class。

| 条件 | 路径 |
| --- | --- |
| `!amo_allowed` | `FaultPath` |
| `amo_allowed && has_local_unique_permission` | `NoTxn + local RMW` |
| `amo_allowed && local_hit && !has_local_unique_permission` | `CleanUnique + local RMW` |
| `amo_allowed && !local_hit` | `ReadUnique + local RMW` |

即使 AMO 覆盖整条 line，也不能用纯 `MakeUnique` 替代 `ReadUnique`，因为 AMO
需要旧值作为返回值和运算输入。

## Evict / Writeback

替换有效 line 前必须处理旧 line。

| 旧 line 状态 | ACE transaction | 结果 |
| --- | --- | --- |
| `UD` 或 `SD` | `WriteBack` | 下游接收 dirty data，line invalid |
| dirty line 的可选清理 | `WriteClean` | 下游接收 data，line 变 clean，可随后 evict |
| `UC` 或 `SC` | `Evict` | 通知下游 snoop filter，line invalid |

约束：

- `WriteBack/WriteClean` 必须发送整条 cache line。
- 写回事务使用 line 保存的原始 `genattr`，尤其是 `secure/qos/region/master_id`。
- Dirty owner 被其他 master snoop 时，必须通过 snoop response 或 writeback
  保证最新数据不会丢。

## Snoop 响应

下游 L2/HN-F/interconnect 对 L1 发 snoop 时，L1 必须按本地 line 状态响应。

| Snoop | 本地命中行为 | Line 更新 |
| --- | --- | --- |
| `ReadOnce` | 返回数据；当前 `cache_ace` 也会标记 shared | 至少不再保持独占语义 |
| `ReadShared` | Unique 命中设置 `was_unique`；dirty 命中提供 dirty data | line 变 shared，dirty 清除或由 response 表达 |
| `ReadClean` / `ReadNotSharedDirty` | 提供数据；Unique 命中设置 `was_unique` | line 变 shared；dirty data 必须对外可见 |
| `ReadUnique` | 提供数据；dirty 命中标记 dirty | line invalid，reservation reset |
| `CleanShared` | dirty 命中写干净或提供数据 | line 保留，dirty 清除 |
| `CleanInvalid` | dirty 命中提供数据 | line invalid，reservation reset |
| `MakeInvalid` | 不需要数据 | line invalid，reservation reset |

当前 `CoherentAceL2` 是简化目录模型：当 snoop response 标记
`datatransfer && dirty` 时，L2 会把 snoop 回来的 line 用 `WriteNoSnoop` 写回后端
memory。这是 VP 的简化回数路径，对应 CHI/HN-F 中 dirty owner 回数的原则。

## No-snoop 路径

`ReadNoSnoop/WriteNoSnoop` 用于非一致性访问：

- PBMT `NC/IO/RESERVED`。
- PMA `IO`。
- PMA `cacheable=false`。
- PMA `coherent=false`。
- 平台标记 non-shareable / non-snoopable 的地址。

规则：

- 不分配 coherent L1 line。
- 不更新 L2 directory sharer/owner。
- 不应命中已有 coherent line。当前 `cache_ace` 对这种情况有断言；完整实现应在
  属性切换前 clean+invalidate。
- IO/StrongIO 访问还需要排序路径 drain 本地 outstanding writeback/maintenance。

## ACE 与 CHI 映射参考

| 本文路径 | ACE transaction | CHI/RN-F 语义 |
| --- | --- | --- |
| `RC` | `ReadClean` | 获取 clean read data，不接 dirty responsibility |
| `RNSD` | `ReadNotSharedDirty` | 获取 read data，但不接 SharedDirty |
| `RS` | `ReadShared` | 获取可共享 read copy，允许 fabric snoop dirty owner |
| `RU` | `ReadUnique` | 获取唯一写权限，同时取回旧 data |
| `UpgradePath` | `CleanUnique` | 已有 shared line，升级到 unique |
| `DatalessUniquePath` | `MakeUnique` | 不取旧 data，只获取 unique 权限 |
| `AtomicRmwPath` | `ReadUnique` 或 `CleanUnique` 后本地 RMW | 获取旧 data + unique ownership |
| `ReadNoSnp` | `ReadNoSnoop` | Non-coherent NoSnp read |
| `WriteNoSnp` | `WriteNoSnoop` | Non-coherent NoSnp write |
| `WritebackPath` | `WriteBack/WriteClean` | copyback dirty/clean data 到 PoC |
| `EvictPath` | `Evict` | clean line replacement 的目录通知 |

核查文本逻辑时抓住三条 CHI 原理：

- 读可以共享，写必须唯一。
- Dirty 数据必须跟着 owner 转移或写回，不能只改状态。
- NoSnp/NoSnoop 与 coherent cache line 不能混用。

## 当前 VP 对齐点

- `vp/src/core/common/pma.h`：定义 `PmaAmoClass`、`PmaReservability`、
  `PmaOrdering`、cacheable/coherent 等 PMA 属性。
- `vp/src/core/common/mem.h`：先执行 PMA check，再把 PBMT 和 PMA 摘要写入
  `tlm_ext_pbmt`。
- `vp/src/util/tlm_ext_pbmt.h`：提供 `ace_cacheable()`，作为
  `is_coherent_addr` 的当前 VP 近似实现。
- `ext/libsystemctlm-soc/tlm-modules/master-ace.h`：根据 `ace_cacheable()` 设置
  `genattr` 的 `domain/bufferable/modifiable/read_allocate/write_allocate`。
- `ext/libsystemctlm-soc/tlm-modules/cache-ace.h`：实现
  `ReadNoSnoop/WriteNoSnoop`、`ReadShared/ReadNotSharedDirty/ReadUnique`、
  `CleanUnique/MakeUnique`、`WriteBack/WriteClean/Evict` 和 snoop handlers。
- `ext/lib_ace_l2_memory/ace_l2_memory_subsystem.h`：实现简化 L2 directory 和
  dirty owner snoop/writeback 回数路径。
- `doc/GUI-VP/PMA_PBMT_ACE_TEST.md`：记录 Linux 侧 PMA/PBMT/ACE trace 验证方法。

## Trace 验收规则

打开 `RVVP_ACE_TRACE_BUDGET` 后，至少验证：

- PMA main/cacheable/coherent 且 PBMT=PMA：
  `ace_cacheable=1 domain=Inner rd_alloc=1 wr_alloc=1`。
- PBMT=NC/IO：
  `ace_cacheable=0 domain=NonSharable rd_alloc=0 wr_alloc=0`，只允许 no-snoop。
- PMA_DENY：
  不应出现 ACE transaction；访问应在 PMA check 阶段变成 precise access-fault。
- Load miss：
  应出现 `ReadShared` 或 `ReadNotSharedDirty`，并在 L1 分配 line。
- Partial store miss：
  应出现 `ReadUnique`。
- Full-line store miss：
  可出现 `MakeUnique`，随后本地写入 line。
- Dirty eviction 或 dirty owner 被 snoop：
  必须出现 `WriteBack`、snoop data transfer，或当前 L2 简化的
  snooped-line writeback。
- Snoop invalidation：
  `ReadUnique/CleanInvalid/MakeInvalid` 后本地 line 不再命中，相关 SC 必须失败。
