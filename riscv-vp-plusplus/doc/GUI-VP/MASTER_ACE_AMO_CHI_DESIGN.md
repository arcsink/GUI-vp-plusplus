# Master ACE AMO 与 CHI atomic 语义设计

本文档整理 RISC-V AMO 指令从 CPU/LSU 到 master-ACE L1 cache、coherent fabric
和 memory 的语义映射。重点不是把 AMO 简化成 barrier，而是区分三类语义：

- 原子 read-modify-write：AMO 必须以不可分割的方式读取旧值、计算新值、写回。
- 一致性权限：cacheable coherent 地址必须取得唯一写权限，避免其他 requester 同时持有可写副本。
- 排序约束：AMO 的 `aq/rl` 位只表达 acquire/release 顺序，不等同于 ACE barrier。

## 当前 VP 行为

当前 RV32/RV64 AMO 执行路径是：

```text
ISS AMO
  -> set_next_amo_class()
  -> atomic_load_word/double()
  -> 本地计算新值
  -> atomic_store_word/double()
```

memory interface 里用 `bus_lock` 把 load 和 store 包住，其他 hart 的 memory
transaction 会在 `wait_for_access_rights()` 处等待。因此在当前 VP 内部，AMO 的
功能原子性基本成立。

当前已经具备的能力：

- AMO class 传到 PMA：`AMOSwap / AMOLogical / AMOArithmetic`。
- PMA 检查覆盖 AMO class、misaligned atomicity granule、access fault。
- AMO load trap 会转换为 store/AMO access fault。
- trap/interrupt 入口会释放潜在 bus lock，避免锁泄漏。

当前缺口：

- CPU 发出的 `tlm_generic_payload` 没有携带 `is_amo / amo_op / aq / rl`。
- `master-ace.h` 没有把 AMO 映射成 ACE exclusive、unique RMW 或 CHI atomic。
- `cache-ace.h` 看到的是普通 read 加普通 write，不知道二者属于同一个 AMO。
- AMO 的 `aq/rl` 顺序语义没有传到 master-ACE 或 coherent fabric。

所以当前实现适合功能模拟，但还不是协议精确的 ACE/CHI AMO 建模。

## 设计目标

AMO 设计应满足：

- 对同一个地址的 RMW 不可被其他 CPU、DMA 或 coherent requester 插入。
- 对 cacheable coherent 地址，AMO 必须在 coherent domain 中取得正确权限。
- 对 non-cacheable 或 IO 地址，必须按 PMA 决定允许、fault 或 no-snoop atomic。
- `aq/rl` 只影响发射和完成顺序，不改变 AMO 的基本 transaction 类型。
- LR/SC 不复用 AMO 规则；LR/SC 是 reservation 语义，单独建模。

## TLM 扩展建议

CPU 到 master-ACE 的 gp 需要新增一个 AMO 语义扩展，避免把 AMO 隐藏成普通
read/write：

```cpp
enum class tlm_amo_op {
    Swap,
    Add,
    Xor,
    And,
    Or,
    MinSigned,
    MaxSigned,
    MinUnsigned,
    MaxUnsigned,
};

struct tlm_ext_atomic : tlm::tlm_extension<tlm_ext_atomic> {
    bool is_amo = false;
    bool is_lr = false;
    bool is_sc = false;
    bool aq = false;
    bool rl = false;
    tlm_amo_op amo_op;
    PmaAmoClass amo_class;
};
```

第一阶段可以只实现 AMO 字段，LR/SC 字段留给后续 exclusive/reservation 设计。

## ACE 实现 AMO 的推荐路径

ACE 本身没有像 CHI 那样的一组 AtomicLoad/AtomicSwap request。master-ACE L1
实现 RISC-V AMO 时，应把 AMO 建模为“取得 unique 权限后在本地完成 RMW”。

### coherent cacheable 地址

| 本地 line 状态 | 需要动作                  | ACE transaction          | RMW 位置       | 结果状态 |
| -------------- | ------------------------- | ------------------------ | -------------- | -------- |
| `UD/UC`        | 已有唯一权限              | 无 fabric request        | L1 本地        | `UD`     |
| `SD/SC`        | 升级到唯一权限            | `CleanUnique` 或等价升级 | L1 本地        | `UD`     |
| `I`            | 获取旧数据和唯一权限      | `ReadUnique`             | L1 本地        | `UD`     |
| no-allocate    | 不建议用于 coherent AMO   | 见 non-cacheable 路径    | fabric/memory  | 不分配   |

