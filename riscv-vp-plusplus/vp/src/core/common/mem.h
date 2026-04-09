#ifndef RISCV_ISA_MEM_H
#define RISCV_ISA_MEM_H

#include "bus_lock_if.h"
#include "dmi.h"
#include "mem_if.h"
#include "mmu.h"
#include "pma.h"
#include "util/tlm_ext_pbmt.h"
#include "util/propertytree.h"
#include "util/tlm_ext_initiator.h"

/*
 * For optimization, use DMI to fetch instructions
 * NOTE: Also by-passes the MMU for efficiency reasons -> CAN NOT BE USED ON PLATFORMS WITH VIRTUAL MEMORY MANAGEMENT!
 */
template <typename T_RVX_ISS>
struct InstrMemoryProxy_T : public instr_memory_if {
	/* config properties */
	sc_core::sc_time prop_clock_cycle_period = sc_core::sc_time(10, sc_core::SC_NS);
	unsigned int prop_access_clock_cycles = 2;

	MemoryDMI dmi;

	tlm_utils::tlm_quantumkeeper &quantum_keeper;

	sc_core::sc_time access_delay;

	InstrMemoryProxy_T(const MemoryDMI &dmi, T_RVX_ISS &owner) : dmi(dmi), quantum_keeper(owner.quantum_keeper) {
		/*
		 * get config properties from global property tree (or use default)
		 * Note: Instance has no name -> use the owners name is used as instance identifier
		 */
		VPPP_PROPERTY_GET("InstrMemoryProxy." + owner.name(), "clock_cycle_period", sc_core::sc_time,
		                  prop_clock_cycle_period);
		VPPP_PROPERTY_GET("InstrMemoryProxy." + owner.name(), "access_clock_cycles", uint64_t,
		                  prop_access_clock_cycles);
		access_delay = prop_clock_cycle_period * prop_access_clock_cycles;
	}

	virtual uint32_t load_instr(uint64_t pc) override {
		quantum_keeper.inc(access_delay);
		return dmi.load<uint32_t>(pc);
	}
};

