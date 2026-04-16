# RISC-V FENCE / FENCE.I / SFENCE.VMA 详细设计指导文档

本文档用于指导 GUI-VP 中 RISC-V fence 相关语义的实现。目标不是再描述一次
“fence 是 ordering 语义”，而是把实现边界、状态机、可见点、单 hart / 多 hart
行为、MMIO 行为，以及与 ACE/CHI / PMA / PBMT / aq/rl 的关系写成可以直接指导
编码和评审的设计说明。

本文档当前只覆盖：

- `FENCE pred,succ`
- `FENCE.TSO`
- `FENCE.I`
- `SFENCE.VMA`
- AMO/LR/SC 的 `aq/rl` 与 fence 的复用关系

本文档当前不覆盖：

- `HFENCE.VVMA`
- `HFENCE.GVMA`
- H 扩展两级地址翻译

H 扩展后续应单独出设计文档，不与本文件混写。

## 0. 本次代码变更说明

本文档对应当前 GUI-VP 中已经落地的一轮 fence 相关代码修改。这里把“已经做了什么”和
“还没有做什么”单独写清楚，避免设计目标与当前代码状态混淆。

### 0.0 本次入库摘要

本次入库包含三部分内容：

1. fence 相关代码落地
   - `lscache` 增加 write-side store buffer / queue、retire、drain、forwarding
   - `FENCE pred,succ` / `FENCE.TSO` 接入 `lscache.fence(...)`
   - `FENCE.I` / `SFENCE.VMA` 在 execute 路径上与 data-side fence 串接
   - AMO/LR/SC 执行前补充 local data fence 挂点

2. 设计文档补齐
   - 补充当前实现边界
   - 补充第二阶段目标与当前代码状态之间的差异
   - 补充 Linux 可观测验证矩阵

3. Linux 测试资产补齐
   - 新增 SB 基线测试
   - 新增 `fence rw,rw` / `fence.tso` / `fence.i` 多场景综合测试
   - 记录实际 guest 运行日志

### 0.0.1 本次验证日志

本次入库对应的 Linux 测试日志与报告文件如下：

- 综合测试报告：
  - `doc/GUI-VP/testcases/FENCE_LINUX_MULTICORE_TEST_REPORT.md`
- 综合测试原始日志：
  - `doc/GUI-VP/testcases/results/guivp_fence_linux_suite_2000.log`
- SB 基线报告：
  - `doc/GUI-VP/testcases/FENCE_LINUX_SB_TEST_REPORT.md`
- SB 基线原始日志：
  - `doc/GUI-VP/testcases/results/guivp_fence_sb_run_2000.log`

这些日志对应的是在 `make run_rv64_mc` 的 RV64 Linux 多核 guest 中完成的实跑结果。

### 0.1 已落地变更

1. ISA decode / execute 路径已经显式区分普通 `FENCE`、`FENCE.I` 与 `SFENCE.VMA`
   - `FENCE` 执行时不再只是无语义占位，而是把 `pred/succ/fm` 传入 `lscache.fence(...)`
   - `FENCE.I` 执行前先调用 data-side `lscache.fence()`，再调用 `dbbcache.fence_i(pc)`
   - `SFENCE.VMA` 执行前同样先做 local data fence，再做 `dbbcache`/`lscache`/TLB flush

2. data-side 已引入最小可观测的 store buffer / queue 机制
   - 普通 main-memory store 不再一律同步直写到底层 memory interface
   - `lscache` 内部维护 store buffer entry 队列、epoch 以及 retire 逻辑
   - younger load 可以对 fully-covered older store 做本地 forwarding
   - partial overlap 场景会保守 drain 后再继续

3. `FENCE pred,succ` 已开始使用 decode 信息，而不是“无条件 full drain”
   - 当前实现里，`pred` 包含 `W` 时，会先 drain older buffered stores
   - 这是第二阶段精细化实现的第一步：开始根据 access class 判断是否需要等待 older op

4. `FENCE.TSO` 已有独立执行入口
   - 通过 `fm == 0b1000` 区分 `FENCE.TSO`
   - 当前实现仍然是保守版本：对当前已有的 write-side queue 先做本地 drain
   - trace / 测试层应把它和普通 `fence rw,rw` 区分记录

