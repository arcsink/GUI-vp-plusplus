# PMA/PBMT 到 ACE cache-control 流程测试方法

本文档记录 GUI-VP 中验证 PMA 属性、PTE PBMT 属性以及 master-ACE cache-control 联通流程的方法。

## 测试目标

确认 Linux 访问 cache-ace reserved-memory 窗口时，VP 能够完成如下流程：

1. Linux 驱动访问 reserved-memory 测试窗口。
2. MMU 从 PTE 中解析 PBMT 属性，并在内存访问时携带该属性。
3. PMA 根据物理地址解析 memory 属性，例如 `MainMemory`、`cacheable`、`coherent`。
4. VP 将 PTE PBMT 和 PMA 属性一起写入 TLM extension。
5. master-ACE 根据合并后的属性设置 ACE cache-control，例如 `domain`、`bufferable`、`modifiable`、`read_allocate`、`write_allocate`。
6. coherent memory 访问进入 `master-ace -> coherent L2 -> bus/memory` 路径。

当前正向测试把 cache-ace reserved-memory 窗口拆成三个固定分段：`0xE0000000-0xEFFFFFFF` 使用 PTE PBMT `PMA` 并由 PMA 决定为 coherent main memory，`0xF0000000-0xF7FFFFFF` 使用 PTE PBMT `NC`，`0xF8000000-0xFFFFFFFF` 使用 PTE PBMT `IO`。期望结果是：PMA 分段最终 `ace_cacheable=1` 并参与一致性；NC/IO 分段最终 `ace_cacheable=0`，走 no-snoop / non-shareable 路径。

## 相关代码位置

- `vp/src/platform/linux/linux_main.cpp`：配置 cache-ace DRAM 窗口和 PMA 属性。
- `vp/src/platform/linux/dt/linux-dt-gen.py`：在 RV64 设备树中声明 `svpbmt`，让 Linux 合法生成 NC/IO PBMT PTE。
- `vp/src/core/common/mmu.h`：解析 PTE PBMT，并在生成物理地址时从 PPN 中排除 PBMT 属性位。
- `vp/src/core/common/mem.h`：完成 PMA 检查，并把 PMA + PBMT 属性写入 TLM extension。
- `vp/src/util/tlm_ext_pbmt.h`：TLM extension，携带 PBMT 和 PMA cache-control 摘要。
- `ext/libsystemctlm-soc/tlm-modules/master-ace.h`：根据 PMA + PBMT 合并结果设置 ACE `genattr`。
- `ext/libsystemctlm-soc/tlm-modules/cache-ace.h`：根据 `genattr` 决定走 coherent cache 路径或 no-snoop 路径。
- `ext/lib_ace_l2_memory/ace_l2_memory_subsystem.h`：coherent L2 根据合并后的属性判断事务是否参与一致性。
- `buildroot_rv64/output/build/linux-6.15.2/drivers/misc/guivp_pbmt_test.c`：Linux 侧测试驱动源码。

## 代码流程

PBMT 和 PMA 是两条来源不同、最终在 TLM/ACE 路径上合并的属性链路：

