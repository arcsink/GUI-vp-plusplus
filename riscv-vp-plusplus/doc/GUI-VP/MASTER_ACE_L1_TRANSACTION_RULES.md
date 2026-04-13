# Master ACE L1 cache transaction 规则设计

本文档定义 GUI-VP 中 master-ACE L1 cache 应该怎样把 CPU 侧 load/store
transaction 转换成 ACE transaction。它建立在现有 PMA/PBMT 流程之上：
`CombinedMemoryInterface` 负责把 PTE PBMT 和 PMA 属性写入 `tlm_ext_pbmt`，
`ACEMaster` 负责把这些属性合并成 ACE `genattr`，L1 cache 再根据
`genattr`、cache line 状态和访问类型选择具体 ACE snoop transaction。

目标不是完整复刻硬件 micro-architecture，而是给 VP 一个稳定、可检查、
能跑 Linux 的事务规则：cacheable/coherent 访问走 ACE coherent L1/L2，
NC/IO 或 PMA 不允许 cache 的访问走 no-snoop，并且二者不能产生同一 cache
line 的可见不一致。

## CHI 原理对齐

这里的 master-ACE L1 可以按 CHI 的 RN-F cache 思路理解：CPU 侧 transaction
先进入 requester cache；如果本地 line 已经处在满足该访问的状态，就本地完成；
如果状态不足，L1 先向下游 coherent fabric 申请目标状态，再完成 CPU 访问。

与 CHI 对齐的基本原则：

- Read path 获取可读副本：普通读 miss 通过 `ReadShared` 或
  `ReadNotSharedDirty` 获取可共享副本；如果后续要写，则不能只停在 Shared。
- Write path 获取唯一权限：写命中 Shared 必须先 `CleanUnique`；写 miss 必须先
  `ReadUnique` 或 `MakeUnique`，然后才能本地修改 line。
- Dirty owner 负责回数：其他 master read/write 同一 line 时，dirty owner 必须
  通过 snoop response 或 writeback 把最新数据交给 fabric。
- Dataless request 只改变权限：`MakeUnique/CleanUnique/Evict` 本质上是状态请求或
  目录通知；是否需要数据由当前 line 状态和 CPU 写覆盖范围决定。
- NoSnp/NoSnoop 不参与一致性：PBMT NC/IO、PMA IO、PMA non-cacheable 或
  non-coherent 都不能分配 coherent L1 line，也不能更新 L2 目录 sharer/owner。

因此本文中的 ACE transaction 规则不是按指令类型硬编码，而是按“当前 line
状态 + 目标权限 + PMA/PBMT 属性”选择事务。这个抽象也方便以后把 ACE L1 行为
迁移或映射到 CHI RN-F 请求。

## 输入属性

每个 CPU 侧 transaction 在进入 master-ACE 前必须已经携带：

- `tlm_generic_payload`：`cmd`、`addr`、`len`、`data_ptr`、byte-enable。
- `tlm_ext_pbmt`：`pbmt`、`pma_memory_type`、`pma_ordering`、`pma_cacheable`、
  `pma_coherent`。
- 可选 `genattr_extension`：用于 exclusive、barrier、cache maintenance 等
  上层已经明确指定的 ACE 属性。

master-ACE 入口的默认合并规则：

- 默认 coherent normal memory：`domain=Inner`、`bufferable=1`、
  `modifiable=1`、`read_allocate=1`、`write_allocate=1`。
- 如果 `tlm_ext_pbmt::ace_cacheable()` 为 false，则强制 no-snoop：
  `domain=NonSharable`、`bufferable=0`、`modifiable=0`、
  `read_allocate=0`、`write_allocate=0`。
- `ace_cacheable()` 只能在 PTE PBMT 没有强制 `NC/IO/RESERVED` 且 PMA 为
  `MainMemory + cacheable + coherent` 时为 true。PTE PBMT 的 NC/IO 优先级
  高于 PMA cacheable/coherent。

## L1 line 状态

VP 当前 L1 line 可以用三个字段表示：

- `valid=false`：Invalid。
- `valid=true, shared=true, dirty=false`：SharedClean。
- `valid=true, shared=true, dirty=true`：SharedDirty，允许存在但下游 snoop
  必须能取回 dirty 数据。
- `valid=true, shared=false, dirty=false`：UniqueClean。
- `valid=true, shared=false, dirty=true`：UniqueDirty。