5. `FENCE.I` 的本 hart 指令可见性已经真实接到 DBBCache coherence 路径
   - `dbbcache.fence_i(pc)` 会触发本 hart `coherence_update`
   - 这会使本 hart 已缓存的 basic block 在后续 fetch/decode 时重新检查 memory 内容

### 0.2 当前仍未完成的部分

下面这些能力，本轮代码还没有实现，因此不能在文档里写成“已经具备”：

1. 还没有完整 non-blocking LSU / instruction queue / ROB / load queue
2. 还没有全局多 hart TSO 模型
3. 还没有 ACE/CHI fabric 级 barrier / ordering point
4. 还没有 remote hart 自动 `FENCE.I` / `SFENCE.VMA` shootdown
5. 还没有把 `pred/succ` 四类集合都缩到真正精确的 per-class outstanding counter

因此当前代码状态的准确表述应当是：

- 已经从“普通 `FENCE` 完全空实现”前进到“有 store-buffer queue、可 drain、可区分
  `FENCE.TSO`、可执行本 hart `FENCE.I` coherence update”的阶段；
- 但仍然不能宣称“已经实现全局 TSO”或“已经具备完整乱序微架构级 fence 语义”。

## 1. 设计依据

### 1.1 官方规范

- RISC-V Unprivileged ISA, Version 20240411
  - RVWMO memory model
  - `FENCE`
  - `FENCE.I` / Zifencei
  - `Ztso`
- RISC-V Privileged ISA
  - `SFENCE.VMA`

### 1.2 参考开源项目

本设计参考以下开源项目的边界和实现取舍：

- Spike
  - 功能模型，整体比 RVWMO / RVTSO 更强，README 明确说明其对 RVWMO 和 RVTSO
    的符合方式本质上是 sequentially consistent。
  - 适合作为“功能正确优先、允许 over-fence”的参考。
- QEMU RISC-V
  - 动态翻译器模型，`fence` / `fence.i` 的处理偏向“切断翻译块复用 / 回到主循环 /
    保守串行化”，而不是构造微架构级 store buffer。
  - 适合作为“功能模拟器不必强行建 cache-line 级 fence 实现”的参考。
- OpenSBI / RISC-V privileged spec 约定的 shootdown 流程
  - 用于多 hart `FENCE.I` / `SFENCE.VMA` 的 remote 协议边界。
- Rocket / BOOM / XiangShan 一类乱序或带 store buffer 的实现经验
  - 关键共性不是“fence 发一笔特殊 data transaction”，而是
    “drain older stores + block younger issue + 必要时等待对外可见点”。

## 2. 当前 GUI-VP 代码现状

### 2.1 已实现部分

- decoder 已能识别 `FENCE` / `FENCE.I`
- `FENCE.I` 已调用 `dbbcache.fence_i(pc)`
- `SFENCE.VMA` 已采用本 hart 全局 over-fence：
  - `dbbcache.fence_vma(pc)`
  - `lscache.fence_vma()`
  - `flush_tlb()`

### 2.2 当前缺口

普通 `FENCE` 目前仍然只是：

```text
OP_CASE(FENCE)
  -> lscache.fence()
```

而 `lscache.fence()` 当前是空实现，注释明确说明：

```text
not using out of order execution/caches so can be ignored
```

因此当前 VP 可以认为：

- `FENCE.I` 有最小功能实现
- `SFENCE.VMA` 有保守功能实现
- 普通 `FENCE` 还没有完整的 ordering 设计落地

## 3. 基本术语和实现定义

实现前必须先统一术语，否则“等待完成”“等待可见”这些词会在代码评审里失去意义。

### 3.1 local complete

某次 memory / MMIO 操作对当前 hart 来说已经结束，不再占用本 hart 的发射状态。

对于当前 blocking `b_transport` VP：

- 一次 load/store/MMIO 调用返回时即可视为 local complete。

对于未来带 queue 的 LSU / L1：

- 请求已从 issue 侧退休，但可能仍在 writeback queue / eviction queue /
  fabric pending table 中。

