#pragma once

#include <stdint.h>
#include <tlm_utils/tlm_quantumkeeper.h>

#include <systemc>

#include "irq_if.h"
#include "mmu_mem_if.h"
#include "util/propertytree.h"

constexpr unsigned PTE_PPN_SHIFT = 10;
constexpr unsigned PGSHIFT = 12;
constexpr unsigned PGSIZE = 1 << PGSHIFT;
constexpr unsigned PGMASK = PGSIZE - 1;

constexpr unsigned PTE_V = 1;
constexpr unsigned PTE_R = 1 << 1;
constexpr unsigned PTE_W = 1 << 2;
constexpr unsigned PTE_X = 1 << 3;
constexpr unsigned PTE_U = 1 << 4;
constexpr unsigned PTE_G = 1 << 5;
constexpr unsigned PTE_A = 1 << 6;
constexpr unsigned PTE_D = 1 << 7;
constexpr unsigned PTE_RSW = 0b11 << 8;

constexpr unsigned PMP_R = 0x01;
constexpr unsigned PMP_W = 0x02;
constexpr unsigned PMP_X = 0x04;
constexpr unsigned PMP_A = 0x18;
constexpr unsigned PMP_TOR = 0x08;
constexpr unsigned PMP_NA4 = 0x10;
constexpr unsigned PMP_NAPOT = 0x18;
constexpr unsigned PMP_L = 0x80;
constexpr unsigned PMP_SHIFT = 2;

struct pte_t {
	uint64_t value;

	bool V() {
		return value & PTE_V;
	}
	bool R() {
		return value & PTE_R;
	}
	bool W() {
		return value & PTE_W;
	}
	bool X() {
		return value & PTE_X;
	}
	bool U() {
		return value & PTE_U;
	}
	bool G() {
		return value & PTE_G;
	}
	bool A() {
		return value & PTE_A;
	}
	bool D() {
		return value & PTE_D;
	}

	operator uint64_t() {
		return value;
	}
};

struct vm_info {
	int levels;
	int idxbits;
	int widenbits;
	int ptesize;
	uint64_t ptbase;
};

struct xlate_flags_t {
	bool forced_virt = false;           // H Extension fdw: reserve a first-class flag for guest memory accesses so HLV/HSV can enter the shared MMU path without ad-hoc parameters
	bool hlvx = false;                  // H Extension fdw: reserve execute-like load semantics for future HLVX permission checks in the shared MMU path
	bool is_vs_stage_pt_access = false; // H Extension fdw: reserve an explicit marker for VS-stage page-table accesses before second-stage translation is introduced

	bool is_special_access() const {
		return forced_virt || hlvx || is_vs_stage_pt_access;
	}
};

// H Extension fdw: carry a shared effective translation context so future guest accesses and VS/VU stage-1 walks
// can flow through the same MMU entry instead of each caller open-coding satp/vsatp decisions.
struct mem_access_info_t {
	uint64_t vaddr;
	MemoryAccessType type;
	PrivilegeLevel effective_priv;
	bool effective_virt;
	xlate_flags_t flags;
};

template <typename T_RVX_ISS>
struct MMU_T {
	/* config properties */
	sc_core::sc_time prop_clock_cycle_period = sc_core::sc_time(10, sc_core::SC_NS);
	unsigned int prop_mmu_access_clock_cycles = 3;

	T_RVX_ISS &core;
	tlm_utils::tlm_quantumkeeper &quantum_keeper;
	sc_core::sc_time mmu_access_delay;

	mmu_memory_if *mem = nullptr;
	bool page_fault_on_AD = false;
	mutable unsigned guest_gstage_debug_budget = 64;

	struct tlb_entry_t {
		uint64_t ppn = -1;
		uint64_t vpn = -1;
	};

	static constexpr unsigned TLB_ENTRIES = 256;
	static constexpr unsigned NUM_MODES = 2;         // User and Supervisor
	static constexpr unsigned NUM_ACCESS_TYPES = 3;  // FETCH, LOAD, STORE

	tlb_entry_t tlb[NUM_MODES][NUM_ACCESS_TYPES][TLB_ENTRIES];