关键原则：

- AMO 必须读取旧值，因此 miss 时不能只用 `MakeUnique`，因为 `MakeUnique` 不返回旧数据。
- 如果本地已经持有 `UD/UC`，可以不发 ACE request，直接在 L1 line 内 RMW。
- 如果本地只有 shared 状态，需要先让其他副本失效或降级，取得 unique 权限。
- RMW 期间必须阻止同一 line 的 snoop 与本地 store 交错；可以用 cache line busy
  位、master atomic slot 或当前 VP 的保守 `bus_lock`。

### non-cacheable 或 non-coherent 地址

| 条件                         | 推荐动作                         |
| ---------------------------- | -------------------------------- |
| PMA 不支持 AMO               | precise store/AMO access fault   |
| PMA 支持 AMO，平台无原子总线 | 使用当前 `bus_lock` no-snoop RMW |
| PMA 支持 AMO，fabric 支持原子 | 走 fabric atomic 或设备 atomic   |

non-cacheable AMO 不应分配 coherent L1 line。当前 VP 可保留 `bus_lock + ReadNoSnoop +
WriteNoSnoop` 作为功能模型，但文档和 trace 必须标明这不是 ACE 原子事务。

## CHI 协议插叙：AMO 应该转化出的动作

CHI 相比 ACE 增加了协议级 atomic request。对于 RISC-V AMO，如果目标是 CHI
coherent fabric，优先映射为 CHI atomic transaction，而不是拆成普通 read/write。

### CHI atomic request 分类

CHI atomic request 可以按语义分为：

| CHI 动作        | 语义                         | RISC-V 对应                         |
| --------------- | ---------------------------- | ----------------------------------- |
| `AtomicLoad`    | 在目标处执行运算，返回旧值   | `AMOADD/AMOAND/AMOOR/AMOXOR/MIN/MAX` |
| `AtomicSwap`    | 原子交换，返回旧值           | `AMOSWAP`                           |
| `AtomicCompare` | 比较后条件更新，返回旧值/状态 | 未来 CAS 扩展，不对应基础 A 扩展    |
| `AtomicStore`   | 原子更新但不返回旧值         | 仅当 `rd=x0` 且实现允许优化时可考虑 |

RISC-V 基础 AMO 架构上会把旧值写回 `rd`。即使软件把 `rd=x0`，第一阶段也建议仍使用
返回旧值的形式，避免为优化引入两套可见行为。后续确认所有异常、trace 和性能统计都
稳定后，可以把 `rd=x0` 优化为 no-return atomic。

### RISC-V AMO 到 CHI op 的建议映射

| RISC-V 指令       | CHI request      | CHI atomic op 含义          |
| ----------------- | ---------------- | --------------------------- |
| `AMOSWAP`         | `AtomicSwap`     | memory = rs2，返回旧值      |
| `AMOADD`          | `AtomicLoad`     | memory = old + rs2          |
| `AMOXOR`          | `AtomicLoad`     | memory = old xor rs2        |
| `AMOAND`          | `AtomicLoad`     | memory = old and rs2        |
| `AMOOR`           | `AtomicLoad`     | memory = old or rs2         |
| `AMOMIN/AMOMAX`   | `AtomicLoad`     | signed min/max              |
| `AMOMINU/AMOMAXU` | `AtomicLoad`     | unsigned min/max            |

CHI atomic request 应携带：

- 目标物理地址。
- access size：4 字节对应 `AMO*.W`，8 字节对应 `AMO*.D`。
- operand：`rs2` 的值。
- operation：add/and/or/xor/signed min/max/unsigned min/max/swap。
- return-data requirement：基础 RISC-V AMO 需要返回旧值。
- ordering attribute：由 `aq/rl` 和 PMA ordering 派生。

### CHI 中 atomic 的一致性效果

CHI atomic 由 HN 侧在一致性点上序列化。直观动作是：

```text
RN-F 发出 atomic request
  -> HN 根据地址找到 home node
  -> HN 对该 cache line/word 建立原子序列化点
  -> 必要时 snoop 其他 RN，收回或失效相关副本
  -> 在 home/cache/memory 处读取旧值并计算新值
  -> 写入新值
  -> 返回旧值和完成响应
```

这与 ACE L1 本地 RMW 的区别是：