### 3.2 globally visible

older store 的结果已经达到此实现选定的“外部可观测点”，之后其他 hart 或设备不能再
从该实现内部看到一个比它更旧的状态。

本项目第一阶段定义如下：

- 对 blocking main-memory access：
  - `b_transport` 返回即可视为 globally visible
- 对 MMIO write：
  - 目标设备 `b_transport` 返回即可视为 side effect committed
- 对将来带异步 writeback / ACE / CHI 的路径：
  - 至少要求已经离开本地 pending queue，且满足本地实现定义的
    “不能再被 younger op 越过”的点

### 3.3 translation-visible

页表 store 已经在当前 hart 后续 page walk / TLB refill 上可见。

`SFENCE.VMA` 要保证的是：

- previous page-table stores
  before
- later implicit page-table reads by the same hart

### 3.4 issue block

fence 安装的一种阻塞条件。它不是 trap，也不是特殊事务，而是：

- younger 某类访问不允许发射
- older 某类访问先 drain / visible
- 条件满足后自动解除

## 4. GUI-VP 的分阶段目标

### 4.1 第一阶段：功能正确、保守 over-fence

适用于当前 GUI-VP 的 blocking TLM 路径。

规则：

- 同一 hart 的显式 load/store/MMIO 按程序顺序发出
- 普通 `FENCE` 可以 over-fence 成 full local data fence
- `FENCE.TSO` 可以先 over-fence 成 full local data fence
- `FENCE.I` 保证本 hart store -> later fetch 可见
- `SFENCE.VMA` 保证本 hart page-table store -> later walk 可见
- remote hart 同步不在本地 fence 指令里隐式完成，必须通过外部 IPI / SBI / shootdown 协议

### 4.2 第二阶段：协议可观测、可映射到 ACE/CHI

适用于未来引入：

- store buffer
- writeback queue
- eviction queue
- non-blocking L1
- coherent fabric

规则：

- 普通 `FENCE` 不等于“发一笔 barrier data access”
- `FENCE` 的核心是：
  - drain older matching ops
  - block younger matching issue
  - 必要时等待 fabric ordering point
- 是否发 ACE/CHI barrier 取决于后端结构，而不是由 ISA 直接规定

## 5. 本项目采用的内存模型边界

### 5.1 架构对外暴露：默认按 RVWMO + fence 语义设计

除非平台明确声明 `Ztso` / `PmaOrdering::RVTSO`，否则默认采用 RVWMO 的软件可见
语义，再通过：

- `FENCE`
- `aq/rl`
- `SFENCE.VMA`
- `FENCE.I`

补足需要的顺序保证。

### 5.2 第一阶段内部实现：允许比 RVWMO 更强

当前 GUI-VP 第一阶段允许实现比 RVWMO 更强，原因如下：

- blocking `b_transport` 本身已高度串行化
- 当前没有显式 ROB / LSQ / store buffer
- 功能正确比保留微架构级松弛更重要

因此第一阶段允许：

- 把 `FENCE pred,succ` 保守处理为 local full fence
- 把 `FENCE.TSO` 保守处理为 local full fence
- 只要不弱于规范，即可接受

### 5.3 不要在文档中声称“已经实现全局 TSO”

只有在以下条件都被清楚定义后，才能声称实现了“全局 TSO”：

- 每个 hart 的 store buffer 规则
- store -> load 的本地 forwarding 规则
- 对其他 hart 的可见顺序定义
- 多通道 / 多 target / fabric 的全局顺序点
- fence / aq/rl / atomic 在该顺序点上的精确行为

当前 GUI-VP 不满足这些条件，因此：

- 第一阶段不能宣称“实现全局 TSO”
- 只能说“当前实现比 RVWMO 更保守，单 hart 上多数行为强于 TSO”

## 5.4 为什么当前无 queue VP 中仍然保留 `FENCE`

当前 GUI-VP 没有以下结构：

- instruction queue
- ROB / LSQ
- store buffer
- non-blocking data cache pipeline
- 异步 writeback / eviction queue

同时，当前 memory path 主要依赖 blocking `b_transport`，因此同一 hart 的显式
load/store/MMIO 在实现上天然已经高度串行化。