1. Linux 侧生成 PTE PBMT：`guivp_pbmt_test.c` 根据访问 offset 选择分段。PMA 分段保持默认 `PAGE_KERNEL`，NC 分段使用 `pgprot_writecombine()`，IO 分段使用 `pgprot_noncached()`。前提是 RV64 device tree 中声明了 `svpbmt`，否则 Linux 不会把 NC/IO PBMT 位作为合法 PTE 属性使用。
2. VP 侧配置 PMA：`linux_main.cpp` 在平台初始化时把 `cache_ace_dram_window` 设置为 PMA region，属性为 `MainMemory + RVWMO + cacheable + coherent`，并显式配置读、写、执行访问宽度。这个配置通过 `memif.get_pma().set_region(..., MachineMode)` 完成，语义上只允许 M Mode 配置。
3. MMU 翻译虚拟地址：`mmu.h` 在 page walk 中读取 PTE，使用 `pte.PBMT()` 解析 PTE[62:61]，并记录到 `last_translation_pbmt`。构造物理地址时必须先从 PTE 中清掉 `PTE_PBMT`，否则 NC/IO 位会被误当成 PPN 高位。
4. Memory interface 获取两类属性：`mem.h` 在 `_load_data()` / `_store_data()` 中先调用 `v2p()` 得到物理地址，再用 `get_last_translation_pbmt()` 获取刚才翻译出的 PBMT。随后 `_raw_load_data()` / `_raw_store_data()` 对物理地址调用 `check_pma_or_raise()`，拿到该 region 的 PMA 属性。
5. PMA check 调用逻辑：`check_pma_or_raise()` 构造 `pma_check_request`，填入 `paddr`、`size`、`access_type`、`atomic`、`lr_sc`、`page_table_access`、`implicit`，再调用 `pma.check_access()`。`pma.check_access()` 先用 `find_region()` 命中 region，未命中时使用默认属性，然后检查 size 是否为 0、地址范围是否溢出、读/写/执行宽度是否支持、misaligned 是否允许、页表隐式访问是否允许、AMO/LRSC 是否允许，以及隐式访问非幂等区域是否安全。
6. PMA check 的异常行为：如果 `pma.check_access()` 返回 `allowed=false`，`check_pma_or_raise()` 会根据 PMA region 的 `failure_response` 区分响应类型。默认 `PreciseAccessFault` 对应规范 3.6 中可精确 trap 的 PMA violation：取指为 `EXC_INSTR_ACCESS_FAULT`，读为 `EXC_LOAD_ACCESS_FAULT`，写或 AMO 为 `EXC_STORE_AMO_ACCESS_FAULT`。页表遍历中的隐式 PTE 访问也不是 page fault，而是把 `MMU::walk()` 传入的原始访问类型和原始虚拟地址用于报告对应 access-fault。对于 legacy bus/device 一类不能精确 trap 的平台场景，PMA region 可以配置为 `BusError` 或 `Timeout`，当前 VP 会把两类响应作为不同的仿真错误路径保留下来，后续可继续接入具体 bus-error 中断或 timeout 事务模型。如果通过检查，`pma_check_result.attributes` 会继续传给 TLM transaction。
7. 写入 TLM extension：`mem.h` 在 `_do_transaction()` 前设置 `tlm_ext_pbmt`。`ext_pbmt->pbmt` 保存 PTE PBMT，`ext_pbmt->set_pma(...)` 保存 PMA 摘要。PMA 摘要由 `pma_memory_type_to_tlm()` 和 `pma_ordering_to_tlm()` 转换为 TLM 可携带的枚举，同时保留 `cacheable` 与 `coherent` 布尔值。
8. TLM extension 字段含义：`tlm_ext_pbmt` 携带 `pbmt`、`pma_memory_type`、`pma_ordering`、`pma_cacheable`、`pma_coherent`。这些字段随着同一个 `tlm_generic_payload` 传到 master-ACE，成为后续 ACE cache-control 的输入。
9. 合并规则：`tlm_ext_pbmt::ace_cacheable()` 使用保守合并策略。它先用 `pte_forces_uncached()` 判断 PBMT 是否为 `NC/IO/RESERVED`，再用 `pma_allows_ace_cache()` 判断 PMA 是否为 `MainMemory + cacheable + coherent`。只有 PTE 没有强制 uncached 且 PMA 允许 ACE cache，结果才是 `true`。
10. 参数传递到 cache-control：`master-ace.h` 的 `b_transport_cpu()` 从 transaction 中读取 `tlm_ext_pbmt`，并先创建或复用 `genattr_extension`。默认先设置 `bufferable=true`、`modifiable=true`、`read_allocate=true`、`write_allocate=true`、`domain=Domain::Inner`；如果 `pbmt_ext && !pbmt_ext->ace_cacheable()`，就把这些 cache-control 字段改成 `bufferable=false`、`modifiable=false`、`read_allocate=false`、`write_allocate=false`、`domain=Domain::NonSharable`。
11. trace 字段来源：master-ACE trace 中的 `pbmt`、`pma_mem`、`pma_cacheable`、`pma_coherent` 直接来自 `tlm_ext_pbmt`；`ace_cacheable` 来自 `tlm_ext_pbmt::ace_cacheable()`；`domain`、`bufferable`、`modifiable`、`rd_alloc`、`wr_alloc` 来自已经设置好的 `genattr_extension`。
12. cache/L2 执行路径：`cache-ace.h` 和 `ace_l2_memory_subsystem.h` 根据 ACE `genattr` 和 `tlm_ext_pbmt` 判断是否按 coherent cacheable 事务处理。PMA 分段应表现为 cacheable/coherent；NC/IO 分段虽然仍经过 master-ACE 观察路径，但 `ace_cacheable=0`，不能按 coherent cacheable 事务处理。

