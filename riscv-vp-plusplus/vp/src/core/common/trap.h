#pragma once

// for rv64_cheriv9 rvfi_dii
#include "../rv64_cheriv9/rvfi-dii/rvfi_dii_trace.h"  // TODO: Should not include rv64 in common!

enum ExceptionCode {
	// interrupt exception codes (mcause)
	EXC_U_SOFTWARE_INTERRUPT = 0,
	EXC_S_SOFTWARE_INTERRUPT = 1,
	EXC_VS_SOFTWARE_INTERRUPT = 2,  // H Extension fdw: HS reports VSSIP as a virtual supervisor software interrupt
	EXC_M_SOFTWARE_INTERRUPT = 3,

	EXC_U_TIMER_INTERRUPT = 4,
	EXC_S_TIMER_INTERRUPT = 5,
	EXC_VS_TIMER_INTERRUPT = 6,  // H Extension fdw: HS reports VSTIP as a virtual supervisor timer interrupt
	EXC_M_TIMER_INTERRUPT = 7,

	EXC_U_EXTERNAL_INTERRUPT = 8,
	EXC_S_EXTERNAL_INTERRUPT = 9,
	EXC_VS_EXTERNAL_INTERRUPT = 10,  // H Extension fdw: HS reports VSEIP as a virtual supervisor external interrupt
	EXC_M_EXTERNAL_INTERRUPT = 11,

	// non-interrupt exception codes (mcause)
	EXC_INSTR_ADDR_MISALIGNED = 0,
	EXC_INSTR_ACCESS_FAULT = 1,
	EXC_ILLEGAL_INSTR = 2,
	EXC_BREAKPOINT = 3,
	EXC_LOAD_ADDR_MISALIGNED = 4,
	EXC_LOAD_ACCESS_FAULT = 5,
	EXC_STORE_AMO_ADDR_MISALIGNED = 6,
	EXC_STORE_AMO_ACCESS_FAULT = 7,

	EXC_ECALL_U_MODE = 8,
	EXC_ECALL_S_MODE = 9,
	EXC_ECALL_VS_MODE = 10,  // H Extension fdw: virtual supervisor ecall raised when ECALL executes in VS
	EXC_ECALL_M_MODE = 11,

	EXC_INSTR_PAGE_FAULT = 12,
	EXC_LOAD_PAGE_FAULT = 13,
	EXC_STORE_AMO_PAGE_FAULT = 15,
	EXC_INSTR_GUEST_PAGE_FAULT = 20,      // H Extension fdw: instruction guest page fault for G-stage translation failures
	EXC_LOAD_GUEST_PAGE_FAULT = 21,       // H Extension fdw: load guest page fault for G-stage translation failures
	EXC_VIRT_INSTRUCTION_FAULT = 22,  // H Extension fdw: virtual-instruction exception for H/V privilege checks
	EXC_STORE_AMO_GUEST_PAGE_FAULT = 23,  // H Extension fdw: store/amo guest page fault for G-stage translation failures

	// CHERIv9 exception codes
	EXC_CHERI_LOAD_FAULT = 26,
	EXC_CHERI_STORE_FAULT = 27,
	EXC_CHERI_FAULT = 28,
};

struct SimulationTrap {
	ExceptionCode reason;
	uint64_t mtval;
	uint64_t tval2 = 0;      // H Extension fdw: second trap value for HS/M hypervisor trap CSRs
	uint64_t tinst = 0;      // H Extension fdw: transformed trap instruction value for HS/M hypervisor trap CSRs
	bool write_gva = false;  // H Extension fdw: whether trap metadata should mark the address as guest virtual
};

inline void raise_trap(ExceptionCode exc, uint64_t mtval) {
	throw SimulationTrap({exc, mtval});
}

inline void raise_trap(ExceptionCode exc, unsigned long mtval, rvfi_dii_trace_t* trace) {
	if (trace != nullptr) {
		// trace->rvfi_dii_pc_wdata = mtval;
		trace->rvfi_dii_trap = 1;
		// trace->rvfi_dii_rs1_addr = 0;
		// trace->rvfi_dii_rs2_addr = 0;
		// trace->rvfi_dii_mem_addr = 0;
	}
	raise_trap(exc, mtval);
}