这意味着：

- 普通 `FENCE` 当前通常不会额外“拦住某个将要越过的 younger instruction”
- 也没有真实的队列需要它去 drain
- 因而它在当前运行时的动态效果会明显弱于乱序 CPU 或带 store buffer 的模型

但这不意味着 `FENCE` 没有意义。当前仍然保留 `FENCE` 的原因有四个：

1. ISA 完整性
   - 软件、编译器、内核、驱动会发出 `fence`
   - VP 必须能 decode / execute / trace 它，否则 ISA 不完整

2. 当前模型本身已经比 RVWMO 更强
   - 很多原本需要 fence 才建立的顺序，在 blocking 顺序执行模型里天然已被保守满足
   - 因此当前 `FENCE` 的效果不是“消失了”，而是被更强的执行模型部分吸收了

3. 它仍然是软件可见的同步点
   - 多 hart 软件协议、MMIO 驱动、页表更新、自修改代码周边逻辑，仍然把 fence 当作架构同步点
   - 即使当前微架构没有 queue，这个同步点也不能从 ISA 语义层消失

4. 它是后续精化实现的正式挂点
   - 未来一旦引入 store buffer、writeback queue、ACE/CHI outstanding transaction，
     `FENCE` 就应挂接到 drain / block / ordering-point 逻辑
   - 因此现在保留它，不是“空指令占位”，而是保留正确的架构边界

因此本文档对当前普通 `FENCE` 的定位是：

- 运行时效果：在当前 blocking、无 queue VP 中通常接近 full local serialization 的
  语义占位点，而不是一个有丰富动态行为的微架构事件
- 架构意义：它仍然是合法 ISA 指令、软件同步点、trace 观察点、以及后续 LSU /
  cache / fabric ordering 机制的接入点

## 6. 单 hart 语义设计

### 6.1 当前 blocking VP 的事实行为

在当前实现里，同一 hart 的显式内存访问通常具有以下性质：

- 指令按程序顺序执行
- load/store 通过 blocking `b_transport` 同步完成
- 无显式 store buffer
- 无显式 load queue 重排

因此同一 hart 上：

- older store 不会被 younger load/store 真正越过
- older MMIO write 不会被 younger access 真正越过

这意味着当前模型在单 hart 上天然比 RVWMO 更强。

### 6.2 第一阶段的单 hart fence 规则

对于当前 GUI-VP，普通 `FENCE` 采用如下规则：

```text
execute FENCE
  1. commit 当前 hart 已累计的周期 / local quantum
  2. 保证之前所有显式 memory / MMIO 操作都已 local complete
  3. 若后续引入本地 pending queue，则等待 queue drain
  4. 返回执行后续指令
```

由于当前本地请求已经是 blocking 的，第 2 步通常天然满足；因此第一阶段允许
`lscache.fence()` 是一个“future hook”，但文档必须明确它的语义不是 no-op，而是：

```text
当前因为没有 outstanding data ops，所以无需额外动作；
若后续存在 outstanding data ops，则 fence() 的职责就是等待这些 older ops
满足本地 fence 的 ordering point。
```

### 6.3 单 hart TSO 的设计指导

如果后续要支持 `PmaOrdering::RVTSO` 或显式 Ztso，单 hart 必须明确以下规则：

- load 具有 acquire-RCpc 风格约束
- store 具有 release-RCpc 风格约束
- AMO 具有 acquire+release 的更强语义
- 允许本 hart store buffer forwarding 到后续 load
- 不允许 younger store 越过 older store
- 不允许 younger load 越过 older load
- 不允许 younger memory op 越过 AMO

实现上建议：

- 引入每 hart store buffer
- younger load 命中本地同地址 older store 时允许 forwarding
- `FENCE.TSO` 只要求实现 TSO 所需 ordering，不要求强成 full fence

在第一阶段若没有 store buffer，则：

- 可以把所有行为实现得比 TSO 更强
- 但文档必须写清楚这是 over-fence，不是精确 TSO

## 7. 多 hart / 全局可见性设计

### 7.1 `FENCE` 只约束当前 hart

