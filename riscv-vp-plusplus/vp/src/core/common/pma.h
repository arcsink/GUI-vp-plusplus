#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <vector>

#include "irq_if.h"
#include "mmu_mem_if.h"

// PMA（Physical Memory Attributes）描述的是虚拟地址翻译完成后，针对最终物理地址
// 才能确定的内存属性。本文件中的检查器不负责地址翻译，只校验物理访问是否合法；
// 若属性不满足要求，由调用方根据访问类型触发对应的 access-fault trap。
enum class PmaMemoryType {
	// 普通主存，默认可缓存、可参与一致性，典型场景是 DRAM。
	MainMemory,
	// 设备或 memory-mapped IO 区域。平台可以对此类区域收紧访问宽度、ordering、
	// 幂等性、cacheable/coherent、atomic 等属性。
	IO,
};

// AMO PMA 按规范 3.6.3.1 使用递增能力分类：
// AMONone < AMOSwap < AMOLogical < AMOArithmetic。
// AMOArithmetic 表示支持算术类 AMO 的完整常见集合；CAS 不在这里作为并列 PMA class 建模。
enum class PmaAmoClass {
	AMONone,
	AMOSwap,
	AMOLogical,
	AMOArithmetic,
};

// Reservability 控制 LR/SC 是否允许，以及是否具备 eventual forward progress 语义。
enum class PmaReservability {
	RsrvNone,
	RsrvNonEventual,
	RsrvEventual,
};

// Ordering 作为 PMA 属性保存，后续 interconnect/cache 路径可以据此选择对应的
// 内存顺序模型或 IO ordering 策略。
enum class PmaOrdering {
	RVWMO,
	RVTSO,
	RelaxedIO,
	StrongIO,
};

// PMA 失败后的平台响应类型。规范 3.6 建议：能精确 trap 的 PMA violation
// 表现为 access-fault；legacy bus/device 探测场景也可能返回 bus error 或 timeout。
enum class PmaFailureResponse {
	PreciseAccessFault,
	BusError,
	Timeout,
};

// 描述某一类访问（read/write/execute）支持的访问宽度。
struct pma_access_widths {
	bool byte = true;
	bool half = true;
	bool word = true;
	bool double_word = true;
	bool quad_word = false;
	bool misaligned = true;

	bool supports(uint64_t size) const {
		switch (size) {
			case 1:
				return byte;
			case 2:
				return half;
			case 4:
				return word;
			case 8:
				return double_word;
			case 16:
				return quad_word;
			default:
				return false;
		}
	}
};

// 单个物理内存区域的完整 PMA 属性集合。默认值用于保持旧 VP 行为：
// 未显式配置的区域按普通 coherent main memory 处理，支持常见标量宽度并使用 RVWMO。
struct pma_attributes {
	PmaMemoryType memory_type = PmaMemoryType::MainMemory;

	pma_access_widths read;
	pma_access_widths write;
	pma_access_widths execute = {false, true, true, false, false, true};

	bool page_table_read = true;
	bool page_table_write = true;

	// PMA 模型中的 AMO/LRSC 属性。misaligned AMO 只有在可选的
	// misaligned atomicity granule 覆盖整个访问时才允许。
	PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic;
	PmaReservability reservability = PmaReservability::RsrvEventual;
	uint64_t misaligned_atomicity_granule = 0;

	// cache/coherence/order 字段在这里先作为描述性属性保存；mem.h 和 ACE 路径
	// 可以使用这些字段来驱动事务行为，后续也可以继续加强检查。
	PmaOrdering ordering = PmaOrdering::RVWMO;
	unsigned ordering_channel = 0;
	bool cacheable = true;
	bool coherent = true;
	unsigned coherence_controller = 0;

	// 非幂等区域默认不应被隐式访问使用，例如取指、页表遍历等；除非属性明确允许。
	bool idempotent = true;
	bool read_idempotent = true;
	bool write_idempotent = true;

	// PMA violation 的平台响应类型。默认使用精确 access-fault；若某个 legacy bus
	// 或设备区域无法在 CPU 侧精确 trap，可配置为 bus-error 或 timeout 语义。
	PmaFailureResponse failure_response = PmaFailureResponse::PreciseAccessFault;