实现约束：

- 一个 cache line 的 tag 必须按 `CACHELINE_SZ` 对齐。
- CPU transaction 可以跨 line，L1 必须按 line 拆分处理；每个子事务只更新
  自己覆盖的 line bytes。
- `NonSharable` 或 no-allocate transaction 不应该命中仍有效的 coherent line。
  当前 `cache_ace` 对 no-snoop 命中有断言；更完整的实现应在属性切换到
  no-snoop 前先对本地 line 做 clean+invalidate，然后再绕过 L1。

## CPU read 规则

| 条件 | L1 行为 | ACE transaction | Line 更新 |
| --- | --- | --- | --- |
| `force_no_snoop` | 绕过 L1 | `ReadNoSnoop` | 不分配、不修改 line |
| cacheable read hit | 直接返回 line 数据 | 无下游事务 | line 状态不变 |
| cacheable read miss | 读取整条 line | `ReadShared` | 分配 line，`shared/dirty` 由 response 决定 |
| cacheable read miss 优化 | 读取整条 line | `ReadNotSharedDirty` | 分配 line，`shared/dirty` 由 response 决定 |
| exclusive read / LR miss | 读取整条 line 并设置本地 monitor | `ReadShared` + exclusive 属性 | 分配 line，若没有 EXOKAY 则 exclusive 失败 |
| cache maintenance read txn | 交给 maintenance 路径 | `CleanShared/CleanInvalid/MakeInvalid/...` | 见 cache maintenance 规则 |

设计取舍：

- 普通 load miss 的基线事务使用 `ReadShared`；`ReadNotSharedDirty` 可以作为性能
  或覆盖率优化，但不能改变功能语义。
- LR 不直接用 `ReadUnique`。它先通过 exclusive read 建立 monitor；SC/store
  阶段再要求唯一权限。
- `ReadNoSnoop` 必须使用原始 CPU transaction 的 `addr/len/data_ptr`，不要扩展
  成 cache line 长度。

## CPU write 规则

WriteBack L1 的规则：

| 条件 | L1 行为 | ACE transaction | Line 更新 |
| --- | --- | --- | --- |
| `force_no_snoop` | 绕过 L1 | `WriteNoSnoop` | 不分配、不修改 line |
| write hit UniqueClean/UniqueDirty | 写入 line | 无下游事务 | `dirty=true, shared=false` |
| write hit SharedClean/SharedDirty | 先拿唯一权限 | `CleanUnique` | 成功后 `shared=false`，再写入并置 dirty |
| write miss，覆盖整条 line | 不需要读旧数据，只拿唯一权限 | `MakeUnique` | 先分配 UniqueDirty line，随后在本地写入 payload |
| write miss，部分 line | 需要旧数据合并 | `ReadUnique` | 分配 line，写入后 `dirty=true, shared=false` |
| exclusive write / SC | 必须 monitor 命中 | 同普通 write 的唯一化路径 | 成功则 EXOKAY，失败不写入 |

WriteThrough L1 的规则：

| 条件 | L1 行为 | ACE transaction | Line 更新 |
| --- | --- | --- | --- |
| `force_no_snoop` | 绕过 L1 | `WriteNoSnoop` | 不分配、不修改 line |
| full-line write，byte-enable 不稀疏 | 直接写整行 | `WriteLineUnique` | 只在已有 UC/SC line 时同步更新 |
| partial/sparse write | 写唯一事务 | `WriteUnique` | 只在已有 UC/SC line 时同步更新 |

设计取舍：

- WriteBack 模式是 Linux 多核 coherent 路径的默认策略。
- partial write miss 必须用 `ReadUnique`，因为需要把未覆盖 bytes 与旧 line
  数据合并。
- full-line write miss 可以用 dataless `MakeUnique`，避免无意义读数据。当前
  `cache_ace` 的实现是两步式：第一次循环发 `MakeUnique` 分配 UniqueDirty
  line，下一次循环命中该 line 后再执行 `write_line()` 写入 CPU payload。
- `WriteUnique/WriteLineUnique` 更适合 WriteThrough 或显式直写场景；WriteBack
  hit unique 时不要发下游 write transaction。

## Evict/writeback 规则

替换有效 line 前必须处理旧 line：

