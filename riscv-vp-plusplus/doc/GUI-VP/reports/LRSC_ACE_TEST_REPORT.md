# LR/SC 到 master-ACE 验证报告

日期：2026-04-15

## 结论

LR/SC 主流程验证通过。

- Linux 侧已经能用真实 `lr.d.aqrl` / `sc.d.aqrl` 触发 VP 的 LR/SC 路径。
- VP 侧 trace 确认 LR transaction 携带 `lr=1`，SC 成功时携带 `sc=1`。
- cacheable/coherent `PMA_AMO_LOGICAL` 页上的 LR miss 会进入 master-ACE/cache-ACE，并在下游发出 `ReadUnique`。
- SC 成功后 Linux 自测读回新值，确认条件写生效。
- `PMA_RSRV_NONE` 页上 LR 和 SC 均触发 access-fault，reservability PMA 检查仍然有效。

本次验证中发现并修正了 Linux 自测的两个问题：

1. 正向 LR/SC 原本拆成两个 C helper 调用，中间插入过多指令，可能超过 VP 的 LR/SC forward-progress 窗口，导致 SC 合法失败。
2. SC 后原本用普通 `memcpy_fromio()` 读回，但当前 `AmoAceWindowBridge` 只把带 `tlm_ext_atomic` 的访问送入 ACE cache，普通读会绕过 dirty cache line 直达 backing memory。读回改为 `amoor.d.aqrl 0`，确保观察到 ACE cache 中的最新值。

## 代码流程确认

### ISS decode/execute

RV64 LR/SC 执行路径：

```text
LR_D/LR_W
  -> mem->set_next_lr_sc(aq, rl)
  -> mem->atomic_load_reserved_double/word(addr)
  -> lr_sc_counter = 17

SC_D/SC_W
  -> mem->set_next_lr_sc(aq, rl)
  -> mem->atomic_store_conditional_double/word(addr, value)
  -> rd = 0 on success, rd = 1 on failure
  -> lr_sc_counter = 0
```

关键点：

- `aq/rl` 在 ISS 层被传入 memory interface。
- SC 写回 `rd` 发生在 store path 返回之后，trap 不会先污染 `rd`。
- `lr_sc_counter` 限制 forward-progress 窗口；测试必须把 LR 和 SC 放在紧邻代码中，不能拆成两个函数后再期待必然成功。

### Memory interface

`CombinedMemoryInterface_T` 的 LR/SC 路径：

```text
atomic_load_reserved
  -> bus_lock->lock(hart)
  -> lr_addr = addr
  -> _load_data(... atomic=true, lr_sc=true)
  -> _do_transaction(READ, phase=Load)
  -> tlm_ext_atomic::set_lr(aq, rl)

atomic_store_conditional
  -> translate/check PMA reservability first
  -> if bus_lock held and addr == lr_addr:
       _store_data(... atomic=true, lr_sc=true)
       _do_transaction(WRITE, phase=Store)
       tlm_ext_atomic::set_sc(aq, rl)
       return true
     else:
       return false
```

关键点：

- SC 即使没有有效 reservation，也先做 PMA reservability 检查，避免绕过 PMA fault。
- 失败 SC 不发下游写事务；成功 SC 才能在 VP trace 中看到 `sc=1`。

### Linux platform bridge

`AmoAceWindowBridge` 对 `PMA_AMO_LOGICAL` 测试页的分流规则：

```text
if tlm_ext_atomic && (is_amo || is_lr || is_sc):
    route to CacheAceMaster
else:
    route to backing memory
```

因此：

- Linux 普通初始化 store 不进入 ACE cache。
- LR/SC/AMO 真实指令会进入 ACE cache。
- 测试中验证 SC 写入后的值时，也必须使用 atomic 访问读回。

### cache-ACE 行为

cache-ACE 把 `is_lr && phase=Load` 视为需要 unique line 的 atomic read side：

```text
LR/AMO load miss      -> ReadUnique
LR/AMO shared hit     -> CleanUnique
LR/AMO unique hit     -> no downstream ACE request
successful SC store   -> write local unique line
failed SC             -> no transaction
```

这与设计规则一致：LR 建立 reservation，SC 成功时需要已经持有或获取 unique 写权限。

## Linux 侧触发方式

驱动 `guivp_pbmt_test` 在 probe 阶段自动执行：