	// 平台扩展预留字段，用于承载暂未显式建模的属性。保持 opaque，便于后续添加策略。
	uint64_t custom = 0;

	bool readable() const {
		return read.byte || read.half || read.word || read.double_word || read.quad_word;
	}

	bool writable() const {
		return write.byte || write.half || write.word || write.double_word || write.quad_word;
	}

	bool executable() const {
		return execute.byte || execute.half || execute.word || execute.double_word || execute.quad_word;
	}

	bool atomic() const {
		return amo_class != PmaAmoClass::AMONone || reservability != PmaReservability::RsrvNone;
	}
};

// PMA region 使用半开区间：[base, base + size)。
struct pma_region {
	uint64_t base = 0;
	uint64_t size = 0;
	pma_attributes attributes;

	uint64_t end() const {
		return base + size;
	}

	bool contains(uint64_t paddr) const {
		return size != 0 && paddr >= base && paddr < end();
	}
};

// 地址翻译完成后，memory interface 传给 PMA 检查器的访问元数据。
// PMA 合法性不仅取决于物理地址和宽度，也取决于访问类型和是否为 atomic/隐式访问。
struct pma_check_request {
	uint64_t paddr = 0;
	uint64_t size = 1;
	MemoryAccessType access_type = LOAD;
	bool atomic = false;
	bool lr_sc = false;
	PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic;
	PmaReservability reservability = PmaReservability::RsrvNonEventual;
	bool page_table_access = false;
	bool implicit = false;
};

// 检查结果中保留已经解析出的属性，即使访问被拒绝，调用方也能直接追踪失败原因，
// 不需要再次进行 region 查找。
struct pma_check_result {
	bool allowed = true;
	const pma_region *region = nullptr;
	pma_attributes attributes;
	PmaFailureResponse failure_response = PmaFailureResponse::PreciseAccessFault;
	const char *reason = "";
};

class PMA {
   public:
	// PMA 配置属于 machine-mode 平台职责。S-mode/U-mode 可以通过 VP 查询属性，
	// 但不能修改属性。
	static bool is_machine_mode(PrivilegeLevel mode) {
		return mode == MachineMode;
	}

	static bool can_set_attributes(PrivilegeLevel mode) {
		return is_machine_mode(mode);
	}

	const pma_attributes &get_default_attributes() const {
		return default_attributes;
	}

	const std::vector<pma_region> &get_regions() const {
		return regions;
	}

	const pma_region *find_region(uint64_t paddr) const {
		auto it = std::find_if(regions.begin(), regions.end(),
		                       [paddr](const pma_region &region) { return region.contains(paddr); });
		return it == regions.end() ? nullptr : &*it;
	}

	pma_attributes get_attributes(uint64_t paddr) const {
		const pma_region *region = find_region(paddr);
		return region ? region->attributes : default_attributes;
	}

	pma_check_result check_access(const pma_check_request &request) const {
		// 先解析 region，再基于命中的 region 属性或默认属性执行后续检查。
		pma_check_result result;
		result.region = find_region(request.paddr);
		result.attributes = result.region ? result.region->attributes : default_attributes;
		result.failure_response = result.attributes.failure_response;

		if (request.size == 0) {
			return deny(result, "zero-sized access");
		}

		if (request.paddr > std::numeric_limits<uint64_t>::max() - (request.size - 1)) {
			return deny(result, "access address range overflow");
		}

		const pma_access_widths *widths = access_widths(result.attributes, request.access_type);
		if (widths == nullptr || !widths->supports(request.size)) {
			return deny(result, "access width is not supported");
		}

		if (!is_aligned(request.paddr, request.size)) {
			// misaligned 标量访问使用对应宽度的 misaligned 标志；misaligned atomic
			// 需要更强的 PMA atomicity-granule 保证。
			if (request.atomic && !is_within_misaligned_atomicity_granule(result.attributes, request.paddr, request.size)) {
				return deny(result, "misaligned atomic access is not supported");
			}
			if (!request.atomic && !widths->misaligned) {
				return deny(result, "misaligned access is not supported");
			}
		}

		if (request.page_table_access) {
			// 页表读写属于隐式内存访问；对于不能安全承载页表的区域，可以关闭该能力。
			if (request.access_type == LOAD && !result.attributes.page_table_read) {
				return deny(result, "page-table read is not supported");
			}
			if (request.access_type == STORE && !result.attributes.page_table_write) {
				return deny(result, "page-table write is not supported");
			}
		}

		if (request.atomic) {
			// LR/SC 使用 reservability 判断；其他 AMO 使用 AMO class 判断。
			if (request.lr_sc) {
				if (!supports_reservability(result.attributes.reservability, request.reservability)) {
					return deny(result, "LR/SC access is not reservable");
				}
			} else if (!supports_amo_class(result.attributes.amo_class, request.amo_class)) {
				return deny(result, "AMO class is not supported");
			}
		}

		if (request.implicit && !result.attributes.idempotent) {
			// 对隐式访问保持保守：推测访问或自动访问非幂等区域可能产生可见副作用。
			if (request.access_type == LOAD && !result.attributes.read_idempotent) {
				return deny(result, "implicit read to non-idempotent region is not supported");
			}
			if (request.access_type == STORE && !result.attributes.write_idempotent) {
				return deny(result, "implicit write to non-idempotent region is not supported");
			}
		}

		return result;
	}

