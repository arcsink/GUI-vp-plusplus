# PMA 变更记录

## 2026-04-09 增加 Misaligned Atomicity Granule Linux 自测

### 变更内容

- Linux 平台 `cache_ace` PMA 窗口新增 `PMA_MAG16` 测试页，物理地址为 `0xEFFFC000-0xEFFFCFFF`，该页继承普通 main-memory AMO/LRSC 属性，并额外设置 `misaligned_atomicity_granule = 16`。
- RV64 AMO 执行路径移除 ISS 入口处对 `AMO*.W` / `AMO*.D` 的硬性对齐拦截，改为让 misaligned AMO 继续进入 memory interface，由 PMA 的 `is_within_misaligned_atomicity_granule()` 统一决定是否允许。
- Linux `guivp_pbmt_test` 启动自测新增 `PMA_MAG16` 验证：先在 `addr + 4` 执行 misaligned `amoadd.d.aqrl`，要求完整落在同一个 16-byte granule 内并成功；再在 `addr + 12` 执行 misaligned `amoadd.d.aqrl`，要求跨 granule 并通过 nofault/fixup 路径返回 `-EFAULT`。
- `linux_patches/0003-misc-add-guivp-pbmt-test-driver.patch` 的 `drivers/misc/Makefile` hunk 更新为匹配当前 Linux 6.15.2 源树中的 `lan966x-pci-objs` 上下文，确保 Buildroot `linux-dirclean` 后可以重新打补丁。

### 变更原因

虽然 PMA 层已经实现了 `is_within_misaligned_atomicity_granule()`，但之前 RV64 AMO 在 ISS 入口就会因为地址未对齐直接抛 `STORE/AMO_ADDR_MISALIGNED`，导致 Linux 实际运行路径根本到不了 MAG PMA 检查。把 AMO 的对齐放行到 PMA 后，Linux 自测才能真正区分“同 granule 的 misaligned atomic 允许”和“跨 granule 的 misaligned atomic 拒绝”。

### 验证结果

- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
- `make -C buildroot_rv64 linux-dirclean linux-rebuild`：通过，并确认重新编译 `drivers/misc/guivp_pbmt_test.o`。
- `env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY make run_rv64_mc`：Linux 启动到 `buildroot login:`。
- Linux 日志确认 `PMA_MAG16` 自测通过：`guivp_pbmt_test: MAG selftest PMA_MAG16 phys=0x00000000efffc000 ok_ret=0 cross_ret=-14 PASS`。
- Linux 日志确认 `PMA_MAG16` 页已被平台正确公布：`guivp_pbmt_test: reserved window base=0x00000000e0000000 size=0x0000000020000000 PMA=0x0-0xfffbfff PMA_MAG16=0xfffc000-0xfffcfff PMA_RSRV_NONE=0xfffd000-0xfffdfff PMA_AMO_LOGICAL=0xfffe000-0xfffefff PMA_DENY=0xffff000-0xfffffff NC=0x10000000-0x17ffffff IO=0x18000000-0x1fffffff`。
- Linux 日志确认既有 PMA 自测仍保持通过：`PMA_DENY ... PASS`、`PMA_AMO_LOGICAL ... PASS`、`PMA_RSRV_NONE ... PASS`。

## 2026-04-08 增加 PMA Reservability 检查流程

### 变更内容

- `pma_check_request` 新增 `reservability` 字段，用于描述 LR/SC 访问需要的最低 reservability 能力。
- `PMA::check_access()` 对 LR/SC 访问改为使用 `supports_reservability()` 做覆盖关系判断，而不是只判断 `RsrvNone`。
- `CombinedMemoryInterface_T::_atomic_store_conditional_data()` 在判断 reservation 是否命中之前，先对 SC 目标地址执行 PMA reservability 检查，避免 SC 因无有效 reservation 直接返回失败而绕过 PMA check。
- VP Linux 平台新增 `PMA_RSRV_NONE` 测试页，物理地址为 `0xEFFFD000-0xEFFFDFFF`，该页普通读写和 AMO 仍允许，但 `reservability` 设置为 `RsrvNone`。
- Linux `guivp_pbmt_test` 启动自测新增 `lr.d.aqrl` 和 `sc.d.aqrl` nofault 访问 `PMA_RSRV_NONE` 页，期望两者都触发 PMA access-fault 并返回 `-EFAULT`。

