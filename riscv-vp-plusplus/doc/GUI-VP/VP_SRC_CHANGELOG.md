# VP src 变更记录

## 2026-04-14 增加 AMO 到 master-ACE 的语义标记

### 变更内容

- 新增 `tlm_ext_atomic`，用于在 CPU 发出的 `tlm_generic_payload` 中携带 AMO
  阶段、AMO op、PMA AMO class 和 `aq/rl` 位。
- RV32/RV64 AMO helper 改为调用 `set_next_amo()`，把 `AMOSWAP/AMOADD/AMOOR`
  等具体操作传给 memory interface。
- `CombinedMemoryInterface_T` 在 AMO load/store 两个半事务上挂载 atomic extension；
  普通 load/store 保持 `phase=None`。
- `master-ace` trace 增加 AMO 字段，便于 Linux 自测时观察 AMO 是否进入 ACE 路径。
- `master-ace` 下游 ACE trace 增加 `snoop/domain/barrier` 字段，便于确认 AMO
  miss 是否转换成 `AR::ReadUnique`。
- `cache-ace` 在 AMO load 命中 shared line 时先获取 unique 权限；AMO load miss 时
  走 `ReadUnique`，避免把 AMO RMW 当成普通 shared read。
- RV64 Linux VP 增加 `PMA_AMO_LOGICAL` 单页桥接测试入口：普通 load/store 直达
  memory，带 `tlm_ext_atomic` 的 AMO transaction 进入 `CacheAceMaster`。
- RV64 Linux ACE memory bridge 在下游 SimpleMemory 没有显式设置 response 时补
  `TLM_OK_RESPONSE`，满足 `cache_ace::read_unique()` 填线条件。
- 新增 `master_ace_amo_test`，直接向 `cache_ace` 注入 AMO load gp，并记录下游
  ACE snoop 类型，覆盖 AMO miss `ReadUnique` 和 shared hit `CleanUnique` 两条路径。

### 变更原因

之前 AMO 在 VP 内部依赖 `bus_lock` 保证功能原子性，但进入 master-ACE/cache-ACE
后只表现为普通 read 加普通 write，下游无法知道这两笔事务属于同一个 RMW。新增
atomic extension 后，第一阶段先把 AMO 身份传递到 ACE cache 入口，并按设计文档
要求让 cacheable AMO 优先获取 unique line。

### 验证结果

- `git -C riscv-vp-plusplus diff --check`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux32-mc-vp -j$(nproc)`：通过。
- RV64 Linux 默认参数启动：通过；内核 probe 自测 `PMA/NC/IO/PMA_DENY/PMA_AMO_LOGICAL/PMA_MAG16/PMA_RSRV_NONE`
  均通过，其中 AMO selftest 显示 `amoor_ret=0`、`amoadd_ret=-14`。
- RV64 Linux 用户态 `guivp_pbmt_test all`：通过；`PMA/NC/IO` 三段映射读写均为 PASS。
- RV64 Linux 关闭 `use-lscache` 的慢路径启动：通过；内核 probe 自测再次覆盖
  `PMA_AMO_LOGICAL/PMA_DENY/PMA_MAG16/PMA_RSRV_NONE` 并通过。
- `cmake --build riscv-vp-plusplus/vp/build --target master_ace_amo_test -j$(nproc)`：通过。
- `riscv-vp-plusplus/vp/build/bin/master_ace_amo_test`：通过，输出 `master_ace_amo_test: PASS`。
- `ctest --test-dir riscv-vp-plusplus/vp/build -R master_ace_amo --output-on-failure`：通过。
- RV64 Linux 原生 AMO 到 master-ACE 路径：通过。命令使用
  `RVVP_ACE_TRACE_BUDGET=40 RVVP_ACE_TRACE_START=0xefffe000 RVVP_ACE_TRACE_END=0xefffefff make run_rv64_mc ...`；
  trace 观察到 `amo=1 amo_op=or amo_phase=load`、下游 `snoop=0x7`，
  随后 `amo_phase=store`，内核 probe 打印 `AMO selftest ... PASS` 并启动到
  Buildroot login。

## 2026-04-08 入库 VNC 启动保护逻辑

### 变更内容

- `VNCServer::start()` 在 `vncPort == 0` 时直接返回 `false`，表示 VNC 端口被禁用时不创建 rfb screen。
- `VNCSimpleFB::updateProcess()` 在 `vncServer.start()` 返回失败时直接退出，避免继续访问不存在的 rfb screen。
- `VNCServer::isActive()` 增加 `rfbScreen != nullptr` 判断，避免未启动或已停止状态下调用 `rfbIsActive(nullptr)`。

### 变更原因

`vp/src` 下存在未入库的 VNC 防护改动。该改动让 VNC 端口为 0 或 VNC screen 尚未创建时的行为更明确，避免后续 framebuffer 更新流程访问空指针。

### 验证结果

- `git -C riscv-vp-plusplus diff --check`：通过。
- `cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)`：通过。