- CHI atomic 可以不把整条 line 分配到 requester cache。
- HN/fabric 负责原子序列化和对其他副本的处理。
- requester 看到的是单个 atomic transaction，而不是 read 加 write 两笔普通事务。

### CHI fallback

如果 CHI fabric 或目标区域不支持 atomic request，可以退化为 cache-owned RMW：

```text
ReadUnique / MakeUnique 获取 unique 权限
  -> RN-F 本地执行 RMW
  -> line 标记 dirty
  -> 后续按 normal coherent writeback 处理
```

但 fallback 必须满足：

- miss 且需要旧值时使用 `ReadUnique`，不能使用不返回数据的 unique-only 操作。
- shared 状态必须先 upgrade 到 unique。
- 其他 RN 的副本必须被 HN/snoop 流程处理。
- AMO 期间同一 cache line 不能被本地其他访问或 snoop 打断。

## aq/rl 排序规则

RISC-V AMO 的 `aq/rl` 是 ordering 语义，不是 atomic 操作本身。

| 位       | 最低要求                                             |
| -------- | ---------------------------------------------------- |
| `aq=1`   | AMO 完成前，后续 memory operation 不能对外可见       |
| `rl=1`   | AMO 发出前，之前 memory operation 必须已经满足顺序点 |
| `aqrl=1` | 同时满足 acquire 和 release，形成更强 RMW 顺序点     |

在当前 blocking TLM 模型中，第一阶段可以采用保守策略：

- `rl=1`：AMO 发出前 drain 本 hart 已发出的 store/writeback。
- `aq=1`：AMO 完成响应返回前不允许后续 memory transaction 发出。
- 如果没有乱序 LSU，普通 blocking 顺序已经覆盖大部分场景，但文档和 trace 仍应显式记录
  `aq/rl`，避免后续引入并发时丢语义。

不要把所有 `aq/rl` 都直接转成 ACE barrier。ACE barrier 可以作为实现 ordering 的
一种工具，但 AMO 的核心动作仍然是 atomic RMW。

## 建议实现阶段

### 阶段 1：保守功能增强

- 新增 `tlm_ext_atomic`，CPU AMO 发出时挂到 gp。
- `master-ace` trace 打印 `is_amo/amo_op/aq/rl`。
- 保留现有 `bus_lock`，但把 AMO 两笔普通 gp 标记为同一个 atomic sequence。
- `cache-ace` 对 AMO coherent 地址强制走 unique RMW path：
  - hit unique：本地 RMW。
  - hit shared：先 `CleanUnique`。
  - miss：`ReadUnique`。
- non-cacheable 地址继续 `ReadNoSnoop + WriteNoSnoop`，但必须标注 atomic fallback。

### 阶段 2：协议更精确

- 为 CHI 后端增加 atomic request 抽象。
- CHI 支持时，AMO 直接映射为 `AtomicLoad/AtomicSwap`。
- CHI 不支持时，退回 unique-line RMW。
- 补 `aq/rl` drain 和 issue-block 状态机。

### 阶段 3：LR/SC 分离建模

- LR 建立 reservation，不直接等同 AMO。
- SC 成功时需要取得 unique 权限并执行条件写。
- 如果后端支持 exclusive monitor，可映射到 ACE/CHI exclusive 机制。
- 如果后端不支持，保留 VP reservation + bus_lock fallback。

## 验证点

- `AMOOR` 在 `AMOLogical` PMA 区域成功，`AMOADD` 在同一区域 fault。
- cacheable coherent AMO miss 时 trace 应出现 unique 获取动作。
- cacheable coherent AMO hit unique 时不应额外发读 miss。
- shared line 上 AMO 应先 upgrade 到 unique，再修改。
- NC/IO 派生 no-snoop 时不得分配 L1 coherent line。
- `aq/rl` trace 可见，后续 memory operation 不越过对应顺序点。
- 外部 coherent requester 存在时，不能观察到 AMO read 和 write 中间状态。

## 新增 master-ACE AMO 直接测试

Linux 自测可以确认 AMO 指令、PMA AMO class 和 fault 行为，但当前 Linux 平台的
CPU memory path 是 `SimpleBus -> SimpleMemory`，不能直接证明 `master-ACE/cache-ACE`
新增分支被覆盖。因此新增独立 SystemC smoke test：

```text
vp/tests/master_ace_amo/master_ace_amo_test.cpp
```

测试拓扑：

```text
Test CPU initiator
  -> cache_ace target_socket
  -> RecordingMemory target
```