template <typename T_RVX_ISS, typename T_sxlen_t, typename T_uxlen_t>
struct CombinedMemoryInterface_T : public sc_core::sc_module,
                                   public instr_memory_if,
                                   public data_memory_if_T<T_sxlen_t, T_uxlen_t>,
                                   public mmu_memory_if {
	/* config properties */
	sc_core::sc_time prop_clock_cycle_period = sc_core::sc_time(10, sc_core::SC_NS);
	unsigned int prop_dmi_access_clock_cycles = 4;

	T_RVX_ISS &iss;
	std::shared_ptr<bus_lock_if> bus_lock;
	uint64_t lr_addr = 0;

	tlm_utils::simple_initiator_socket<CombinedMemoryInterface_T> isock;
	tlm_utils::tlm_quantumkeeper &quantum_keeper;

	// optionally add DMI ranges for optimization
	sc_core::sc_time dmi_access_delay;
	std::vector<MemoryDMI> dmi_ranges;

	tlm::tlm_generic_payload trans;
	tlm_ext_initiator *ext;
	tlm_ext_pbmt *ext_pbmt;

	MMU_T<T_RVX_ISS> *mmu;
	PMA pma;

	bool last_access_was_dmi = false;
	void *last_dmi_page_host_addr = nullptr;

	CombinedMemoryInterface_T(sc_core::sc_module_name, T_RVX_ISS &owner, MMU_T<T_RVX_ISS> *mmu = nullptr)
	    : iss(owner), quantum_keeper(iss.quantum_keeper), mmu(mmu) {
		/*
		 * get config properties from global property tree (or use default)
		 * Note: Instance has no name -> use the owners name is used as instance identifier
		 */
		VPPP_PROPERTY_GET("CombinedMemoryInterface." + owner.name(), "clock_cycle_period", sc_core::sc_time,
		                  prop_clock_cycle_period);
		VPPP_PROPERTY_GET("CombinedMemoryInterface." + owner.name(), "dmi_access_clock_cycles", uint64_t,
		                  prop_dmi_access_clock_cycles);

		dmi_access_delay = prop_clock_cycle_period * prop_dmi_access_clock_cycles;

		ext = new tlm_ext_initiator(&owner);  // tlm_generic_payload frees all extension objects in destructor,
		                                      // therefore dynamic allocation is needed
		ext_pbmt = new tlm_ext_pbmt();
		trans.set_extension<tlm_ext_initiator>(ext);
		trans.set_extension<tlm_ext_pbmt>(ext_pbmt);
	}

	PMA &get_pma() {
		return pma;
	}

	const PMA &get_pma() const {
		return pma;
	}

	uint64_t v2p(uint64_t vaddr, MemoryAccessType type) override {
		if (mmu == nullptr)
			return vaddr;
		return mmu->translate_virtual_to_physical_addr(vaddr, type);
	}

	static uint8_t pma_memory_type_to_tlm(PmaMemoryType memory_type) {
		switch (memory_type) {
			case PmaMemoryType::MainMemory:
				return tlm_ext_pbmt::PMA_MEMORY_MAIN;
			case PmaMemoryType::IO:
				return tlm_ext_pbmt::PMA_MEMORY_IO;
		}
		return tlm_ext_pbmt::PMA_MEMORY_MAIN;
	}

	static uint8_t pma_ordering_to_tlm(PmaOrdering ordering) {
		switch (ordering) {
			case PmaOrdering::RVWMO:
				return tlm_ext_pbmt::PMA_ORDER_RVWMO;
			case PmaOrdering::RVTSO:
				return tlm_ext_pbmt::PMA_ORDER_RVTSO;
			case PmaOrdering::RelaxedIO:
				return tlm_ext_pbmt::PMA_ORDER_RELAXED_IO;
			case PmaOrdering::StrongIO:
				return tlm_ext_pbmt::PMA_ORDER_STRONG_IO;
		}
		return tlm_ext_pbmt::PMA_ORDER_RVWMO;
	}

	static void raise_access_fault_for_access(MemoryAccessType type, uint64_t paddr) {
		switch (type) {
			case FETCH:
				raise_trap(EXC_INSTR_ACCESS_FAULT, paddr);
				break;
			case LOAD:
				raise_trap(EXC_LOAD_ACCESS_FAULT, paddr);
				break;
			case STORE:
				raise_trap(EXC_STORE_AMO_ACCESS_FAULT, paddr);
				break;
		}
	}

	[[noreturn]] static void raise_pma_bus_error(const pma_check_result &result, uint64_t addr) {
		throw std::runtime_error("[pma] bus error at " + std::to_string(addr) + ": " + result.reason);
	}

	[[noreturn]] static void raise_pma_timeout(const pma_check_result &result, uint64_t addr) {
		throw std::runtime_error("[pma] timeout at " + std::to_string(addr) + ": " + result.reason);
	}

	pma_check_result check_pma_or_raise(uint64_t paddr, uint64_t size, MemoryAccessType type, bool atomic = false,
	                                    bool lr_sc = false, bool page_table_access = false, bool implicit = false,
	                                    MemoryAccessType fault_type = LOAD, uint64_t fault_vaddr = 0,
	                                    PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) const {
		pma_check_request request;
		request.paddr = paddr;
		request.size = size;
		request.access_type = type;
		request.atomic = atomic;
		request.lr_sc = lr_sc;
		request.amo_class = amo_class;
		request.page_table_access = page_table_access;
		request.implicit = implicit;

		const pma_check_result result = pma.check_access(request);
		if (result.allowed) {
			return result;
		}

		const MemoryAccessType report_type = page_table_access ? fault_type : type;
		const uint64_t report_addr = page_table_access ? fault_vaddr : paddr;
		switch (result.failure_response) {
			case PmaFailureResponse::PreciseAccessFault:
				/* 规范 3.6 将可精确 trap 的 PMA violation 归为 access-fault，不是 page-fault。 */
				raise_access_fault_for_access(report_type, report_addr);
				break;
			case PmaFailureResponse::BusError:
				raise_pma_bus_error(result, report_addr);
				break;
			case PmaFailureResponse::Timeout:
				raise_pma_timeout(result, report_addr);
				break;
		}

		return result;
	}

	inline void _do_transaction(tlm::tlm_command cmd, uint64_t addr, uint8_t *data, unsigned num_bytes,
	                            uint8_t pbmt = tlm_ext_pbmt::PMA,
	                            const pma_attributes &pma_attr = pma_attributes()) {
		trans.set_command(cmd);
		trans.set_address(addr);
		trans.set_data_ptr(data);
		trans.set_data_length(num_bytes);
		trans.set_response_status(tlm::TLM_OK_RESPONSE);
		ext_pbmt->pbmt = pbmt;
		ext_pbmt->set_pma(pma_memory_type_to_tlm(pma_attr.memory_type), pma_ordering_to_tlm(pma_attr.ordering),
		                  pma_attr.cacheable, pma_attr.coherent);

		/* ensure, that quantum_keeper value of ISS is up-to-date */
		iss.commit_cycles();

		/* update quantum values by transaction delay */

		sc_core::sc_time local_delay = quantum_keeper.get_local_time();

		isock->b_transport(trans, local_delay);

		quantum_keeper.set(local_delay);

		if (trans.is_response_error()) {
			if (iss.trace_enabled())
				std::cout << "WARNING: core memory transaction failed for address 0x" << std::hex << addr << std::dec
				          << " -> raise trap" << std::endl;
			if (cmd == tlm::TLM_READ_COMMAND)
				raise_trap(EXC_LOAD_PAGE_FAULT, addr);
			else if (cmd == tlm::TLM_WRITE_COMMAND)
				raise_trap(EXC_STORE_AMO_PAGE_FAULT, addr);
			else
				throw std::runtime_error("TLM command must be read or write");
		}
	}

	template <typename T>
	inline T _raw_load_data(uint64_t addr, uint8_t pbmt = tlm_ext_pbmt::PMA, MemoryAccessType type = LOAD,
	                        bool atomic = false, bool lr_sc = false, bool page_table_access = false,
	                        bool implicit = false, MemoryAccessType page_fault_type = LOAD,
	                        uint64_t page_fault_vaddr = 0, PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) {
		const pma_check_result pma_result =
		    check_pma_or_raise(addr, sizeof(T), type, atomic, lr_sc, page_table_access, implicit, page_fault_type,
		                       page_fault_vaddr, amo_class);

		// NOTE: a DMI load will not context switch (SystemC) and not modify the memory, hence should be able to
		// postpone the lock after the dmi access
		bus_lock->wait_for_access_rights(iss.get_hart_id());

		T ans;

		for (auto &e : dmi_ranges) {
			if (e.contains(addr)) {
				quantum_keeper.inc(dmi_access_delay);
				ans = e.load<T>(addr);

				/* save the host address of the start of the 4KiB page containing addr */
				last_access_was_dmi = true;
				last_dmi_page_host_addr = e.get_mem_ptr_to_global_addr<T>(addr & ~0xFFF);

				return ans;
			}
		}

		_do_transaction(tlm::TLM_READ_COMMAND, addr, (uint8_t *)&ans, sizeof(T), pbmt, pma_result.attributes);

		/*
		 * A transaction may lead to a context switch. The other context may issue transaction handled via dmi.
		 * i.e.: When we come back to this context, it is possible that we end up with last_access_was_dmi set to true!
		 * Solution: set last_access_was_dmi to false AFTER _do_transaction
		 */
		last_access_was_dmi = false;

		return ans;
	}

	template <typename T>
	inline void _raw_store_data(uint64_t addr, T value, uint8_t pbmt = tlm_ext_pbmt::PMA, bool atomic = false,
	                            bool lr_sc = false, bool page_table_access = false, bool implicit = false,
	                            MemoryAccessType page_fault_type = STORE, uint64_t page_fault_vaddr = 0,
	                            PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) {
		const pma_check_result pma_result =
		    check_pma_or_raise(addr, sizeof(T), STORE, atomic, lr_sc, page_table_access, implicit, page_fault_type,
		                       page_fault_vaddr, amo_class);

		bus_lock->wait_for_access_rights(iss.get_hart_id());

		for (auto &e : dmi_ranges) {
			if (e.contains(addr)) {
				quantum_keeper.inc(dmi_access_delay);
				e.store(addr, value);

				/* save the host address of the start of the 4KiB page containing addr */
				last_access_was_dmi = true;
				last_dmi_page_host_addr = e.get_mem_ptr_to_global_addr<T>(addr & ~0xFFF);

				atomic_unlock();
				return;
			}
		}

		_do_transaction(tlm::TLM_WRITE_COMMAND, addr, (uint8_t *)&value, sizeof(T), pbmt, pma_result.attributes);
		atomic_unlock();

		/* see comment in _raw_load_data */
		last_access_was_dmi = false;
	}

	template <typename T>
	inline T _load_data(uint64_t addr, bool atomic = false, bool lr_sc = false,
	                    PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) {
		const uint64_t paddr = v2p(addr, LOAD);
		const uint8_t pbmt = mmu ? mmu->get_last_translation_pbmt() : static_cast<uint8_t>(tlm_ext_pbmt::PMA);
		return _raw_load_data<T>(paddr, pbmt, LOAD, atomic, lr_sc, false, false, LOAD, 0, amo_class);
	}

	template <typename T>
	inline void _store_data(uint64_t addr, T value, bool atomic = false, bool lr_sc = false,
	                        PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) {
		const uint64_t paddr = v2p(addr, STORE);
		const uint8_t pbmt = mmu ? mmu->get_last_translation_pbmt() : static_cast<uint8_t>(tlm_ext_pbmt::PMA);
		_raw_store_data(paddr, value, pbmt, atomic, lr_sc, false, false, STORE, 0, amo_class);
	}

	uint64_t mmu_load_pte64(uint64_t addr, MemoryAccessType fault_type, uint64_t fault_vaddr) override {
		return _raw_load_data<uint64_t>(addr, tlm_ext_pbmt::PMA, LOAD, false, false, true, true, fault_type,
		                                fault_vaddr);
	}
	uint64_t mmu_load_pte32(uint64_t addr, MemoryAccessType fault_type, uint64_t fault_vaddr) override {
		return _raw_load_data<uint32_t>(addr, tlm_ext_pbmt::PMA, LOAD, false, false, true, true, fault_type,
		                                fault_vaddr);
	}
	void mmu_store_pte32(uint64_t addr, uint32_t value, MemoryAccessType fault_type, uint64_t fault_vaddr) override {
		_raw_store_data(addr, value, tlm_ext_pbmt::PMA, false, false, true, true, fault_type, fault_vaddr);
	}

	void flush_tlb() override {
		if (mmu == nullptr) {
			return;
		}
		mmu->flush_tlb();
	}

	uint32_t load_instr(uint64_t addr) override {
		/*
		 * We have support for RISC-V Compressed C instructions.
		 * As a consequence we might have misaligned 32 bit fetches.
		 *
		 * This is a problem if
		 *  1. the fetch happens at a page boundary AND
		 *  2. virtual memory management is used AND
		 *  3. the two involved pages are not stored sequentially in physical memory.
		 *
		 * e.g.
		 * <pc>		<instr>		<pc_increment>
		 * 0x1FF8	fmul ...	4
		 * 0x1FFC	c.lw ...	2 (compressed)
		 * 0x1FFE	fmax ...	4				<-- not 32 bit aligned @ page boundary
		 * 0x2002	fmin ...	4				<-- not 32 bit aligned
		 *
		 * -> A non-aligned 32 bit fetch at a 4KiB page boundary has to be split!
		 *
		 * TODO: Maybe it would be more realistic to split all misaligned 32 bit accesses
		 * in two 16 bit (-> no misaligned accesses on the bus) -> Future work
		 *
		 */
		if ((addr & 0xFFF) == 0xFFE) {
			const uint64_t hi_paddr = v2p(addr + 2, FETCH);
			const uint8_t hi_pbmt = mmu ? mmu->get_last_translation_pbmt() : static_cast<uint8_t>(tlm_ext_pbmt::PMA);
			const uint64_t lo_paddr = v2p(addr + 0, FETCH);
			const uint8_t lo_pbmt = mmu ? mmu->get_last_translation_pbmt() : static_cast<uint8_t>(tlm_ext_pbmt::PMA);
			return (_raw_load_data<uint16_t>(hi_paddr, hi_pbmt, FETCH, false, false, false, true) << 16) |
			       (_raw_load_data<uint16_t>(lo_paddr, lo_pbmt, FETCH, false, false, false, true) << 0);
		}

		const uint64_t paddr = v2p(addr, FETCH);
		const uint8_t pbmt = mmu ? mmu->get_last_translation_pbmt() : static_cast<uint8_t>(tlm_ext_pbmt::PMA);
		return _raw_load_data<uint32_t>(paddr, pbmt, FETCH, false, false, false, true);
	}

	template <typename T>
	T _atomic_load_data(uint64_t addr, PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) {
		bus_lock->lock(iss.get_hart_id());
		return _load_data<T>(addr, true, false, amo_class);
	}
	template <typename T>
	void _atomic_store_data(uint64_t addr, T value, PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) {
		/*
		 * This is sometimes triggerd when running RV64 debian and apt.
		 * TODO: check cause and fix
		 */
		if (unlikely(!bus_lock->is_locked(iss.get_hart_id()))) {
			std::cerr << "CombinedMemoryInterface: WARNING: bus not locked in _atomic_store_data (please report to VP "
			             "developers)"
			          << std::endl;
		}
		_store_data(addr, value, true, false, amo_class);
	}
	template <typename T>
	T _atomic_load_reserved_data(uint64_t addr) {
		bus_lock->lock(iss.get_hart_id());
		lr_addr = addr;
		return _load_data<T>(addr, true, true, PmaAmoClass::AMONone);
	}
	template <typename T>
	void _check_store_conditional_pma(uint64_t addr) {
		const uint64_t paddr = v2p(addr, STORE);
		check_pma_or_raise(paddr, sizeof(T), STORE, true, true, false, false, STORE, 0, PmaAmoClass::AMONone);
	}
	template <typename T>
	bool _atomic_store_conditional_data(uint64_t addr, T value) {
		/* According to the RISC-V ISA, an implementation can fail each LR/SC sequence that does not satisfy the forward
		 * progress semantic.
		 * The lock is established by the LR instruction and the lock is kept while forward progress is maintained. */
		_check_store_conditional_pma<T>(addr);
		if (bus_lock->is_locked(iss.get_hart_id())) {
			if (addr == lr_addr) {
				_store_data(addr, value, true, true, PmaAmoClass::AMONone);
				return true;
			}
			atomic_unlock();
		}
		return false;
	}

	int64_t load_double(uint64_t addr) override {
		return _load_data<int64_t>(addr);
	}
	T_sxlen_t load_word(uint64_t addr) override {
		return _load_data<int32_t>(addr);
	}
	T_sxlen_t load_half(uint64_t addr) override {
		return _load_data<int16_t>(addr);
	}
	T_sxlen_t load_byte(uint64_t addr) override {
		return _load_data<int8_t>(addr);
	}

	/* unused on RV32 */
	T_uxlen_t load_uword(uint64_t addr) override {
		return _load_data<uint32_t>(addr);
	}

	T_uxlen_t load_uhalf(uint64_t addr) override {
		return _load_data<uint16_t>(addr);
	}
	T_uxlen_t load_ubyte(uint64_t addr) override {
		return _load_data<uint8_t>(addr);
	}

	void store_double(uint64_t addr, uint64_t value) override {
		_store_data(addr, value);
	}
	void store_word(uint64_t addr, uint32_t value) override {
		_store_data(addr, value);
	}
	void store_half(uint64_t addr, uint16_t value) override {
		_store_data(addr, value);
	}
	void store_byte(uint64_t addr, uint8_t value) override {
		_store_data(addr, value);
	}

	T_sxlen_t atomic_load_word(uint64_t addr, PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) override {
		return _atomic_load_data<int32_t>(addr, amo_class);
	}
	void atomic_store_word(uint64_t addr, uint32_t value,
	                       PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) override {
		_atomic_store_data(addr, value, amo_class);
	}
	T_sxlen_t atomic_load_reserved_word(uint64_t addr) override {
		return _atomic_load_reserved_data<int32_t>(addr);
	}
	bool atomic_store_conditional_word(uint64_t addr, uint32_t value) override {
		return _atomic_store_conditional_data(addr, value);
	}

	/* unused on RV32 */
	int64_t atomic_load_double(uint64_t addr, PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) override {
		return _atomic_load_data<int64_t>(addr, amo_class);
	}
	void atomic_store_double(uint64_t addr, uint64_t value,
	                         PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic) override {
		_atomic_store_data(addr, value, amo_class);
	}
	int64_t atomic_load_reserved_double(uint64_t addr) override {
		return _atomic_load_reserved_data<int64_t>(addr);
	}
	bool atomic_store_conditional_double(uint64_t addr, uint64_t value) override {
		return _atomic_store_conditional_data(addr, value);
	}

	void atomic_unlock() override {
		bus_lock->unlock(iss.get_hart_id());
	}

	inline bool is_bus_locked() override {
		return bus_lock->is_locked();
	}

	/* see comment in data_memory_if_T */
	void *get_last_dmi_page_host_addr() override {
		if (!last_access_was_dmi) {
			return nullptr;
		}
		return last_dmi_page_host_addr;
	}
};

#endif /* RISCV_ISA_MEM_H */