当前固定分段对应关系如下：

- `0xE0000000-0xEFFFFFFF`：PTE PBMT 为 `PMA`，PMA 为 main/cacheable/coherent，合并结果 `ace_cacheable=1`。
- `0xF0000000-0xF7FFFFFF`：PTE PBMT 为 `NC`，即使 PMA 为 main/cacheable/coherent，合并结果仍为 `ace_cacheable=0`。
- `0xF8000000-0xFFFFFFFF`：PTE PBMT 为 `IO`，即使 PMA 为 main/cacheable/coherent，合并结果仍为 `ace_cacheable=0`。

## 编译 VP

在 GUI-VP Kit 根目录执行：

```bash
cmake --build riscv-vp-plusplus/vp/build --target linux64-mc-vp -j$(nproc)
```

期望看到：

```text
[100%] Built target linux64-mc-vp
```

## 启动 Linux 并打开 ACE trace

为了只观察 PMA/PBMT 测试窗口，建议把 trace 限定到 cache-ace reserved-memory 范围：

```bash
timeout 120s env \
  -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY \
  -u all_proxy -u ALL_PROXY \
  RVVP_ACE_TRACE_BUDGET=40 \
  RVVP_ACE_TRACE_START=0xe0000000 \
  RVVP_ACE_TRACE_END=0xffffffff \
  make run_rv64_mc
```

说明：

- `RVVP_ACE_TRACE_BUDGET=40`：最多打印 40 条 ACE trace，避免日志过多。
- `RVVP_ACE_TRACE_START=0xe0000000`：只观察 cache-ace 窗口起始地址。
- `RVVP_ACE_TRACE_END=0xffffffff`：只观察 cache-ace reserved-memory 范围。
- `timeout 120s`：Linux 启动到登录提示后会继续等待输入，用 timeout 主动结束。
- 退出码 `124` 通常表示 timeout 主动终止，不代表 Linux 启动失败。

## Linux 侧测试函数

当前 Linux 驱动 `guivp_pbmt_test` 在初始化时会自动执行：

```c
guivp_pbmt_run_selftest()
```

该函数会访问 cache-ace reserved-memory 的三个分段：

- `PMA`：`0xE0000000-0xEFFEFFFF`，PTE PBMT 使用 PMA，由 VP PMA 属性决定 cache-control。
- `PMA_DENY`：`0xEFFFF000-0xEFFFFFFF`，VP PMA 禁止读/写，用于验证 PMA precise access-fault 能被 Linux nofault 访问捕获。
- `NC`：`0xF0000000-0xF7FFFFFF`，PTE PBMT 强制 non-cacheable。
- `IO`：`0xF8000000-0xFFFFFFFF`，PTE PBMT 强制 I/O。

`PMA/NC/IO` 分段会执行写后读验证，`PMA_DENY` 分段会执行异常触发验证。`PMA_DENY` 使用 `copy_from_kernel_nofault()` / `copy_to_kernel_nofault()` 访问禁止页，期望读写都返回 `-EFAULT`。日志中应出现类似输出：

```text
guivp_pbmt_test: reserved window base=0x00000000e0000000 size=0x0000000020000000 PMA=0-0xfffefff PMA_DENY=0xffff000-0xfffffff NC=0x10000000-0x17ffffff IO=0x18000000-0x1fffffff
guivp_pbmt_test: selftest PMA phys=0x00000000e0000000 wrote=... read=...
guivp_pbmt_test: selftest NC phys=0x00000000f0000000 wrote=... read=...
guivp_pbmt_test: selftest IO phys=0x00000000f8000000 wrote=... read=...
guivp_pbmt_test: fault selftest PMA_DENY phys=0x00000000effff000 read_ret=-14 write_ret=-14 PASS
```

判断标准：`PMA/NC/IO` 的 `wrote` 和 `read` 必须一致；`PMA_DENY` 必须出现 `PASS`，且 `read_ret` / `write_ret` 均为负值，通常为 `-14` 即 `-EFAULT`。后者才是 PMA 异常触发路径的验证点。

## master-ACE / coherent L2 侧判断标准

打开 trace 后，应能看到 master-ACE 入口打印 PTE PBMT 和 PMA 属性，例如：

