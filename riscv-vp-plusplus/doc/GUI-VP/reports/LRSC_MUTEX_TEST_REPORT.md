# LR/SC 跨核互斥等待验证报告

日期：2026-04-15

## 结论

之前的 [`LRSC_ACE_TEST_REPORT.md`](LRSC_ACE_TEST_REPORT.md) 覆盖了单核 LR/SC 正向成功、SC 语义标记、ACE trace、PMA reservability fault，但**没有覆盖**：

```text
A hart 执行 LR 并持有 reservation/bus_lock
B hart 访问同属性地址
B hart 必须等待 A hart SC/unlock 后才能继续
```

本次新增两类测试，专门验证这个互斥等待场景。测试通过。

- VP 侧单元测试 `lrsc_bus_lock_test`：确定性验证 BusLock 等待/恢复语义。
- Linux 侧 boot-time selftest `LRSC mutex selftest`：用真实 `lr.d/sc.d` 和另一个 hart 的同地址 `amoor.d` 触发 VP 等待路径。

## 新增 VP 侧测试

新增文件：

- `vp/tests/lrsc_bus_lock/lrsc_bus_lock_test.cpp`
- `vp/tests/lrsc_bus_lock/CMakeLists.txt`

CMake 新增：

```cmake
add_subdirectory(tests/lrsc_bus_lock)
```

测试模型：

```text
hart A:
  bus_lock.lock(0)              # 模拟 LR 建立 reservation 并持有 bus lock
  wait until hart B starts access
  keep lock for 100 ns
  bus_lock.unlock(0)            # 模拟 SC/release

hart B:
  wait until hart A has locked
  call bus_lock.wait_for_access_rights(1)
  verify it resumes only after hart A unlocks
```

测试地址标记为：

```text
0xefffe080
```

该地址位于 `PMA_AMO_LOGICAL` 页内，和 Linux LR/SC 正向测试使用同一类 cacheable/coherent/PMA atomic 属性。

## 为什么放在 VP 侧单元测试

Linux 侧直接构造“A 核 LR 后长时间等待 B 核访问”的用例不稳定，因为当前 VP 的 LR/SC forward-progress 窗口是有限的：

```text
LR 后 lr_sc_counter = 17
每次慢路径单步递减
归零后 release_lr_sc_reservation()
```

也就是说，真实 RISC-V LR/SC 序列必须在 LR 后很短窗口内到达 SC。Linux 线程同步、kthread 调度、completion/atomic 变量轮询都会插入大量指令，很容易让 SC 合法失败。这个行为符合 RISC-V 对 SC 可失败的允许范围，但不适合作为“B hart 是否等待 bus_lock”的确定性测试。

因此本次测试直接验证 VP 的互斥等待核心：

```cpp
bus_lock.wait_for_access_rights(hart_id)
```

该函数正是 `CombinedMemoryInterface_T::_raw_load_data()` 和 `_raw_store_data()` 在所有普通/atomic data memory transaction 前调用的等待点。

## 新增 Linux 侧测试

新增到 `drivers/misc/guivp_pbmt_test.c` 的 boot-time selftest：

```text
guivp_pbmt_selftest_pma_lrsc_mutex()
```

测试地址：

```text
0xefffe080
```

测试模型：

```text
A hart:
  kthread 主线程在 PMA_AMO_LOGICAL + 0x80 上执行 lr.d.aqrl
  在 LR 和 SC 中间插入 12 个 nop，保持在 VP 的 17 指令 LR/SC 窗口内
  执行 sc.d.aqrl 并统计 SC 成功次数

B hart:
  kthread_bind() 绑到另一个 online CPU
  每轮在 A hart 开始 LR/SC 后，对同一地址执行 amoor.d.aqrl 0
  该 AMO 不改变数据值，但会走同属性、同地址的 atomic data transaction
```

新增辅助函数：

```text
guivp_pbmt_lrsc_hold_nofault()
```

它和已有 `guivp_pbmt_lrsc_nofault()` 的区别是 LR 和 SC 之间保留一个很短的真实指令窗口，让 B hart 有机会在 A hart 持有 reservation/bus_lock 时进入访问路径。

为了在 VP 日志中直接观察等待，本次还新增了可选 trace：

```text
RVVP_BUS_LOCK_TRACE=1
```

开启后，`BusLock::lock()` 在非 owner hart 等待和恢复时打印：

```text
[BusLock] hart=<B> wait owner=<A> time=<t>
[BusLock] hart=<B> resume time=<t>
```

## 代码流程确认

### LR 建锁

VP LR 路径：

```text
ISS LR
  -> mem->set_next_lr_sc(aq, rl)
  -> mem->atomic_load_reserved_word/double(addr)
  -> CombinedMemoryInterface_T::_atomic_load_reserved_data()
       bus_lock->lock(hart_id)
       lr_addr = addr
       _load_data(... atomic=true, lr_sc=true)
```

### B hart 等待