| 旧 line 状态 | ACE transaction | 结果 |
| --- | --- | --- |
| UniqueDirty 或 SharedDirty | `WriteBack` | 下游接收数据，line invalid |
| dirty line 的可选清理 | `WriteClean` | 下游接收数据，line clean，可随后 evict |
| clean line | `Evict` | 通知下游 snoop filter，line invalid |

约束：

- `WriteBack/WriteClean` 必须发送整条 cache line。
- 写回事务使用 line 保存的原始 `genattr`，尤其是 `secure/qos/region/master_id`。
- dirty line 被 snoop 命中时，必须通过 snoop response 或后续 writeback 保证
  dirty 数据不会丢。

## Snoop 响应规则

下游 coherent L2 或 interconnect 对 L1 发 snoop 时，L1 必须按本地 line 状态响应：

| Snoop | 本地命中行为 | Line 更新 |
| --- | --- | --- |
| `ReadOnce` | 如果命中，返回数据；当前 `cache_ace` 也会标记 shared | line 至少不再保持独占语义 |
| `ReadShared` | 如果命中 Unique，设置 `was_unique`；dirty 时提供 dirty 数据 | line 变 Shared，dirty 清除或由 response 表达 |
| `ReadClean` / `ReadNotSharedDirty` | 如果命中，提供数据；Unique 时设置 `was_unique` | line 变 Shared；dirty 数据必须通过 response/writeback 对外可见 |
| `ReadUnique` | 如果命中，提供数据，dirty 时标记 dirty | line invalid，monitor reset |
| `CleanShared` | dirty 命中则写干净或提供数据 | line 保留，dirty 清除 |
| `CleanInvalid` | dirty 命中则提供数据 | line invalid，monitor reset |
| `MakeInvalid` | 不需要数据 | line invalid，monitor reset |

约束：

- snoop 和本地 CPU transaction 不能并发破坏同一 line；当前 `cache_ace`
  用 mutex 和 ongoing write 检查串行化，后续如果改成异步模型，需要保留这个
  原子性。
- snoop invalidate 必须 reset exclusive monitor，避免 SC 在权限被外部拿走后
  仍然成功。
- L2/HN-F 目录侧不能只改 sharer/owner 位而丢弃 dirty owner 的数据。当前
  `CoherentAceL2` 在 snoop response 标记 `datatransfer && dirty` 时，会把
  snoop 回来的 line 用 `WriteNoSnoop` 写回后端 memory，这是 VP 的简化回数路径。

## Atomic / LR-SC 规则

RISC-V AMO 在 CPU 侧会表现为 atomic load/store 序列，但进入 ACE L1 后必须满足：

- PMA 先检查 AMO class、reservability、misaligned atomicity granule；PMA 不允许
  的访问不能进入 ACE transaction 选择。
- 普通 AMO RMW 推荐在 L1 内按 "read-for-unique + local modify" 建模：miss 用
  `ReadUnique`，hit shared 用 `CleanUnique`，hit unique 直接修改。
- LR 使用 exclusive read 建立 monitor。
- SC 在 monitor 命中且成功获得唯一权限后写入；否则不写入并返回失败。
- 任意 snoop invalidation、本地 eviction、属性强制 no-snoop 都必须清除相关
  monitor。

## Ordering 规则

PMA ordering 不直接决定 cacheability，但决定 transaction 的保守程度：

- `RVWMO`：普通 main memory 默认规则，可由 RISC-V fence 在 CPU/ISS 层约束顺序。
- `RVTSO`：同一 hart 的 store/load 可在 VP 中保持更保守的发射顺序；不要为了
  cache 命中绕过已进入 L1 的 older store。
- `RelaxedIO` / `StrongIO`：通常应与 PBMT IO 或 PMA IO 一起导致
  `force_no_snoop`。`StrongIO` 需要在进入和离开 transaction 前 drain 本地
  outstanding writeback/maintenance。

当前 SystemC b_transport 路径基本是串行阻塞的，因此 ordering 的第一阶段实现
可以只保证：

- no-snoop IO 不进入 L1 分配。
- barrier/DVM/cache maintenance transaction 透传或走专门路径。
- 在处理 barrier/cache maintenance 前等待正在进行的 writeback/writeclean 完成。

## 属性到事务的总表

