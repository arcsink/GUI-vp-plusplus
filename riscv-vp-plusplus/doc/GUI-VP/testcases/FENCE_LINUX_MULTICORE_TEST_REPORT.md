# Linux 多核 Fence 综合测试报告

本文档记录 2026-04-16 在 GUI-VP RV64 多核 Linux 平台上，对 `fence rw,rw`、`fence.tso`
和 `fence.i` 进行的一轮综合验证。

## 1. 对应变更

本轮测试对应的设计文档补充了“本次代码变更说明”章节，位置：

- `doc/GUI-VP/design/RISCV_FENCE_DESIGN.md`

本轮代码状态的要点是：

1. `FENCE` 已把 `pred/succ/fm` 传入 `lscache.fence(...)`
2. `FENCE.TSO` 已通过 `fm=0b1000` 单独区分
3. `lscache` 已有 store-buffer / queue、drain 和 load-forwarding 基础机制
4. `FENCE.I` 已接到本 hart `dbbcache` coherence update
5. 仍未实现全局 TSO、remote 自动 shootdown、完整 non-blocking LSU

因此，本轮 Linux 测试的目标不是证明“全局 TSO 已完成”，而是验证：

- `fence rw,rw`
- `fence.tso`
- `fence.i`

在当前实现状态下，是否已经具备预期的本地/多核可观测行为。

## 2. 测试资产

测试源码：

- `doc/GUI-VP/testcases/src/guivp_fence_linux_suite.c`

原始运行日志：

- `doc/GUI-VP/testcases/results/guivp_fence_linux_suite_2000.log`

## 3. 测试内容

### 3.1 SB(store buffering) 数据有序性

两线程分别执行：

- 线程 0：`x = 1; [optional fence]; r0 = y;`
- 线程 1：`y = 1; [optional fence]; r1 = x;`

按三种模式统计 `r0 == 0 && r1 == 0`：

1. `no-fence`
2. `fence rw,rw`
3. `fence.tso`

这里的判定标准是：

- `fence rw,rw` 不能出现 `both_zero`
- `fence.tso` 不能出现 `both_zero`
- `no-fence` 是否出现 `both_zero` 只作信息项，因为当前 VP 仍偏保守

### 3.2 本 hart `fence.i`

单线程在可执行匿名页上做自修改代码：

1. 先生成一段返回 `1` 的代码并执行 warm-up
2. 把代码 patch 为返回 `2`
3. 不执行 `fence.i` 直接再次执行
4. 再执行 `fence.i`
5. 第三次执行

期望：

- 不执行 `fence.i` 时，当前 hart 仍可能看到旧指令
- 执行 `fence.i` 后，应看到新指令

### 3.3 多 hart `fence.i` 指令可见性

使用两线程共享同一段可执行代码页：

1. consumer hart 先 warm-up，缓存旧代码块
2. producer hart patch 指令
3. producer 用数据侧 fence 发布同步点
4. consumer 在两种条件下执行代码：
   - 不执行本 hart `fence.i`
   - 执行本 hart `fence.i`

这里拆成两组发布方式：

1. producer 用 `fence rw,rw` 发布
2. producer 用 `fence.tso` 发布

期望：

- consumer 不执行本 hart `fence.i` 时，不能把“看到新代码”当成保证
- consumer 执行本 hart `fence.i` 后，应看到新代码

## 4. 构建与运行

交叉编译：

```bash
buildroot_rv64/output/host/bin/riscv64-buildroot-linux-gnu-gcc \
  -O2 -pthread -Wall -Wextra \
  -o /tmp/guivp_fence_test/guivp_fence_linux_suite \
  riscv-vp-plusplus/doc/GUI-VP/testcases/src/guivp_fence_linux_suite.c
```

Linux 启动命令：

```bash
env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
  -u all_proxy -u ALL_PROXY \
  make run_rv64_mc
```

guest 中执行：

```bash
/data/guivp_fence_linux_suite 2000
```

## 5. 实际结果

本轮实际输出如下：