`FENCE pred,succ` 的语义是：

- 当前 hart 之前的 predecessor-set 访问
  必须先于
- 当前 hart 之后的 successor-set 访问

它不是“让所有 hart 同步”的全局栅栏。

因此本项目的基本原则是：

- 普通 `FENCE` 只在本 hart 安装 ordering point
- remote hart 不会因为本 hart 执行了 `FENCE` 而自动停止或 flush

### 7.2 GUI-VP 第一阶段的全局可见性定义

对当前 blocking VP：

- main memory store 在 `b_transport` 返回时视为 globally visible
- MMIO write 在目标设备返回时视为 side effect committed

于是第一阶段的跨 hart 规则可以写成：

```text
若 hart0 执行 store; fence rw,rw; 再通知 hart1，
则 fence 返回后，该 store 已经离开 hart0 的本地执行路径，并对统一 backing memory 可见。
hart1 能否正确观察到它，还取决于 hart1 自己之后的 load / translation / fetch 路径。
```

### 7.3 将来支持 non-blocking L1 / fabric 后的全局规则

后续若有：

- writeback queue
- eviction queue
- outstanding ACE/CHI request
- 独立 MMIO request queue

则必须为每个 hart 维护一个 fence 相关的 outstanding scoreboard，例如：

```cpp
struct lsu_outstanding_state {
    uint32_t older_mem_reads = 0;
    uint32_t older_mem_writes = 0;
    uint32_t older_io_reads = 0;
    uint32_t older_io_writes = 0;
    uint32_t older_writebacks = 0;
    uint32_t older_barriers = 0;
};
```

`FENCE pred,succ` 的实现不再是空函数，而是：

- 根据 `pred` 选择要等待清零的 older bucket
- 根据 `succ` 安装 younger issue block
- 条件满足后解除 block

## 8. `FENCE pred,succ` 详细设计

### 8.1 指令字段

decoder 必须保留以下原始字段：

- `pred = instr.fence_pred()`
- `succ = instr.fence_succ()`
- `fm   = instr.fence_fm()`

即使第一阶段选择 over-fence，也必须在 trace 中保留这些字段，原因是：

- 便于后续从 full fence 缩回精确 fence
- 便于验证编译器 / 内核 / 驱动实际发出的 fence 形态

### 8.2 分类规则

`pred/succ` 位按以下分类：

- `R` -> normal memory read
- `W` -> normal memory write
- `I` -> device input / MMIO read / side-effecting read
- `O` -> device output / MMIO write / side-effecting write

实现上必须先把访问路径打上类型标签。建议基于 PMA / PBMT 导出：

- `MainMemory` -> `R/W`
- `IO` -> `I/O`

其中 `StrongIO` / `RelaxedIO` 仍通过同一标签体系进入 fence 判断，只是默认顺序属性更强。

### 8.3 第一阶段执行算法

当前 GUI-VP 第一阶段，普通 `FENCE` 采用保守 full fence 算法：

```text
on FENCE(pred, succ):
  trace(pred, succ, fm)
  commit_cycles()
  sync quantum if needed
  wait all older explicit memory and MMIO ops of this hart local-complete
  return
```

实现说明：

- 这比规范要求更强
- 但不会错误
- 适合当前 blocking VP

### 8.4 第二阶段精确算法

未来引入队列后，建议实现如下：

```text
on FENCE(pred, succ):
  1. decode pred/succ/fm
  2. mark fence sequence number = current issue age
  3. block younger issue for any class in succ
  4. wait until all older ops in pred satisfy visibility condition
  5. release fence block
```

其中 visibility condition 建议定义为：

- `R`: older normal reads 已完成，不再可能对本地后续行为产生重排序影响
- `W`: older normal writes 已 globally visible
- `I`: older IO reads 已返回且 side effect 已完成
- `O`: older IO writes 已被设备接受并 committed

### 8.5 不同 fence 形态的推荐实现

#### `fence rw,rw`

- 第一阶段：full local fence
- 第二阶段：等待所有 older normal memory read/write；阻止 younger normal memory read/write

#### `fence w,w`