	MMU_T(T_RVX_ISS &core) : core(core), quantum_keeper(core.quantum_keeper) {
		/*
		 * get config properties from global property tree (or use default)
		 * Note: Instance has no name -> use the owners name is used as instance identifier
		 */
		VPPP_PROPERTY_GET("MMU." + core.name(), "clock_cycle_period", sc_core::sc_time, prop_clock_cycle_period);
		VPPP_PROPERTY_GET("MMU." + core.name(), "mmu_access_clock_cycles", uint64_t, prop_mmu_access_clock_cycles);

		mmu_access_delay = prop_clock_cycle_period * prop_mmu_access_clock_cycles;

		flush_tlb();
	}

	void flush_tlb() {
		memset(&tlb[0], -1, NUM_MODES * NUM_ACCESS_TYPES * TLB_ENTRIES * sizeof(tlb_entry_t));
	}

	uint64_t translate_virtual_to_physical_addr(uint64_t vaddr, MemoryAccessType type, xlate_flags_t flags = {}) {
		// H Extension fdw: normalize every translation request into a shared access context before stage-1 root
		// selection so VS/VU accesses can evolve toward Spike-like effective privilege/virtualization handling.
		mem_access_info_t access_info = generate_access_info(vaddr, type, flags);
		auto &stage1_satp = get_stage1_satp(access_info);

		if (stage1_satp.reg.fields.mode == SATP_MODE_BARE && !access_info.effective_virt)
			return vaddr;

		auto mode = access_info.effective_priv;

		if (mode == MachineMode)
			return vaddr;

		// optional timing
		quantum_keeper.inc(mmu_access_delay);

		// optimization only, to void page walk
		assert(mode == 0 || mode == 1);
		assert(type == 0 || type == 1 || type == 2);
		auto vpn = (vaddr >> PGSHIFT);
		auto idx = vpn % TLB_ENTRIES;
		auto &x = tlb[mode][type][idx];
		if (x.vpn == vpn)
			return x.ppn | (vaddr & PGMASK);

		uint64_t paddr = walk(access_info);

		if (!pmp_ok(paddr, type, access_info.effective_priv, access_info.flags.hlvx)) {
			switch (type) {
				case FETCH:
					raise_trap(EXC_INSTR_ACCESS_FAULT, access_info.vaddr);
					break;
				case LOAD:
					raise_trap(EXC_LOAD_ACCESS_FAULT, access_info.vaddr);
					break;
				case STORE:
					raise_trap(EXC_STORE_AMO_ACCESS_FAULT, access_info.vaddr);
					break;
			}
		}

		// optimization only, to void page walk
		x.ppn = (paddr & ~((uint64_t)PGMASK));
		x.vpn = vpn;

		return paddr;
	}

	mem_access_info_t generate_access_info(uint64_t vaddr, MemoryAccessType type, xlate_flags_t flags = {}) {
		// H Extension fdw: establish a first-class MMU context now so later guest_load/guest_store/HLVX support can
		// reuse the same entry path instead of widening translate_virtual_to_physical_addr ad hoc.
		mem_access_info_t access_info = {vaddr, type, core.prv, core.virt, flags};

		if (type != FETCH && core.csrs.mstatus.reg.fields.mprv)
			access_info.effective_priv = core.csrs.mstatus.reg.fields.mpp;

		if (flags.forced_virt && core.csrs.misa.has_hypervisor_extension()) {
			// H Extension fdw: route future HLV/HSV guest accesses through a first-class virtualization context instead
			// of ad-hoc satp/vsatp switches in individual callers.
			access_info.effective_virt = true;
			access_info.effective_priv = core.csrs.hstatus.reg.fields.spvp ? SupervisorMode : UserMode;
		}

		return access_info;
	}

	auto &get_stage1_satp(const mem_access_info_t &access_info) {
		// H Extension fdw: choose the stage-1 root from satp or vsatp using the shared translation context so VS/VU
		// execution no longer leaks through the non-virtualized satp path.
		return access_info.effective_virt ? core.csrs.vsatp : core.csrs.satp;
	}