测试方法：

- 向 `cache_ace` 注入带 `tlm_ext_atomic{is_amo=true, phase=Load}` 的 cacheable
  coherent read，模拟 AMO load 半事务。
- `RecordingMemory` 记录下游 ACE `genattr.snoop`，用 snoop 类型判断 cache-ACE
  实际发出的动作。
- AMO load miss 必须观察到 `AR::ReadUnique`。
- 先用普通 exclusive read 建立 shared line，再发 AMO load，必须观察到
  `AR::CleanUnique`，并且不能重新发 `AR::ReadUnique`。

复跑命令：

```bash
cmake -S riscv-vp-plusplus/vp -B riscv-vp-plusplus/vp/build
cmake --build riscv-vp-plusplus/vp/build --target master_ace_amo_test -j$(nproc)
riscv-vp-plusplus/vp/build/bin/master_ace_amo_test
ctest --test-dir riscv-vp-plusplus/vp/build -R master_ace_amo --output-on-failure
```

该测试的定位是协议路径覆盖：如果后续有人误把 AMO miss 改回普通 `ReadShared` 或
`ReadNotSharedDirty`，或者去掉 shared hit 上的 `CleanUnique`，测试会失败。

## Linux 原生 AMO 到 master-ACE 的验证方法

保留上面的直接 SystemC smoke test，同时在 RV64 Linux VP 中增加一条专用测试入口：

```text
Linux native AMO instruction
  -> CPU mem_if
  -> SimpleBus AMO test page
  -> AmoAceWindowBridge
     - non-AMO load/store: direct memory path
     - AMO load/store: CacheAceMaster/cache_ace path
  -> LinuxMemoryBridge
  -> SimpleMemory
```

测试窗口固定为：

```text
PMA_AMO_LOGICAL page: 0xefffe000-0xefffefff
```

这个桥接方式有两个目的：

- Linux probe 里的普通初始化写仍然直达 memory，避免把普通 `memcpy_toio()` 当作
  ACE cache 写路径测试。
- 只有 CPU AMO helper 挂载了 `tlm_ext_atomic{is_amo=true}` 的 transaction 才进入
  `CacheAceMaster`，因此 trace 能明确证明 Linux 原生 AMO 指令覆盖到了新增流程。

复跑命令：

```bash
cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)
RVVP_ACE_TRACE_BUDGET=40 \
RVVP_ACE_TRACE_START=0xefffe000 \
RVVP_ACE_TRACE_END=0xefffefff \
make run_rv64_mc VP_ARGS="--tlm-global-quantum=1000000 --use-dbbcache --tun-device tun10"
```

关键观察点：

- `guivp_pbmt_test` probe 必须打印 `AMO selftest ... PASS`。
- trace 必须出现 AMO load half：

```text
[MasterACE CacheAceMaster cpu-entry] READ addr=0xefffe000 ... amo=1 amo_op=or amo_phase=load aq=1 rl=1
```

- 下游 ACE 必须把 AMO miss 转成 unique 获取：

```text
[MasterACE CacheAceMaster.m_ace_port] READ addr=0xefffe000 len=64 snoop=0x7 domain=1 barrier=0
```

其中 `snoop=0x7` 是 ACE `AR::ReadUnique`，对应 CHI 侧的 `ReadUnique` 语义：
先取得 unique line，再执行本地 read-modify-write。

- trace 必须出现 AMO store half：

```text
[MasterACE CacheAceMaster cpu-entry] WRITE addr=0xefffe000 ... amo=1 amo_op=or amo_phase=store aq=1 rl=1
```

2026-04-14 VP 实测结果：

```text
guivp_pbmt_test: AMO selftest PMA_AMO_LOGICAL phys=0x00000000efffe000 amoor_ret=0 amoadd_ret=-14 ... PASS
Buildroot login reached
```

## 与 barrier 的边界

ACE barrier 和 AMO 不是同义词：

- barrier 是 ordering marker。
- AMO 是 atomic RMW。
- `aq/rl` 是 AMO 携带的 ordering 约束。
- 可以用 barrier/drain 实现部分 ordering，但不能用 barrier 取代 AMO 的 RMW 动作。

因此 CPU AMO 不应简单设置 `genattr.set_barrier(true)`。正确方向是新增 atomic
语义扩展，再由 ACE/CHI 后端选择 unique-line RMW 或 CHI atomic request。