```text
[MasterACE CoreMasterACE2 cpu-entry] WRITE addr=0xe0000000 len=8 pbmt=PMA pma_mem=main pma_cacheable=1 pma_coherent=1 ace_cacheable=1 domain=1 bufferable=1 modifiable=1 rd_alloc=1 wr_alloc=1
[MasterACE CoreMasterACE2 cpu-entry] READ addr=0xe0000000 len=8 pbmt=PMA pma_mem=main pma_cacheable=1 pma_coherent=1 ace_cacheable=1 domain=1 bufferable=1 modifiable=1 rd_alloc=1 wr_alloc=1
[MasterACE CoreMasterACE2 cpu-entry] WRITE addr=0xf0000000 len=8 pbmt=NC pma_mem=main pma_cacheable=1 pma_coherent=1 ace_cacheable=0 ...
[MasterACE CoreMasterACE2 cpu-entry] WRITE addr=0xf8000000 len=8 pbmt=IO pma_mem=main pma_cacheable=1 pma_coherent=1 ace_cacheable=0 ...
```

关键字段含义：

- `pbmt=PMA`：PTE PBMT 没有强制 NC/IO，使用 PMA 属性决定内存类型。
- `pbmt=NC`：PTE PBMT 强制 non-cacheable，即使 PMA 是 cacheable/coherent，合并结果也应为 `ace_cacheable=0`。
- `pbmt=IO`：PTE PBMT 强制 I/O，即使 PMA 是 cacheable/coherent，合并结果也应为 `ace_cacheable=0`。
- `pma_mem=main`：PMA 将该物理窗口识别为 main memory。
- `pma_cacheable=1`：PMA 允许该窗口进入 cache 层次。
- `pma_coherent=1`：PMA 允许该窗口参与一致性。
- `ace_cacheable=1`：PTE PBMT 和 PMA 合并后的结果允许走 ACE coherent cache 路径。
- `domain=1`：ACE domain 为 Inner shareable。
- `bufferable=1`、`modifiable=1`、`rd_alloc=1`、`wr_alloc=1`：ACE cache-control 允许缓存和分配。

随后应能看到事务进入 coherent L2：

```text
[MasterACE CoreMasterACE2.m_ace_port] READ addr=0xe0000000 len=64
[CoherentL2 AceSubsystem.l2_cache] READ addr=0xe0000000 len=64 requester=L1-0
[CoherentL2 AceSubsystem.l2_cache] response addr=0xe0000000 status=TLM_OK_RESPONSE
```

判断标准：PMA 分段事务应进入 `CoherentL2`，并且 response 为 `TLM_OK_RESPONSE`；NC/IO 分段应显示 `ace_cacheable=0`，不能按 coherent cacheable 事务处理。

## Linux 启动完成判断

成功启动时应看到：

```text
Welcome to Buildroot
buildroot login:
```

如果命令最后输出：

```text
make: *** [Makefile:106: run_rv64_mc] Terminated
```

同时退出码为 `124`，这是 `timeout 120s` 在登录提示后主动终止，不是启动失败。

## 异常判断

如果 PMA/PBMT 到 ACE cache-control 流程异常，可以优先检查：

1. 没有看到 `guivp_pbmt_test: fault selftest PMA_DENY ... PASS`：检查 VP 是否把 `0xEFFFF000-0xEFFFFFFF` 配置成不可读/不可写 PMA fault page。
2. `PMA_DENY` 的 `read_ret` 或 `write_ret` 为 0：说明异常没有触发，优先检查 PMA region 是否被更大的普通 region 覆盖。
3. 没有看到 `guivp_pbmt_test: selftest PMA/NC/IO`：检查 Linux 驱动是否编入 kernel，以及设备树是否包含 `reserved-memory/cache-ace@e0000000`。
4. `wrote` 和 `read` 不一致：检查 reserved-memory 窗口是否被正确映射到 VP memory，且没有被其他区域覆盖。
5. master-ACE trace 中 `ace_cacheable=0`：检查 PTE PBMT 是否为 NC/IO/RESERVED，或 PMA 是否被配置为 `cacheable=false` / `coherent=false` / `IO`。
6. 没有进入 `CoherentL2`：检查 `cache_ace_dram_window` 是否被从 data DMI 中挖掉，以及 RV64 多核路径是否绑定到了 `master-ace -> coherent L2`。