	vm_info decode_vm_info(const mem_access_info_t &access_info) {
		// H Extension fdw: decode the stage-1 VM geometry from the already-selected satp/vsatp view so the page walk
		// can stop depending on the non-virtualized satp storage directly.
		assert(access_info.effective_priv <= SupervisorMode);
		auto &stage1_satp = get_stage1_satp(access_info);
		uint64_t ptbase = (uint64_t)stage1_satp.reg.fields.ppn << PGSHIFT;
		unsigned mode = stage1_satp.reg.fields.mode;
		switch (mode) {
			case SATP_MODE_BARE:
				// H Extension fdw: match Spike's walk() contract where a Bare first stage yields levels==0 and still
				// allows the caller to continue into G-stage translation if V=1.
				return {0, 0, 0, 0, 0};
			case SATP_MODE_SV32:
				return {2, 10, 0, 4, ptbase};
			case SATP_MODE_SV39:
				return {3, 9, 0, 8, ptbase};
			case SATP_MODE_SV48:
				return {4, 9, 0, 8, ptbase};
			case SATP_MODE_SV57:
				return {5, 9, 0, 8, ptbase};
			case SATP_MODE_SV64:
				return {6, 9, 0, 8, ptbase};
			default:
				throw std::runtime_error("unknown Sv (satp) mode " + std::to_string(mode));
		}
	}

	vm_info decode_gstage_vm_info() {
		// H Extension fdw: establish a dedicated G-stage geometry decode entry now so hgatp-backed second-stage page
		// walks can be threaded into walk() incrementally instead of overloading the existing satp/vsatp decoder.
		constexpr unsigned HGATP_MODE_OFF_LOCAL = 0x0;
		constexpr unsigned HGATP_MODE_SV32X4_LOCAL = 0x1;
		constexpr unsigned HGATP_MODE_SV39X4_LOCAL = 0x8;
		constexpr unsigned HGATP_MODE_SV48X4_LOCAL = 0x9;
		constexpr unsigned HGATP_MODE_SV57X4_LOCAL = 0xA;

		uint64_t ptbase = (uint64_t)core.csrs.hgatp.reg.fields.ppn << PGSHIFT;
		unsigned mode = core.csrs.hgatp.reg.fields.mode;
		if (core.xlen == 32) {
			if (mode == HGATP_MODE_OFF_LOCAL)
				return {0, 0, 0, 0, 0};
			switch (mode) {
				case HGATP_MODE_SV32X4_LOCAL:
					return {2, 10, 2, 4, ptbase};
				default:
					throw std::runtime_error("unknown G-stage (hgatp) mode " + std::to_string(mode));
			}
		} else {
			if (mode == HGATP_MODE_OFF_LOCAL)
				return {0, 0, 0, 0, 0};
			switch (mode) {
				case HGATP_MODE_SV39X4_LOCAL:
					return {3, 9, 2, 8, ptbase};
				case HGATP_MODE_SV48X4_LOCAL:
					return {4, 9, 2, 8, ptbase};
				case HGATP_MODE_SV57X4_LOCAL:
					return {5, 9, 2, 8, ptbase};
				default:
					throw std::runtime_error("unknown G-stage (hgatp) mode " + std::to_string(mode));
			}
		}
	}

	bool check_vaddr_extension(uint64_t vaddr, const vm_info &vm) {
		int highbit = vm.idxbits * vm.levels + PGSHIFT - 1;
		assert(highbit > 0);
		uint64_t ext_mask = (uint64_t(1) << (core.xlen - highbit)) - 1;
		uint64_t bits = (vaddr >> highbit) & ext_mask;
		bool ok = (bits == 0) || (bits == ext_mask);
		return ok;
	}

	uint64_t pmp_addr_at(unsigned index) const {
		return uint64_t(core.csrs.pmpaddr[index].reg.val) << PMP_SHIFT;
	}