	void set_default_attributes(const pma_attributes &attributes, PrivilegeLevel mode) {
		require_machine_mode(mode);
		default_attributes = attributes;
	}

	void set_region(uint64_t base, uint64_t size, const pma_attributes &attributes, PrivilegeLevel mode) {
		require_machine_mode(mode);
		validate_region(base, size);

		// 命中完全相同的范围时直接更新属性，允许平台代码在启动过程中逐步细化 PMA，
		// 同时避免产生重复 region。
		auto it = std::find_if(regions.begin(), regions.end(), [base, size](const pma_region &region) {
			return region.base == base && region.size == size;
		});

		if (it != regions.end()) {
			it->attributes = attributes;
			return;
		}

		regions.push_back({base, size, attributes});
	}

	bool clear_region(uint64_t base, uint64_t size, PrivilegeLevel mode) {
		require_machine_mode(mode);

		auto old_size = regions.size();
		regions.erase(std::remove_if(regions.begin(), regions.end(),
		                             [base, size](const pma_region &region) {
			                             return region.base == base && region.size == size;
		                             }),
		              regions.end());
		return regions.size() != old_size;
	}

   private:
	pma_attributes default_attributes;
	std::vector<pma_region> regions;

	static pma_check_result deny(pma_check_result result, const char *reason) {
		result.allowed = false;
		result.reason = reason;
		return result;
	}

	static bool is_aligned(uint64_t addr, uint64_t size) {
		return size == 0 || (addr % size) == 0;
	}

	static const pma_access_widths *access_widths(const pma_attributes &attributes, MemoryAccessType type) {
		switch (type) {
			case FETCH:
				return &attributes.execute;
			case LOAD:
				return &attributes.read;
			case STORE:
				return &attributes.write;
		}
		return nullptr;
	}

	static bool is_power_of_two(uint64_t value) {
		return value != 0 && (value & (value - 1)) == 0;
	}

	static bool supports_amo_class(PmaAmoClass supported, PmaAmoClass requested) {
		return static_cast<unsigned>(supported) >= static_cast<unsigned>(requested);
	}

	static bool supports_reservability(PmaReservability supported, PmaReservability requested) {
		return static_cast<unsigned>(supported) >= static_cast<unsigned>(requested);
	}

	static bool is_within_misaligned_atomicity_granule(const pma_attributes &attributes, uint64_t addr, uint64_t size) {
		const uint64_t granule = attributes.misaligned_atomicity_granule;
		if (!is_power_of_two(granule) || size > granule) {
			return false;
		}

		const uint64_t mask = granule - 1;
		return (addr & ~mask) == ((addr + size - 1) & ~mask);
	}

	static void require_machine_mode(PrivilegeLevel mode) {
		if (!is_machine_mode(mode))
			throw std::runtime_error("[pma] attributes can only be configured in machine mode");
	}

	static void validate_region(uint64_t base, uint64_t size) {
		if (size == 0)
			throw std::invalid_argument("[pma] region size must not be zero");

		if (base > std::numeric_limits<uint64_t>::max() - size)
			throw std::invalid_argument("[pma] region address range overflow");
	}
};