| PBMT/PMA 合并结果 | Read | Write | Allocate | Domain |
| --- | --- | --- | --- | --- |
| `ace_cacheable=true` | `ReadShared` / `ReadNotSharedDirty` / `ReadUnique` | WB: local dirty + `CleanUnique/MakeUnique/ReadUnique`; WT: `WriteUnique/WriteLineUnique` | 允许 | `Inner` |
| PBMT `NC` | `ReadNoSnoop` | `WriteNoSnoop` | 禁止 | `NonSharable` |
| PBMT `IO` | `ReadNoSnoop` | `WriteNoSnoop` | 禁止 | `NonSharable` |
| PBMT `RESERVED` | `ReadNoSnoop` 或提前 fault | `WriteNoSnoop` 或提前 fault | 禁止 | `NonSharable` |
| PMA `IO` | `ReadNoSnoop` | `WriteNoSnoop` | 禁止 | `NonSharable` |
| PMA `cacheable=false` | `ReadNoSnoop` | `WriteNoSnoop` | 禁止 | `NonSharable` |
| PMA `coherent=false` | `ReadNoSnoop` | `WriteNoSnoop` | 禁止 | `NonSharable` |

## CHI 映射参考

后续如果把这套规则迁移到 CHI，可按语义而不是按 ACE 名字硬套：

| 本文 ACE 语义 | CHI/RN-F 语义 |
| --- | --- |
| `ReadShared` / `ReadNotSharedDirty` | 获取可共享读副本，允许 HN-F snoop dirty owner |
| `ReadUnique` | 获取可写唯一副本，同时取回旧数据 |
| `MakeUnique` | dataless 获取唯一权限，适合 full-line overwrite |
| `CleanUnique` | 将 Shared/可能被别人持有的 line 提升为 Unique |
| `WriteBack` / `WriteClean` | copyback dirty 或 clean data，维护 PoC 最新值 |
| `Evict` | clean line replacement 的目录通知 |
| `ReadNoSnoop` / `WriteNoSnoop` | Non-coherent NoSnp/NoSnoop 路径，不分配 RN-F cache |

核查文本逻辑时只要抓住三条 CHI 原理即可：

- 读可以共享，写必须唯一。
- dirty 数据必须跟着 owner 转移或写回，不能只改状态。
- NoSnp/NoSnoop 与 coherent cache line 不能混用。

## Trace 验收规则

打开 `RVVP_ACE_TRACE_BUDGET` 后，至少要能验证以下断言：

- PBMT=PMA 且 PMA main/cacheable/coherent：
  `ace_cacheable=1 domain=Inner rd_alloc=1 wr_alloc=1`，L1 miss 后进入
  `ReadShared` 或 `ReadNotSharedDirty`，store miss 进入 `ReadUnique` 或
  `MakeUnique`。
- PBMT=NC：
  `ace_cacheable=0 domain=NonSharable rd_alloc=0 wr_alloc=0`，只允许
  `ReadNoSnoop/WriteNoSnoop`，不能分配 L1 line。
- PBMT=IO：
  同 NC，并且不允许 speculative/implicit cache fill。
- PMA_DENY：
  不应出现 ACE transaction；访问应在 PMA check 阶段变成 precise access-fault。
- Dirty eviction：
  替换 dirty line 时必须出现 `WriteBack` 或等价的数据返回路径。
- Snoop invalidation：
  `ReadUnique/CleanInvalid/MakeInvalid` 后，本地 line 不再命中，相关 SC 必须失败。

## 当前代码对齐点

- `vp/src/util/tlm_ext_pbmt.h` 已提供 `ace_cacheable()`，作为 cacheable/coherent
  合并结果入口。
- `vp/src/core/common/mem.h` 已在 `_do_transaction()` 中把 PMA/PBMT 写入同一个
  TLM payload。
- `ext/libsystemctlm-soc/tlm-modules/master-ace.h` 已根据 `ace_cacheable()` 设置
  `genattr` 的 domain 和 allocate 位。
- `ext/libsystemctlm-soc/tlm-modules/cache-ace.h` 已有 `force_no_snoop()`、
  `ReadNoSnoop/WriteNoSnoop`、`ReadShared/ReadUnique/MakeUnique/CleanUnique`、
  `WriteBack/WriteClean/Evict` 等事务路径。
- `doc/GUI-VP/PMA_PBMT_ACE_TEST.md` 已记录 Linux 侧 PMA/PBMT 测试窗口和 trace
  方法；本规则文档可作为后续实现和测试补全的判定依据。