	uint64_t pmp_cfg_at(unsigned index) const {
		const unsigned cfgs_per_reg = core.xlen / 8;
		const unsigned reg_index = index / cfgs_per_reg;
		const unsigned byte_index = index % cfgs_per_reg;
		return (uint64_t(core.csrs.pmpcfg[reg_index].reg.val) >> (8 * byte_index)) & 0xff;
	}

	bool pmp_any_active() const {
		for (unsigned i = 0; i < core.csrs.pmpaddr.size(); ++i) {
			if (pmp_cfg_at(i) & PMP_A)
				return true;
		}
		return false;
	}

	uint64_t pmp_napot_mask(unsigned index, uint64_t cfg) const {
		const bool is_na4 = (cfg & PMP_A) == PMP_NA4;
		uint64_t raw = uint64_t(core.csrs.pmpaddr[index].reg.val);
		uint64_t mask = (raw << 1) | (!is_na4);
		return ~(mask & ~(mask + 1)) << PMP_SHIFT;
	}

	bool pmp_match4(unsigned index, uint64_t cfg, uint64_t addr) const {
		if ((cfg & PMP_A) == 0)
			return false;
		if ((cfg & PMP_A) == PMP_TOR) {
			uint64_t base = index == 0 ? 0 : pmp_addr_at(index - 1);
			uint64_t top = pmp_addr_at(index);
			return base <= addr && addr < top;
		}
		return ((addr ^ pmp_addr_at(index)) & pmp_napot_mask(index, cfg)) == 0;
	}

	bool pmp_access_ok(uint64_t cfg, MemoryAccessType type, PrivilegeLevel mode, bool hlvx) const {
		const bool cfg_r = cfg & PMP_R;
		const bool cfg_w = cfg & PMP_W;
		const bool cfg_x = cfg & PMP_X;
		const bool cfg_l = cfg & PMP_L;
		const bool prv_m = mode == MachineMode;
		const bool normal_rwx =
		    (type == LOAD && cfg_r && (!hlvx || cfg_x)) || (type == STORE && cfg_w) || (type == FETCH && cfg_x);
		const bool m_bypass = prv_m && !cfg_l;
		return m_bypass || normal_rwx;
	}

	bool pmp_ok(uint64_t addr, MemoryAccessType type, PrivilegeLevel mode, bool hlvx) const {
		if (!pmp_any_active())
			return true;

		for (unsigned i = 0; i < core.csrs.pmpaddr.size(); ++i) {
			uint64_t cfg = pmp_cfg_at(i);
			if (!pmp_match4(i, cfg, addr))
				continue;
			return pmp_access_ok(cfg, type, mode, hlvx);
		}

		return mode == MachineMode;
	}

	bool check_gpa_extension(uint64_t gpa, const vm_info &vm) {
		if (vm.levels == 0)
			return true;
		int gpabits = vm.levels * vm.idxbits + vm.widenbits + PGSHIFT;
		if (gpabits >= 64)
			return true;
		uint64_t ext_mask = ~((uint64_t(1) << gpabits) - 1);
		return (gpa & ext_mask) == 0;
	}

	bool should_log_guest_gstage(uint64_t gpa, const mem_access_info_t &access_info, MemoryAccessType trap_type,
	                             bool is_vs_stage_pt_access) const {
		// H Extension fdw: keep G-stage trace opt-in via the existing guest debug switch so normal L2 bring-up runs
		// can expose guest console output without being drowned out by page-walk logging.
		if (!core.guest_mmio_debug)
			return false;
		// H Extension fdw: keep the G-stage trace narrowly focused on the current L2 entry regression so we can
		// compare the exact GPA->PTE->HPA path against Spike without flooding the console.
		if (!(access_info.effective_virt && !is_vs_stage_pt_access && trap_type == FETCH && gpa >= 0x80200000 &&
		      gpa < 0x80202000))
			return false;
		if (guest_gstage_debug_budget == 0)
			return false;
		guest_gstage_debug_budget--;
		return true;
	}