```text
PMA/NC/IO normal mapping selftest
PMA_DENY fault selftest
PMA_AMO_LOGICAL AMO class selftest
PMA_AMO_LOGICAL LRSC selftest
PMA_MAG16 misaligned atomicity granule selftest
PMA_RSRV_NONE reservability selftest
```

正向 LR/SC 使用连续 inline asm，并允许 SC 合法失败后有限重试：

```text
lr.d.aqrl old, (addr)
sc.d.aqrl status, new_value, (addr)
```

读回使用：

```text
amoor.d.aqrl old, zero, (addr)
```

这样读回路径也携带 `tlm_ext_atomic`，会进入 ACE cache。

## 执行命令

```sh
git -C riscv-vp-plusplus diff --check
cmake --build riscv-vp-plusplus/vp/build --target master_ace_amo_test -j$(nproc)
riscv-vp-plusplus/vp/build/bin/master_ace_amo_test
ctest --test-dir riscv-vp-plusplus/vp/build -R master_ace_amo --output-on-failure
cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)
make -C buildroot_rv64 linux-rebuild
timeout 180s env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY \
  RVVP_ACE_TRACE_BUDGET=140 \
  RVVP_ACE_TRACE_START=0xefffe000 \
  RVVP_ACE_TRACE_END=0xefffe0ff \
  make run_rv64_mc VP_ARGS="--tlm-global-quantum=1000000 --use-dbbcache --tun-device tun10"
```

说明：最后一条命令在系统到达 Buildroot login 后由 `timeout` 截停，因此退出码为 124 属于预期的仿真停止方式。

## 关键结果

### 单元测试

```text
master_ace_amo_test: PASS
ctest: 100% tests passed, 0 tests failed out of 1
```

单元测试覆盖：

- AMO load miss -> `ReadUnique`
- AMO shared hit -> `CleanUnique`
- LR miss -> `ReadUnique`
- LR shared hit -> `CleanUnique`

### Linux/VP trace

AMO 基线：

```text
[MasterACE CacheAceMaster cpu-entry] READ addr=0xefffe000 ... amo=1 amo_op=or amo_phase=load aq=1 rl=1
[MasterACE CacheAceMaster.m_ace_port] READ addr=0xefffe000 len=64 snoop=0x7 domain=1 barrier=0
[MasterACE CacheAceMaster cpu-entry] WRITE addr=0xefffe000 ... amo=1 amo_op=or amo_phase=store aq=1 rl=1
guivp_pbmt_test: AMO selftest PMA_AMO_LOGICAL ... PASS
```

LR/SC 正向路径：

```text
[MasterACE CacheAceMaster cpu-entry] READ addr=0xefffe040 ... lr=1 aq=1 rl=1
[MasterACE CacheAceMaster.m_ace_port] READ addr=0xefffe040 len=64 snoop=0x7 domain=1 barrier=0
[MasterACE CacheAceMaster cpu-entry] WRITE addr=0xefffe040 ... sc=1 aq=1 rl=1
guivp_pbmt_test: LRSC selftest PMA_AMO_LOGICAL phys=0x00000000efffe040 ret=0 read_ret=0 sc_status=0 attempts=1 lr_old=0x31415926bca777d3 read=0x31415926bca777d4 PASS
```

PMA reservability 负向路径：

```text
guivp_pbmt_test: reservability selftest PMA_RSRV_NONE phys=0x00000000efffd000 lr_ret=-14 sc_ret=-14 PASS
```

Linux 启动结果：

```text
Welcome to Buildroot
buildroot login:
```

## 判定

通过。

当前 LR/SC 代码流程符合预期：

- LR 能携带 semantic tag 进入 ACE cache。
- LR miss 正确使用 `ReadUnique` 获取旧值和 unique 权限。
- 成功 SC 能携带 `sc=1` 进入 ACE cache，并更新 unique line。
- 失败 SC 不产生下游写事务。
- PMA reservability 对 LR 和 SC 均生效。

仍需注意的模型边界：

- 当前 Linux `AmoAceWindowBridge` 是测试桥：普通访问 direct memory，atomic 访问 ACE cache。因此同一测试页上若要观察 cache 内 dirty 数据，读回也必须走 atomic 路径，或后续补充普通 coherent read 也进入 ACE cache 的平台模型。
- SC 允许合法失败，测试应保留 retry loop，而不是把单次失败判为实现错误。