- 只要求 older store 在 younger store 前可见
- 第一阶段可 over-fence 成 full local fence
- 第二阶段只 drain write-side bucket

#### `fence o,i`

- 用于典型 doorbell/status MMIO 顺序
- 目标是确保 older MMIO write 的设备 side effect 先于 younger MMIO read
- 对 IO target 不要把它误做成 cache line barrier

#### `fence iorw,iorw`

- 保守全 fence
- 第一阶段直接 full local fence

## 9. `FENCE.TSO` 详细设计

### 9.1 设计原则

`FENCE.TSO` 不能简单写成“等价于 `FENCE RW,RW`”。它是为 TSO 友好的内存序提供的
专用形式。

### 9.2 第一阶段策略

当前 GUI-VP 第一阶段允许：

- decode 阶段识别 `fm` 表示的 `FENCE.TSO`
- execute 阶段先 over-fence 成 full local data fence

这是允许的，但 trace 必须区分：

- 普通 `FENCE`
- `FENCE.TSO`

### 9.3 第二阶段策略

当实现 store buffer 时，`FENCE.TSO` 建议语义如下：

- older loads 先于 younger loads/stores
- older stores 先于 younger stores
- AMO / LR / SC 不能被普通 memory op 越过
- 是否保留 ordinary store -> ordinary load 的 TSO 允许松弛，由是否存在本地
  store-buffer-forwarding 决定

简化地说：

- `FENCE.TSO` 应约束 TSO 仍允许之外的重排
- 不要无条件强成 SC full fence，除非当前阶段明确接受 over-fence

## 10. `FENCE.I` 详细设计

### 10.1 规范要求

`FENCE.I` 保证：

- 当前 hart 之前已经对该 hart 可见的 store
  在
- 当前 hart 之后的 instruction fetch
  之前生效

它只保证本 hart，不保证 remote hart。

### 10.2 当前 GUI-VP 落地

当前保留如下实现：

```text
FENCE.I
  -> ensure prior stores complete for this hart
  -> dbbcache.fence_i(pc)
  -> continue fetch
```

其中 `dbbcache.fence_i(pc)` 通过 `coherence_update(pc)` 使旧 decode block 失效。

### 10.3 第一阶段明确规则

第一阶段明确规定：

- `FENCE.I` 之前的 store 必须已经离开当前 hart 的 blocking data path
- `dbbcache` 必须不再复用旧基本块
- 如果未来引入 I-cache，则同时 invalidate 本 hart I-cache 中相关项

### 10.4 多 hart 指导

remote hart 看见代码修改，不由本地 `FENCE.I` 隐式完成。

多 hart 自修改代码或 JIT 采用如下协议：

```text
writer hart:
  store new instructions
  fence rw,rw          # 或至少满足平台要求的数据可见性约束
  request remote shootdown / remote fence.i

remote hart:
  receive IPI / SBI RFENCE
  execute local FENCE.I
  ack back
```

因此 GUI-VP 本地 `FENCE.I` 文档中必须显式写明：

- 仅本 hart 生效
- remote 需要平台级协议

## 11. `SFENCE.VMA` 详细设计

### 11.1 语义边界

`SFENCE.VMA` 不是普通 data fence。它同步的是：

- previous explicit page-table stores
  before
- later implicit page-table reads / translation cache use

只作用于当前 hart。

### 11.2 第一阶段策略

当前 GUI-VP 继续采用本 hart 全局 over-fence：

```text
SFENCE.VMA:
  privilege checks
  wait previous local page-table stores visible
  dbbcache.fence_vma(pc)
  lscache.fence_vma()
  flush_tlb()
```

允许先忽略：

- `rs1` 的 VA 选择性
- `rs2` 的 ASID 选择性

但必须把：

- `rs1`
- `rs2`

打印到 trace，便于后续范围化。

### 11.3 多 hart 规则

`SFENCE.VMA` 只作用于本 hart。

页表更新后的 remote shootdown 流程：

```text
origin hart:
  update page tables
  data fence so stores are globally visible
  send IPI / SBI remote sfence.vma request

remote hart:
  execute local SFENCE.VMA
  ack
```

### 11.4 第二阶段增强方向