	uint64_t s2xlate(uint64_t gpa, const mem_access_info_t &access_info, MemoryAccessType stage2_type,
	                 MemoryAccessType trap_type,
	                 bool is_vs_stage_pt_access) {
		// H Extension fdw: introduce a minimal G-stage translation skeleton so walk() can start routing VS page-table
		// accesses and guest physical addresses through hgatp-backed translation incrementally.
		if (!access_info.effective_virt)
			return gpa;

		vm_info vm = decode_gstage_vm_info();
		if (vm.levels == 0)
			return gpa;

		const bool log_guest_gstage = should_log_guest_gstage(gpa, access_info, trap_type, is_vs_stage_pt_access);
		if (log_guest_gstage) {
			std::cout << "[vp::guest-gstage-begin] hart " << core.get_hart_id() << " gpa=0x" << std::hex << gpa
			          << " stage2_type=" << std::dec << int(stage2_type) << " trap_type=" << int(trap_type)
			          << " hgatp=0x" << std::hex << core.csrs.hgatp.reg.val << " ptbase=0x" << vm.ptbase
			          << " levels=" << std::dec << vm.levels << " idxbits=" << vm.idxbits
			          << " widenbits=" << vm.widenbits << std::endl;
		}

		// H Extension fdw: HLVX execute-like semantics apply to the final guest access, not to the implicit G-stage
		// reads/writes of VS-stage page-table pages themselves. Keep VS-stage PT accesses as plain LOAD/STORE here,
		// matching Spike's separate hlvx=false path for is_for_vs_pt_addr=true.
		const bool hlvx = access_info.flags.hlvx && !is_vs_stage_pt_access;

		const auto raise_guest_page_fault = [&](uint64_t fault_gpa) -> void {
			uint64_t tinst = 0;
			if (is_vs_stage_pt_access) {
				// H Extension fdw: encode the implicit VS-stage page-table access pseudoinstruction so M/HS trap entry can
				// expose mtinst/htinst values compatible with Spike for SLAT pseudoinstruction tests.
				tinst |= 0x2000;
				if (core.xlen == 64)
					tinst |= 0x1000;
				if (stage2_type == STORE)
					tinst |= 0x0020;
			}

			SimulationTrap trap;
			trap.reason = static_cast<ExceptionCode>(trap_type == FETCH   ? EXC_INSTR_GUEST_PAGE_FAULT
			                                    : trap_type == LOAD    ? EXC_LOAD_GUEST_PAGE_FAULT
			                                                          : EXC_STORE_AMO_GUEST_PAGE_FAULT);
			trap.mtval = access_info.vaddr;
			trap.tval2 = fault_gpa >> 2;
			trap.tinst = tinst;
			trap.write_gva = access_info.effective_virt;
			throw trap;
		};

		if (!check_gpa_extension(gpa, vm)) {
			switch (trap_type) {
				case FETCH:
					raise_guest_page_fault(gpa);
					break;
				case LOAD:
					raise_guest_page_fault(gpa);
					break;
				case STORE:
					raise_guest_page_fault(gpa);
					break;
			}
		}

		uint64_t base = vm.ptbase;
		bool mxr = !is_vs_stage_pt_access && core.csrs.mstatus.reg.fields.mxr;
		for (int i = vm.levels - 1; i >= 0; --i) {
			int ptshift = i * vm.idxbits;
			int idxbits = (i == (vm.levels - 1)) ? vm.idxbits + vm.widenbits : vm.idxbits;
			uint64_t idx = (gpa >> (PGSHIFT + ptshift)) & ((uint64_t(1) << idxbits) - 1);
			uint64_t pte_paddr = base + idx * vm.ptesize;

			assert(vm.ptesize == 4 || vm.ptesize == 8);
			assert(mem);
			pte_t pte;
			if (vm.ptesize == 4)
				pte.value = mem->mmu_load_pte32(pte_paddr);
			else
				pte.value = mem->mmu_load_pte64(pte_paddr);

			if (log_guest_gstage) {
				std::cout << "[vp::guest-gstage-step] hart " << core.get_hart_id() << " level=" << i
				          << " base=0x" << std::hex << base << " idx=0x" << idx << " pte_paddr=0x"
				          << pte_paddr << " pte=0x" << pte.value << " ppn=0x" << (pte.value >> PTE_PPN_SHIFT)
				          << std::endl;
			}

			uint64_t ppn = pte >> PTE_PPN_SHIFT;
			if (!pte.V() || (!pte.R() && pte.W()))
				break;

			if (!pte.R() && !pte.X()) {
				base = ppn << PGSHIFT;
				continue;
			}

			if (!pte.U())
				break;

			if ((stage2_type == FETCH || hlvx) && !pte.X())
				break;
			if ((stage2_type == LOAD) && !hlvx && !pte.R() && !(mxr && pte.X()))
				break;
			if ((stage2_type == STORE) && !(pte.R() && pte.W()))
				break;

			if ((ppn & ((uint64_t(1) << ptshift) - 1)) != 0)
				break;

			uint64_t ad = PTE_A | ((stage2_type == STORE) * PTE_D);
			constexpr uint64_t H_EXT_ADUE_MASK = 0x2000000000000000ULL;
			const bool gstage_adue = core.csrs.menvcfg.reg.val & H_EXT_ADUE_MASK;
			if ((pte & ad) != ad) {
				if (!gstage_adue) {
					break;  // H Extension fdw: align G-stage A/D handling with Spike by honoring menvcfg.ADUE instead of a global page-fault toggle
				} else {
					mem->mmu_store_pte32(pte_paddr, pte | ad);
				}
			}

			uint64_t mask = ((uint64_t(1) << ptshift) - 1);
			uint64_t vpn = gpa >> PGSHIFT;
			uint64_t pgoff = gpa & (PGSIZE - 1);
			uint64_t hpa = (((ppn & ~mask) | (vpn & mask)) << PGSHIFT) | pgoff;
			if (log_guest_gstage) {
				std::cout << "[vp::guest-gstage-leaf] hart " << core.get_hart_id() << " level=" << i
				          << " mask=0x" << std::hex << mask << " vpn=0x" << vpn << " hpa=0x" << hpa
				          << std::endl;
			}
			return hpa;
		}

		switch (trap_type) {
			case FETCH:
				raise_guest_page_fault(gpa);
				break;
			case LOAD:
				raise_guest_page_fault(gpa);
				break;
			case STORE:
				raise_guest_page_fault(gpa);
				break;
		}

		throw std::runtime_error("[mmu] unknown G-stage access type " + std::to_string(trap_type));
	}

