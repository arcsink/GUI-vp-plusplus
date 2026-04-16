# Linux 用户态 Fence / Store-Buffer 测试报告

本文档记录当前 GUI-VP 状态下，基于 RV64 Linux 多核 VP 的 fence/store-buffer 用户态测试结果。

## 1. 测试目标

本轮测试不验证全部 RISC-V memory model，只验证当前实现中最直接相关的一点：

- Linux 用户态两线程 `store -> load` 场景能否观察到典型 SB(store buffering) 结果。
- 插入 `fence rw,rw` 后，当前 VP 是否仍保持正确，不出现 fence 失效。

本轮测试对应源码：

- `doc/GUI-VP/testcases/src/guivp_fence_sb_litmus.c`

## 2. 测试方法

测试程序在 RV64 Linux 用户态启动两个 pthread，并尽量固定到不同 CPU：

- 线程 0：`x = 1; [optional fence rw,rw]; r0 = y;`
- 线程 1：`y = 1; [optional fence rw,rw]; r1 = x;`

每轮结束后统计：

- `both_zero`：即 `r0 == 0 && r1 == 0`

程序顺序执行两种模式：

- `mode=no-fence`
- `mode=fence-rw-rw`

判定规则：

1. `fence-rw-rw` 模式下如果出现 `both_zero != 0`，记为失败。
2. `no-fence` 模式下如果出现 `both_zero > 0`，说明当前实现已经暴露出可观察的 store->load 松弛。
3. `no-fence` 模式下如果 `both_zero == 0`，不能说明 fence 无意义，只能说明这轮 Linux 用户态测试没有观测到可见乱序窗口。

## 3. 构建与投放

交叉编译：

```bash
buildroot_rv64/output/host/bin/riscv64-buildroot-linux-gnu-gcc \
  -O2 -pthread -Wall -Wextra \
  -o /tmp/guivp_fence_test/guivp_fence_sb_litmus \
  riscv-vp-plusplus/doc/GUI-VP/testcases/src/guivp_fence_sb_litmus.c
```

测试程序投放到 data 镜像 `runtime_mram/mram_rv64_data.img`，guest 挂载后路径为：

```text
/data/guivp_fence_sb_litmus
```

## 4. 运行环境

- 日期：2026-04-15
- 平台：`make run_rv64_mc`
- Linux：`6.15.2`
- OpenSBI：`v1.6`
- HART 数：4
- VP 启动参数：

```text
--use-data-dmi --tlm-global-quantum=1000000 --use-dbbcache --use-lscache --tun-device tun10
```

## 5. 实际执行

在 guest 中执行：

```bash
/data/guivp_fence_sb_litmus 20000
```

得到：

```text
[guivp-fence-sb] cpu_count=4 iterations=20000
[guivp-fence-sb] mode=no-fence iterations=20000 both_zero=0 elapsed_sec=99.454224
[guivp-fence-sb] mode=fence-rw-rw iterations=20000 both_zero=0 elapsed_sec=100.487633
[guivp-fence-sb] RESULT=PASS note=no-fence-did-not-hit-both-zero-in-this-run
```

随后为了留档，又执行一轮较短测试：

```bash
/data/guivp_fence_sb_litmus 2000 > /data/guivp_fence_sb_run_2000.log 2>&1
```

日志文件内容见：

- `doc/GUI-VP/testcases/results/guivp_fence_sb_run_2000.log`

短跑结果：

```text
[guivp-fence-sb] cpu_count=4 iterations=2000
[guivp-fence-sb] mode=no-fence iterations=2000 both_zero=0 elapsed_sec=9.784484
[guivp-fence-sb] mode=fence-rw-rw iterations=2000 both_zero=0 elapsed_sec=10.394945
[guivp-fence-sb] RESULT=PASS note=no-fence-did-not-hit-both-zero-in-this-run
```

## 6. 结果解释

本轮结论是：

1. 当前实现下，`fence rw,rw` 模式没有观察到错误结果，说明 fence 路径至少没有出现明显失效。
2. 当前 Linux 用户态测试下，`no-fence` 也没有观察到 `both_zero`，所以这轮测试没有证明“当前 VP 已经对软件暴露出可观测的 store-buffer 乱序窗口”。

这和前面代码分析是一致的：当前虽然已经引入了 store buffer / queue 形态，但还没有形成完整的非阻塞 LSU / load queue / 指令级 pipeline 竞争窗口。对 Linux 用户态 SB litmus 来说，现阶段更像是：

- fence 语义入口已经接上；
- 但 no-fence 场景下，尚未稳定暴露出软件可观测的典型 SB 结果。

因此，严格表述应当是：

- 当前测试验证了“fence 路径未失效”；
- 但还没有验证出“无 fence 时一定能观察到 store->load 松弛”。

## 7. 对当前实现的意义

这份结果说明，当前代码已经具备下面两点：

1. 可以在 Linux 用户态真实执行 `fence rw,rw` 指令，并经过当前 VP 的 ISA + cache/memory 路径。
2. 可以用同一个用户态用例持续回归后续 fence/store-buffer 改动。

但它也说明，下一阶段如果目标是“把第二阶段 queue/buffer 语义真正对软件可见”，还需要继续补：

- 更明确的非阻塞 load/store overlap 时序；
- load queue / issue window；
- 更可观测的跨 hart store visibility 延迟模型；
- 或更强的内核态 / bare-metal litmus 来减小 Linux 调度噪声。

## 8. 结论

本轮 Linux 测试结论为：

- `PASS`，因为 `fence-rw-rw` 模式未观察到错误结果。
- 但结果是“保守通过”，不是“已经证明 no-fence 可稳定乱序”。

后续如果要继续逼近设计文档中第二阶段目标，建议把这份 litmus 保留为回归用例，并再补一组更激进的 bare-metal / kernel-space SB、LB、MP 测试。