后续可逐步支持：

- `rs1=x0, rs2=x0`: global local flush
- `rs1=x0, rs2!=x0`: ASID-scoped flush
- `rs1!=x0, rs2=x0`: VA-scoped flush
- `rs1!=x0, rs2!=x0`: VA+ASID-scoped flush

## 12. 与 PMA / PBMT / MMIO 的关系

### 12.1 fence 分类必须基于地址属性

本项目已存在：

- `PmaMemoryType`
- `PmaOrdering`
- `cacheable`
- `coherent`

这些属性应成为 fence 分类输入，而不是仅靠“是不是普通 load/store 指令”判断。

### 12.2 推荐映射

#### Main memory

- `memory_type = MainMemory`
- load/store 进入 `R/W`

#### IO / MMIO

- `memory_type = IO`
- read 进入 `I`
- write 进入 `O`

#### `PmaOrdering::StrongIO`

即使没有显式 fence，也应比普通 main memory 更保守；但显式 `fence o,i` / `fence io,io`
仍需保留。

#### `PBMT=NC/IO`

可以影响 cacheability / snoopability / downstream transaction 形式，但不能取消
RISC-V fence 从 hart 视角要求的 ordering。

## 13. 与 aq/rl 的统一设计

`aq/rl` 不是 `FENCE` 指令，但建议复用同一套 ordering 状态机。

### 13.1 release (`rl`)

执行带 `rl` 的 AMO / SC 之前：

- older matching ops 必须先满足对应 visibility condition

### 13.2 acquire (`aq`)

带 `aq` 的 AMO / LR 完成前：

- younger matching ops 不允许发射越过它

### 13.3 推荐统一接口

```cpp
enum class OrderingKind {
    Fence,
    FenceTso,
    FenceI,
    SfenceVma,
    Acquire,
    Release,
    AcquireRelease,
};
```

`FENCE`、`aq`、`rl` 不应各自发明一套阻塞机制，应共享：

- older drain 判定
- younger issue block
- trace
- outstanding counter

## 14. 与 ACE / CHI 的映射原则

### 14.1 不要把 `FENCE` 直接等同 barrier transaction

RISC-V `FENCE` 是 ordering rule，不是 cache line 访问，不是 writeback 命令，也不是
必然的 ACE barrier。

### 14.2 什么时候只做本地 drain

以下场景通常不需要 fabric barrier：

- 单 hart
- blocking `b_transport`
- 无 outstanding queue
- `FENCE.I` 仅处理本 hart decode / fetch
- `SFENCE.VMA` 仅处理本 hart translation cache

### 14.3 什么时候考虑 fabric barrier

后续若存在以下结构，可考虑把 barrier 作为实现工具之一：

- pending writeback/writeclean/evict 队列
- 多 target / 多 channel 的可见顺序要求
- MMIO side effect 需要跨 interconnect 明确排序
- 平台要求 fence 的传播点超出本地 L1

但 barrier 只是后端实现选项之一，不应被文档写成 ISA 必然动作。

## 15. 推荐数据结构

```cpp
enum class FenceAccessClass : uint8_t {
    MemRead,
    MemWrite,
    IoRead,
    IoWrite,
};

struct fence_semantics {
    bool pred_i = false;
    bool pred_o = false;
    bool pred_r = false;
    bool pred_w = false;
    bool succ_i = false;
    bool succ_o = false;
    bool succ_r = false;
    bool succ_w = false;
    uint8_t fm = 0;
    bool is_tso = false;
    bool over_fence = true;
};

struct ordering_state {
    bool fence_block_mem_read = false;
    bool fence_block_mem_write = false;
    bool fence_block_io_read = false;
    bool fence_block_io_write = false;
    uint64_t last_fence_seq = 0;
};

struct outstanding_ops {
    uint32_t mem_reads = 0;
    uint32_t mem_writes = 0;
    uint32_t io_reads = 0;
    uint32_t io_writes = 0;
    uint32_t writebacks = 0;
};
```

## 16. 推荐实现步骤

### Step 1: 把 decode 信息保留下来