	uint64_t walk(const mem_access_info_t &access_info) {
		// H Extension fdw: route page walks through the shared translation context now so second-stage hooks can be
		// introduced later without re-threading raw vaddr/type/mode parameters through every caller again.
		uint64_t vaddr = access_info.vaddr;
		MemoryAccessType type = access_info.type;
		bool hlvx = access_info.flags.hlvx;
		PrivilegeLevel mode = access_info.effective_priv;
		bool s_mode = mode == SupervisorMode;
		bool sum = core.csrs.mstatus.reg.fields.sum;
		bool mxr = core.csrs.mstatus.reg.fields.mxr;

		vm_info vm = decode_vm_info(access_info);

		if (vm.levels == 0) {
			// H Extension fdw: match Spike's "VS bare still does G-stage" behavior. When V=1 and VSATP is Bare, the
			// first stage produces GPA==VA, but HGATP must still translate that GPA before fetch/load/store touch memory.
			return s2xlate(vaddr, access_info, type, type, false);
		}

		if (!check_vaddr_extension(vaddr, vm))
			vm.levels = 0;  // skip loop and raise page fault

		uint64_t base = vm.ptbase;
		for (int i = vm.levels - 1; i >= 0; --i) {
			// obtain VPN field for current level, NOTE: all VPN fields have the same length for each separate VM
			// implementation
			int ptshift = i * vm.idxbits;
			unsigned vpn_field = (vaddr >> (PGSHIFT + ptshift)) & ((1 << vm.idxbits) - 1);

			uint64_t pte_gpa = base + vpn_field * vm.ptesize;
			// H Extension fdw: route VS-stage page-table accesses through the minimal G-stage translator so first-stage
			// page walks stop assuming their PTE addresses are already host physical addresses.
			auto pte_paddr = s2xlate(pte_gpa, access_info, LOAD, type, true);
			// TODO: PMP checks for pte_paddr with (LOAD, PRV_S)

			assert(vm.ptesize == 4 || vm.ptesize == 8);
			assert(mem);
			pte_t pte;
			if (vm.ptesize == 4)
				pte.value = mem->mmu_load_pte32(pte_paddr);
			else
				pte.value = mem->mmu_load_pte64(pte_paddr);

			uint64_t ppn = pte >> PTE_PPN_SHIFT;

			if (!pte.V() || (!pte.R() && pte.W())) {
				// std::cout << "[mmu] !pte.V() || (!pte.R() && pte.W())" << std::endl;
				break;
			}

			if (!pte.R() && !pte.X()) {
				base = ppn << PGSHIFT;
				continue;
			}

			assert(type == FETCH || type == LOAD || type == STORE);
			if ((type == FETCH || hlvx) && !pte.X()) {
				// std::cout << "[mmu] (type == FETCH) && !pte.X()" << std::endl;
				break;
			}
			if ((type == LOAD) && !hlvx && !pte.R() && !(mxr && pte.X())) {
				// std::cout << "[mmu] (type == LOAD) && !pte.R() && !(mxr && pte.X())" << std::endl;
				break;
			}
			if ((type == STORE) && !(pte.R() && pte.W())) {
				// std::cout << "[mmu] (type == STORE) && !(pte.R() && pte.W())" << std::endl;
				break;
			}

			if (pte.U()) {
				if (s_mode && ((type == FETCH) || !sum))
					break;
			} else {
				if (!s_mode)
					break;
			}

			// NOTE: all PPN (except the highest one) have the same bitwidth as the VPNs, hence ptshift can be used
			if ((ppn & ((uint64_t(1) << ptshift) - 1)) != 0)
				break;  // misaligned superpage

				uint64_t ad = PTE_A | ((type == STORE) * PTE_D);
				constexpr uint64_t H_EXT_ADUE_MASK = 0x2000000000000000ULL;
				const bool adue = access_info.effective_virt ? (core.csrs.henvcfg.reg.val & H_EXT_ADUE_MASK)
				                                             : (core.csrs.menvcfg.reg.val & H_EXT_ADUE_MASK);
				if ((pte & ad) != ad) {
					if (!adue) {
						break;  // H Extension fdw: match Spike by faulting on missing A/D bits unless the active envcfg.ADUE enables implicit updates
					} else {
						// TODO: PMP checks for pte_paddr with (STORE, PRV_S)
						// H Extension fdw: an implicit VS-stage A/D update is itself a guest memory write and must satisfy
						// second-stage permissions before the first-stage PTE store becomes architecturally visible.
						s2xlate(pte_gpa, access_info, STORE, type, true);

						// NOTE: the store has to be atomic with the above load of the PTE, i.e. lock the bus if required
						// NOTE: only need to update A / D flags, hence it is enough to store 32 bit (8 bit might be enough
						// too)
						mem->mmu_store_pte32(pte_paddr, pte | ad);
					}
				}

			// translation successful, return physical address
			uint64_t mask = ((uint64_t(1) << ptshift) - 1);
			uint64_t vpn = vaddr >> PGSHIFT;
			uint64_t pgoff = vaddr & (PGSIZE - 1);
			uint64_t gpa = (((ppn & ~mask) | (vpn & mask)) << PGSHIFT) | pgoff;
			// H Extension fdw: treat the stage-1 leaf result as a GPA and let the minimal G-stage translator produce
			// the final host physical address before returning from walk().
			return s2xlate(gpa, access_info, type, type, false);
		}

		switch (type) {
			case FETCH:
				raise_trap(EXC_INSTR_PAGE_FAULT, vaddr);
				break;
			case LOAD:
				raise_trap(EXC_LOAD_PAGE_FAULT, vaddr);
				break;
			case STORE:
				raise_trap(EXC_STORE_AMO_PAGE_FAULT, vaddr);
				break;
		}

		throw std::runtime_error("[mmu] unknown access type " + std::to_string(type));
	}
};