```text
[guivp-fence-suite] cpu_count=4 sb_iterations=2000
[guivp-fence-suite] sb mode=no-fence iterations=2000 both_zero=0 elapsed_sec=8.779364
[guivp-fence-suite] sb mode=fence-rw-rw iterations=2000 both_zero=0 elapsed_sec=8.405359
[guivp-fence-suite] sb mode=fence-tso iterations=2000 both_zero=0 elapsed_sec=9.385280
[guivp-fence-suite] fencei-local warm=1 no_fence_ret=1 with_fence_i_ret=2
[guivp-fence-suite] fencei-remote label=no-remote-fence-i warmup=1 consumer_fence_i=0 result=1 expected=1
[guivp-fence-suite] fencei-remote label=remote-fence-i-rw-rw-publish warmup=1 consumer_fence_i=1 result=3 expected=3
[guivp-fence-suite] fencei-remote label=remote-fence-i-tso-publish warmup=1 consumer_fence_i=1 result=4 expected=4
[guivp-fence-suite] summary sb_no_fence_both_zero=0 sb_fence_rwrw_both_zero=0 sb_fence_tso_both_zero=0 failures=0
[guivp-fence-suite] RESULT=PASS note=sb-no-fence-did-not-hit-both-zero-in-this-run
```

## 6. 结果解释

### 6.1 `fence rw,rw`

`fence rw,rw` 模式下 `both_zero=0`，说明当前实现里：

- write-side queue / store buffer 在这条 fence 上至少没有失效
- 多核 Linux 用户态下没有观察到被 fence 保护后仍然发生的 SB 违例

同时，本轮 remote `fence.i` 用例里，producer 用 `fence rw,rw` 发布 patch 后，
consumer 在执行本 hart `fence.i` 后看到了新代码 `3`，说明当前 `fence rw,rw`
可以作为这轮测试中的数据发布点使用。

### 6.2 `fence.tso`

`fence.tso` 模式下同样 `both_zero=0`，说明当前 VP 至少已经把这条指令接到了
独立的执行路径，并且没有弱化掉需要的本地 write-side ordering。

更重要的是，remote `fence.i` 测试里，producer 用 `fence.tso` 发布 patch 后，
consumer 执行本 hart `fence.i`，最终看到了新代码 `4`。这说明当前代码里的
`fm=0b1000` 路径已经可以参与多核软件同步场景，而不只是 decode 占位。

但仍需强调：

- 这不等于“已经实现全局 TSO”
- 这里只能说明当前已有 write-side queue 上，`fence.tso` 没有明显失效

### 6.3 本 hart `fence.i`

本 hart 自修改代码结果是：

- `no_fence_ret=1`
- `with_fence_i_ret=2`

这说明当前 `dbbcache.fence_i(pc)` 的 coherence update 是真实可观测的：

- 不执行 `fence.i` 时，同一 hart 继续执行了旧 basic block
- 执行 `fence.i` 后，本 hart 重新取到了新指令

### 6.4 多 hart `fence.i`

remote 指令可见性结果是：

1. `no-remote-fence-i`：
   - consumer 结果是 `1`
   - 即 producer 已 patch 代码，但 consumer 不执行本 hart `fence.i` 时，
     仍继续看到了旧指令

2. `remote-fence-i-rw-rw-publish`：
   - consumer 结果是 `3`
   - producer 用 `fence rw,rw` 发布后，consumer 执行本 hart `fence.i`，
     成功观察到新代码

3. `remote-fence-i-tso-publish`：
   - consumer 结果是 `4`
   - producer 用 `fence.tso` 发布后，consumer 执行本 hart `fence.i`，
     同样成功观察到新代码

这组三个结果正好给出了“被观察”和“不被观察”两类场景：

- 不执行 remote hart 自身 `fence.i`：新指令不被保证观察到
- remote hart 执行自身 `fence.i`：新指令被观察到

这和设计文档中的边界是一致的：`FENCE.I` 只保证本 hart；多 hart 需要 remote hart
自己参与协议。

## 7. 结论

本轮 Linux 多核综合验证结论如下：

1. `fence rw,rw`：通过
2. `fence.tso`：通过
3. 本 hart `fence.i`：通过
4. 多 hart `fence.i` 的“看见 / 看不见新代码”边界：通过

同时也要保留一个保守结论：

- `SB no-fence both_zero=0` 说明当前 Linux 用户态下，VP 仍偏强、偏保守
- 因此本轮报告证明的是“fence 相关路径已经可观测且未失效”
- 不是“当前 VP 已经稳定对软件暴露出完整 RVWMO/TSO 级乱序窗口”

## 8. 后续建议

如果下一步要继续收紧到第二阶段目标，建议继续补三类回归：

1. bare-metal SB / LB / MP litmus
2. kernel-space remote `fence.i` / `sfence.vma` 协议测试
3. trace 级验证，确认 `pred/succ/fm` 与 queue drain 事件一一对应