任意非 owner hart 发起 data memory 访问时：

```text
_raw_load_data/_raw_store_data
  -> bus_lock->wait_for_access_rights(hart_id)
       if bus_lock is locked by another hart:
           wait_until_unlocked()
```

`BusLock::wait_until_unlocked()`：

```cpp
while (locked)
    sc_core::wait(lock_event);
```

### SC 释放

VP SC 成功路径：

```text
atomic_store_conditional
  -> if bus_lock held by same hart && addr == lr_addr:
       _store_data(... atomic=true, lr_sc=true)
       atomic_unlock()
       return true
```

`BusLock::unlock()` 会：

```cpp
locked = false;
lock_event.notify(SC_ZERO_TIME);
```

因此 B hart 会在 `lock_event` 触发后恢复。

## 执行命令

### VP 单元测试

```sh
cmake -S riscv-vp-plusplus/vp -B riscv-vp-plusplus/vp/build
cmake --build riscv-vp-plusplus/vp/build --target lrsc_bus_lock_test -j$(nproc)
riscv-vp-plusplus/vp/build/bin/lrsc_bus_lock_test
ctest --test-dir riscv-vp-plusplus/vp/build -R lrsc_bus_lock --output-on-failure
```

### Linux boot-time 测试

```sh
cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)
make -C buildroot_rv64 linux-rebuild
timeout 240s env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
  -u all_proxy -u ALL_PROXY \
  RVVP_BUS_LOCK_TRACE=1 \
  RVVP_ACE_TRACE_BUDGET=260 \
  RVVP_ACE_TRACE_START=0xefffe000 \
  RVVP_ACE_TRACE_END=0xefffe0ff \
  make run_rv64_mc \
  VP_ARGS="--tlm-global-quantum=10 --use-dbbcache --tun-device tun10"
```

说明：`--tlm-global-quantum=10` 用于提高多 hart 在 LR/SC 短窗口内交错的概率。该配置启动较慢，本次命令在后续 MAG 测试打印途中被 `timeout` 终止，但 LR/SC mutex 测试已完成并打印 PASS。

## 测试结果

### VP 单元测试

直接运行：

```text
lrsc_bus_lock_test: PASS addr=0xefffe080 b_wait_ns=100
```

CTest：

```text
Test #6: lrsc_bus_lock ....................   Passed
100% tests passed, 0 tests failed out of 1
```

### Linux boot-time 测试

Linux 驱动输出：

```text
guivp_pbmt_test: LRSC mutex selftest PMA_AMO_LOGICAL phys=0x00000000efffe080 attempts=256 a_success=256 a_fail=0 b_access=256 b_old=0x6c727363828a95f8 PASS
```

同一地址的 ACE trace：

```text
READ  addr=0xefffe080 ... lr=1  aq=1 rl=1
WRITE addr=0xefffe080 ... sc=1  aq=1 rl=1
READ  addr=0xefffe080 ... amo=1 amo_op=or amo_phase=load  aq=1 rl=1
WRITE addr=0xefffe080 ... amo=1 amo_op=or amo_phase=store aq=1 rl=1
```

VP BusLock trace 捕捉到真实等待和恢复：

```text
[BusLock] hart=2 wait owner=3 time=10407400020 ns
[BusLock] hart=2 resume time=10407400070 ns
```

这说明 B hart 的同地址 AMO 访问确实在 A hart 持有 LR/SC BusLock 时进入，并等待到 A hart SC/unlock 后恢复。

## 判定

通过。

测试确认：

- A hart 持有 LR/SC bus lock 后，owner hart 自己访问不会等待。
- B hart 在 A hart 持锁期间调用 `wait_for_access_rights()` 会阻塞。
- B hart 只会在 A hart unlock/SC-release 之后恢复。
- Linux 侧真实 `lr.d/sc.d` 可以配合另一个 hart 的同地址 `amoor.d` 触发上述等待路径。

当前 VP 的 `BusLock` 是全局锁，不是按地址粒度或按属性粒度的锁。因此对“同样属性地址”的互斥等待来说，当前实现提供的是更强保证：只要一个 hart 持有 LR/SC lock，其他 hart 的 data memory transaction 都会等待，不限于同一地址。

## 和 Linux LR/SC 测试的关系

两类测试互补：

- [`LRSC_ACE_TEST_REPORT.md`](LRSC_ACE_TEST_REPORT.md)：证明真实 Linux `lr.d/sc.d` 能触发 LR/SC transaction，SC 成功时 VP trace 出现 `sc=1`，且 `PMA_RSRV_NONE` 会 fault。
- 本报告：证明 LR 建立的 VP bus lock 能阻塞另一个 hart 的访问，直到 owner hart 释放；并证明 Linux 侧真实双 hart 流程可以触发该等待。

合起来覆盖：

```text
真实 LR/SC 指令路径 + ACE 语义标记 + SC 成功写入 + PMA reservability + 跨 hart 互斥等待
```