- `FENCE` trace 打印 `pred/succ/fm`
- `FENCE.TSO` 在 decode 或 execute 阶段可区分
- `SFENCE.VMA` trace 打印 `rs1/rs2`

### Step 2: 给 `lscache.fence()` 明确语义注释

当前即使动作为空，也要改注释，明确它代表：

- 等待当前 hart older data ops 满足 local fence ordering point

而不是“这个指令没有语义”。

### Step 3: 第一阶段保持 blocking over-fence

- `FENCE` -> full local fence
- `FENCE.TSO` -> full local fence，但 trace 标记 `is_tso`
- `FENCE.I` -> 保持 dbbcache coherence update
- `SFENCE.VMA` -> 保持 local global translation flush

### Step 4: 后续再接 outstanding queue

- 先加统计和 trace
- 再加 outstanding counter
- 最后再从 full fence 缩成精确 pred/succ fence

## 17. 验证要求

### 17.1 decode / trace

- `fence rw,rw`
- `fence w,w`
- `fence o,i`
- `fence.tso`
- `fence.i`
- `sfence.vma x0,x0`
- `sfence.vma rs1,rs2`

都要能看到对应字段。

### 17.2 单 hart 功能验证

- store A; `fence w,r`; load B
- store MMIO doorbell; `fence o,i`; read MMIO status
- patch instruction bytes; `fence.i`; jump to patched code

### 17.3 多 hart 验证

- hart0 store data + `fence`; hart1 after synchronization sees updated data
- hart0 update page table + remote `sfence.vma`; hart1 sees new translation
- hart0 patch code + remote `fence.i`; hart1 fetch sees new code

建议把多 hart 验证至少拆成下面三条，不要只做一种“有 fence 能跑通”的 happy path：

1. data ordering:
   - `store -> fence rw,rw -> load`
   - `store -> fence.tso -> load`
   - 多 hart SB / message passing 场景都要至少保留一组 Linux 或 bare-metal 回归

2. instruction visibility:
   - 同一 hart patch code，不执行 `fence.i` 时，允许继续观察到旧指令
   - 同一 hart patch code，执行 `fence.i` 后，应观察到新指令

3. remote instruction visibility:
   - hart0 patch code 并发布数据同步点后，hart1 如果不执行本 hart `fence.i`，
     不应把“看见新代码”当成保证
   - hart1 执行本 hart `fence.i` 后，应能观察到新指令

### 17.4 协议验证

未来有 ACE/CHI / pending queue 后验证：

- younger op 不得在 trace 上越过 older fence-protected op
- `fence` 本身不错误地产生普通 data transaction
- `aq/rl` 与 `FENCE` 在 trace 中可以区分

## 18. 本文档对当前项目的结论

对于 GUI-VP 当前阶段，应当明确写成：

1. 默认按 RVWMO 软件语义设计。
2. 当前 blocking VP 的实现比 RVWMO 更强，很多地方实际是 over-fence。
3. 不能宣称“已经实现全局 TSO”。
4. `FENCE` 的第一阶段实现是 full local fence。
5. `FENCE.I` 只保证本 hart；remote 依赖平台协议。
6. `SFENCE.VMA` 只保证本 hart；remote 依赖 shootdown 协议。
7. H 扩展 fence 另行设计，不在本文件落地。

## 19. 参考资料

- RISC-V Unprivileged ISA, 20240411:
  - https://docs.riscv.org/reference/isa/unpriv/rvwmo.html
  - https://docs.riscv.org/reference/isa/unpriv/zifencei.html
  - https://docs.riscv.org/reference/isa/v20240411/unpriv/ztso-st-ext.html
- RISC-V Privileged ISA:
  - https://docs.riscv.org/reference/isa/priv/supervisor.html
- RISC-V memory model explanatory material:
  - https://docs.riscv.org/reference/isa/unpriv/mm-eplan.html
- Spike:
  - https://github.com/riscv-software-src/riscv-isa-sim
- OpenSBI:
  - https://github.com/riscv-software-src/opensbi
- QEMU RISC-V translator references:
  - https://patchew.org/QEMU/20220725034728.2620750-1-daolu%40rivosinc.com/20220725034728.2620750-2-daolu%40rivosinc.com/