### 变更原因

之前 PMA 对 LR/SC 的 reservability 检查只覆盖 LR 实际 load 路径；SC 如果没有有效 reservation，可能在 memory interface 中直接返回失败，从而没有进入 PMA check。增加显式 reservability 请求和 SC 前置检查后，LR 与 SC 都会先确认目标物理区域是否支持 reservation，后续如果需要区分 `RsrvNonEventual` 和 `RsrvEventual`，也可以通过同一套覆盖关系继续扩展。

### 验证结果

- `make -C buildroot_rv64 linux-rebuild`：通过，并确认重新编译 `drivers/misc/guivp_pbmt_test.o`。
- `buildroot_rv64/output/build/linux-6.15.2/scripts/checkpatch.pl --no-tree --strict linux_patches/0003-misc-add-guivp-pbmt-test-driver.patch`：通过，0 errors / 0 warnings。
- 临时 C++17 PMA reservability 行为测试：`RsrvNone` 拒绝 `RsrvNonEventual` 请求，`RsrvNonEventual` 允许自身但拒绝 `RsrvEventual` 请求，`RsrvEventual` 允许 `RsrvEventual` 请求。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux32-sc-vp -j$(nproc)`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-cheriv9-sc-vp -j$(nproc)`：通过。
- `timeout 120s env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY make run_rv64_mc`：Linux 启动到 `Welcome to Buildroot` / `buildroot login:`。
- Linux 日志确认 `PMA_RSRV_NONE` 自测通过：`guivp_pbmt_test: reservability selftest PMA_RSRV_NONE phys=0x00000000efffd000 lr_ret=-14 sc_ret=-14 PASS`。
- Linux 日志确认原有 AMO class 与 PMA_DENY 自测仍通过：`PMA_AMO_LOGICAL ... amoadd_ret=-14 ... PASS`，`PMA_DENY ... read_ret=-14 write_ret=-14 PASS`。

## 2026-04-08 增加 Linux AMO class 异常触发验证

### 变更内容

- VP 侧在 PMA/PBMT/ACE 测试窗口中新增 `PMA_AMO_LOGICAL` 测试页，物理地址为 `0xEFFFE000-0xEFFFEFFF`，该页保持普通读写权限，但 `amo_class` 限制为 `AMOLogical`。
- Linux `guivp_pbmt_test` 驱动新增启动自测：先在 `PMA_AMO_LOGICAL` 页执行 `amoor.d.aqrl`，期望成功；再执行 `amoadd.d.aqrl`，期望通过 exception table / nofault 路径返回 `-EFAULT`。
- 原有 `PMA_DENY` 页继续保留在 `0xEFFFF000-0xEFFFFFFF`，用于验证普通 load/store PMA access-fault。

### 变更原因

之前 AMO class 检查只完成了 VP 侧逻辑和 C++ 层行为测试，还没有在 Linux 启动路径中实际执行 RISC-V AMO 指令来触发异常。新增 `PMA_AMO_LOGICAL` 页后，可以验证“logical AMO 允许、arithmetic AMO 拒绝”的 PMA check 语义，并确认异常能被 Linux nofault/fixup 机制正常接住，不影响系统启动。

### 验证结果

- `make -C buildroot_rv64 linux-rebuild`：通过，并确认重新编译 `drivers/misc/guivp_pbmt_test.o`。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
- `timeout 120s env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY make run_rv64_mc`：Linux 启动到 `Welcome to Buildroot` / `buildroot login:`。
- Linux 日志确认 `PMA_AMO_LOGICAL` 自测通过：`guivp_pbmt_test: AMO selftest PMA_AMO_LOGICAL phys=0x00000000efffe000 amoor_ret=0 amoadd_ret=-14 ... PASS`。
- Linux 日志确认原有 `PMA_DENY` 自测仍通过：`guivp_pbmt_test: fault selftest PMA_DENY phys=0x00000000effff000 read_ret=-14 write_ret=-14 PASS`。

## 2026-04-08 增加 AMO class PMA check

### 变更内容

- `pma_check_request` 新增 `amo_class` 字段，用于描述当前 AMO 指令需要的 PMA AMO 能力级别。
- `PMA::check_access()` 在 `atomic=true` 且不是 LR/SC 时，按 `AMONone < AMOSwap < AMOLogical < AMOArithmetic` 的递增能力关系检查 `attributes.amo_class` 是否覆盖请求的 AMO class。
- LR/SC 不复用 AMO class 检查，仍然通过 `PmaReservability` 判断该物理区域是否支持 reservation。
- `data_memory_if` 的 AMO load/store 接口新增 `PmaAmoClass` 参数，RV32/RV64 ISS 在执行 AMO 指令时传入对应 class：`AMOSWAP` 使用 `AMOSwap`，`AMOXOR/AMOAND/AMOOR` 使用 `AMOLogical`，`AMOADD/AMOMIN/AMOMINU/AMOMAX/AMOMAXU` 使用 `AMOArithmetic`。
- CHERI 相关 memory interface 和 AMO helper 同步接收 `PmaAmoClass` 参数，当前 CHERI 路径仍保持原有内存检查行为，只做接口一致性预留。

### 变更原因

之前 PMA check 只判断 `attributes.amo_class == AMONone` 时拒绝 AMO，无法区分“只支持 swap”“支持 logical 但不支持 arithmetic”等更细的 AMO PMA 能力。按照 PMA AMO 能力的递增模型，PMA check 应该根据当前具体 AMO 指令需要的 class 做覆盖关系检查，这样后续配置某个 region 的 `amo_class` 时才能真实影响 AMO 访问结果。

### 验证结果

- `rg -n "execute_amo_w\\(instr, \\[|execute_amo_d\\(instr, \\[" riscv-vp-plusplus/vp/src/core`：无旧 AMO helper 调用点残留。
- `git -C riscv-vp-plusplus diff --check`：通过。
- 临时 C++17 PMA AMO class 行为测试：`AMOLogical` 允许 `AMOSwap/AMOLogical`，拒绝 `AMOArithmetic`；LR/SC 在 `RsrvNone` 下拒绝，在 `RsrvEventual` 下允许。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux32-sc-vp -j$(nproc)`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-cheriv9-sc-vp -j$(nproc)`：通过。

## 2026-04-08 移除 AMOCAS* PMA 分类

### 变更内容

- 从 `PmaAmoClass` 中移除 `AMOCASW`、`AMOCASD`、`AMOCASQ`。
- `PmaAmoClass` 现在只保留 `AMONone`、`AMOSwap`、`AMOLogical`、`AMOArithmetic`。
- 更新 `PmaAmoClass` 注释，说明 AMO PMA 按规范 3.6.3.1 使用递增能力分类，CAS 不作为并列 PMA class 建模。

### 变更原因

`AMOCASW`、`AMOCASD`、`AMOCASQ` 表示具体 CAS 宽度能力，把它们放在 `PmaAmoClass` 中会让它们看起来与 `AMOSwap`、`AMOLogical`、`AMOArithmetic` 并列。按照当前 PMA 模型，`PmaAmoClass` 更适合表达规范 3.6.3.1 中递增的 AMO 支持级别；严格意义上，CAS 不应在这里作为独立并列分类。后续如果需要对 CAS 做更细控制，应新增单独字段或按具体 AMO 指令传入 memory interface 后再细分。

### 验证结果

- `rg -n "AMOCAS" riscv-vp-plusplus`：无残留引用。
- `git -C riscv-vp-plusplus diff --check`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
