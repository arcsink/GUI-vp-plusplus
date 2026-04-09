#pragma once

#include <assert.h>
#include <stdint.h>

#include <stdexcept>
#include <unordered_map>

#include "core/common/core_defs.h"
#include "core/common/trap.h"
#include "util/common.h"

namespace rv64 {

constexpr unsigned FS_OFF = 0b00;
constexpr unsigned FS_INITIAL = 0b01;
constexpr unsigned FS_CLEAN = 0b10;
constexpr unsigned FS_DIRTY = 0b11;

inline bool is_valid_privilege_level(PrivilegeLevel mode) {
	return mode == MachineMode || mode == SupervisorMode || mode == UserMode;
}

struct csr_64 {
	union reg {
		uint64_t val = 0;
	} reg;
};

struct csr_misa_64 : csr_misa {
	csr_misa_64() {
		init();
	}

	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned extensions : 26;
			uint64_t wiri : 36;
			unsigned mxl : 2;
		} fields;
	} reg;

	bool has_C_extension() {
		return reg.fields.extensions & C;
	}

	bool has_E_base_isa() {
		return reg.fields.extensions & E;
	}

	bool has_hypervisor_extension() {
		return reg.fields.extensions & H;
	}

	bool has_user_mode_extension() {
		return reg.fields.extensions & U;
	}

	bool has_supervisor_mode_extension() {
		return reg.fields.extensions & S;
	}

	void init() {
		// supported extensions will be set by the iss according to RV_ISA_Config
		reg.fields.extensions = 0;
		// RV64;
		reg.fields.mxl = 2;
	}
};

struct csr_mvendorid {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned offset : 7;
			unsigned bank : 25;
			unsigned _unused : 32;
		} fields;
	} reg;
};

struct csr_mstatus {
	csr_mstatus() {
		// hardwire to 64 bit mode for now
		reg.fields.sxl = 2;
		reg.fields.uxl = 2;
	}

	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned uie : 1;
			unsigned sie : 1;
			unsigned wpri1 : 1;
			unsigned mie : 1;
			unsigned upie : 1;
			unsigned spie : 1;
			unsigned ube : 1;
			unsigned mpie : 1;
			unsigned spp : 1;
			unsigned vs : 2;
			unsigned mpp : 2;
			unsigned fs : 2;
			unsigned xs : 2;
			unsigned mprv : 1;
			unsigned sum : 1;
			unsigned mxr : 1;
			unsigned tvm : 1;
			unsigned tw : 1;
			unsigned tsr : 1;
			unsigned wpri4 : 9;
			unsigned uxl : 2;
			unsigned sxl : 2;
			unsigned sbe : 1;
			unsigned mbe : 1;
			unsigned gva : 1;    // H Extension fdw: guest virtual address marker on trap into M-mode
			unsigned mpv : 1;    // H Extension fdw: previous virtualization state saved for MRET
			unsigned wpri5 : 23;
			unsigned sd : 1;
		} fields;
	} reg;
};

struct csr_mtvec {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned mode : 2;   // WARL
			uint64_t base : 62;  // WARL
		} fields;
	} reg;

	uint64_t get_base_address() {
		return reg.fields.base << 2;
	}

	enum Mode { Direct = 0, Vectored = 1 };

	void checked_write(uint64_t val) {
		reg.val = val;
		if (reg.fields.mode >= 1)
			reg.fields.mode = 0;
	}
};

struct csr_mie {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned usie : 1;
			unsigned ssie : 1;
			unsigned wpri1 : 1;
			unsigned msie : 1;

			unsigned utie : 1;
			unsigned stie : 1;
			unsigned wpri2 : 1;
			unsigned mtie : 1;

			unsigned ueie : 1;
			unsigned seie : 1;
			unsigned wpri3 : 1;
			unsigned meie : 1;

			uint64_t wpri4 : 52;
		} fields;
	} reg;
};

struct csr_mip {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned usip : 1;
			unsigned ssip : 1;
			unsigned wiri1 : 1;
			unsigned msip : 1;

			unsigned utip : 1;
			unsigned stip : 1;
			unsigned wiri2 : 1;
			unsigned mtip : 1;

			unsigned ueip : 1;
			unsigned seip : 1;
			unsigned wiri3 : 1;
			unsigned meip : 1;

			uint64_t wiri4 : 52;
		} fields;
	} reg;
};

struct csr_mepc {
	union reg {
		uint64_t val = 0;
	} reg;
};

struct csr_mcause {
	union reg {
		uint64_t val = 0;
		struct fields {
			uint64_t exception_code : 63;  // WLRL
			unsigned interrupt : 1;
		} fields;
	} reg;
};

struct csr_mcounteren {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned CY : 1;
			unsigned TM : 1;
			unsigned IR : 1;
			uint64_t reserved : 61;
		} fields;
	} reg;
};

struct csr_menvcfg {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned fiom : 1;
			unsigned wpri1 : 1;
			unsigned lpe : 1;      // H Extension fdw: menvcfg.LPE influences virtualization landing-pad behavior
			unsigned sse : 1;
			unsigned cbie : 2;
			unsigned cbcfe : 1;
			unsigned cbze : 1;
			uint64_t wpri2 : 24;
			unsigned pmm : 2;      // H Extension fdw: menvcfg.PMM provides pointer masking state shared with H paths
			uint64_t wpri3 : 25;
			unsigned dte : 1;      // H Extension fdw: menvcfg.DTE controls double-trap behavior visible to H flows
			unsigned cde : 1;
			unsigned adue : 1;
			unsigned pbmte : 1;
			unsigned stce : 1;
		} fields;
	} reg;
};

struct csr_mcountinhibit {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned CY : 1;
			unsigned zero : 1;
			unsigned IR : 1;
			uint64_t reserved : 61;
		} fields;
	} reg;
};

struct csr_pmpcfg {
	union reg {
		uint64_t val = 0;
	} reg;
};

struct csr_pmpaddr {
	union reg {
		uint64_t val = 0;
		struct fields {
			uint64_t address : 54;
			unsigned zero : 10;
		} fields;
	} reg;

	unsigned get_address() {
		return reg.fields.address << 2;
	}
};

struct csr_satp {
	union reg {
		uint64_t val = 0;
		struct fields {
			uint64_t ppn : 44;   // WARL
			unsigned asid : 16;  // WARL
			unsigned mode : 4;   // WARL
		} fields;
	} reg;
};

struct csr_hstatus {
	csr_hstatus() {
		reg.fields.vsxl = 2;  // H Extension fdw: hardwire VSXL to RV64 for now
	}

	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned wpri1 : 5;
			unsigned vsbe : 1;    // H Extension fdw: VS-mode endianness
			unsigned gva : 1;     // H Extension fdw: guest virtual address indicator for HS trap
			unsigned spv : 1;     // H Extension fdw: previous virtualization state for SRET
			unsigned spvp : 1;    // H Extension fdw: previous virtual privilege for HLV/HSV and SRET
			unsigned hu : 1;      // H Extension fdw: permit HLV/HSV from U-mode
			unsigned wpri2 : 2;
			unsigned vgein : 6;   // H Extension fdw: guest external interrupt number
			unsigned wpri3 : 2;
			unsigned vtvm : 1;    // H Extension fdw: virtual TVM trap control
			unsigned vtw : 1;     // H Extension fdw: virtual TW trap control
			unsigned vtsr : 1;    // H Extension fdw: virtual TSR trap control
			unsigned wpri4 : 1;
			unsigned hukte : 1;   // H Extension fdw: permit ukte in virtualized U-mode path
			unsigned wpri5 : 7;
			unsigned vsxl : 2;    // H Extension fdw: VS XLEN
			uint64_t wpri6 : 14;
			unsigned hupmm : 2;   // H Extension fdw: U-mode pointer masking mode for virtualization
			uint64_t wpri7 : 14;
		} fields;
	} reg;
};

struct csr_hgatp {
	union reg {
		uint64_t val = 0;
		struct fields {
			uint64_t ppn : 44;   // H Extension fdw: G-stage root page number
			unsigned vmid : 14;  // H Extension fdw: guest address-space identifier
			unsigned wpri : 2;
			unsigned mode : 4;   // H Extension fdw: G-stage translation mode (Off/Sv39x4/Sv48x4/Sv57x4)
		} fields;
	} reg;
};

struct csr_vsstatus {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned uie : 1;
			unsigned sie : 1;
			unsigned wpri1 : 2;
			unsigned upie : 1;
			unsigned spie : 1;
			unsigned ube : 1;
			unsigned wpri2 : 1;
			unsigned spp : 1;
			unsigned vs : 2;
			unsigned wpri3 : 2;
			unsigned fs : 2;
			unsigned xs : 2;
			unsigned wpri4 : 1;
			unsigned sum : 1;    // H Extension fdw: VS supervisor may access U pages when set
			unsigned mxr : 1;    // H Extension fdw: VS loads may treat execute-only pages as readable
			unsigned wpri5 : 12;
			unsigned uxl : 2;    // H Extension fdw: VS-visible UXL field for guest U-mode
			uint64_t wpri6 : 29;
			unsigned sd : 1;
		} fields;
	} reg;
};

/* Actually 32 bit large, but use 64 value for consistency and simply set the read/write mask accordingly. */
struct csr_fcsr {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned fflags : 5;
			unsigned frm : 3;
			unsigned reserved : 24;
			unsigned _ : 32;
		} fields;
		struct fflags {
			unsigned NX : 1;  // invalid operation
			unsigned UF : 1;  // divide by zero
			unsigned OF : 1;  // overflow
			unsigned DZ : 1;  // underflow
			unsigned NV : 1;  // inexact
		} fflags;
	} reg;
};

struct csr_vtype {
	union reg {
		uint64_t val = 0x8000000000000000;  // vill=1 at reset
		struct fields {
			unsigned vlmul : 3;
			unsigned vsew : 3;
			unsigned vta : 1;
			unsigned vma : 1;
			uint64_t reserved : 55;
			unsigned vill : 1;
		} fields;
	} reg;
};

struct csr_vl {
	union reg {
		uint64_t val = 0;
	} reg;
};

struct csr_vstart {
	union reg {
		uint64_t val = 0;
	} reg;
};

struct csr_vxrm {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned vxrm : 2;
			uint64_t zero : 62;
		} fields;
	} reg;
};

struct csr_vxsat {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned vxsat : 1;
			uint64_t zero : 63;
		} fields;
	} reg;
};

struct csr_vcsr {
	union reg {
		uint64_t val = 0;
		struct fields {
			unsigned vxsat : 1;
			unsigned vxrm : 2;
			uint64_t reserved : 61;
		} fields;
	} reg;
};

struct csr_vlenb {
	union reg {
		uint64_t val = 0;
	} reg;
};

namespace csr {
template <typename T>
inline bool is_bitset(T &csr, unsigned bitpos) {
	return csr.reg.val & (1 << bitpos);
}

constexpr uint64_t MIE_MASK = 0b1111011101110;  // H Extension fdw: expose the architected M/S/VS/SGE interrupt-enable bits through mie
constexpr uint64_t SIE_MASK = 0b001100110011;
constexpr uint64_t UIE_MASK = 0b000100010001;

constexpr uint64_t MIP_WRITE_MASK = 0b1000100110;  // H Extension fdw: mip writable bits are SSIP/VSSIP/STIP/SEIP in the current non-AIA model
constexpr uint64_t MIP_READ_MASK = MIE_MASK;
constexpr uint64_t SIP_MASK = 0b11;
constexpr uint64_t UIP_MASK = 0b1;

constexpr uint64_t MEDELEG_MASK = 0b1011111111111111;  // H Extension fdw: allow delegating virtual-supervisor-ecall (cause 10) through medeleg
constexpr uint64_t MIDELEG_MASK = MIE_MASK;
constexpr uint64_t HEDELEG_MASK = 0b1011000111111111;
constexpr uint64_t HIDELEG_MASK = 0b001000100010;
constexpr uint64_t HVIP_MASK = 0b10001000100;   // H Extension fdw: keep HS virtual interrupt-pending bits on the architected VS legacy interrupt positions (VSSIP/VSTIP/VSEIP)
constexpr uint64_t HVIEN_MASK = 0b10001000100;  // H Extension fdw: keep HS virtual interrupt-enable bits on the architected VS legacy interrupt positions (VSSIP/VSTIP/VSEIP)
constexpr uint64_t MSTATUS_GVA = 0x4000000000ULL;  // H Extension fdw: mstatus.GVA
constexpr uint64_t MSTATUS_MPV = 0x8000000000ULL;
constexpr uint64_t HSTATUS_VSBE = 0x00000020ULL;      // H Extension fdw: hstatus.VSBE
constexpr uint64_t HSTATUS_GVA = 0x00000040ULL;       // H Extension fdw: hstatus.GVA
constexpr uint64_t HSTATUS_SPV = 0x00000080ULL;
constexpr uint64_t HSTATUS_SPVP = 0x00000100ULL;
constexpr uint64_t HSTATUS_HU = 0x00000200ULL;        // H Extension fdw: hstatus.HU
constexpr uint64_t HSTATUS_VGEIN = 0x0003F000ULL;     // H Extension fdw: hstatus.VGEIN
constexpr uint64_t HSTATUS_VTVM = 0x00100000ULL;      // H Extension fdw: hstatus.VTVM
constexpr uint64_t HSTATUS_VTW = 0x00200000ULL;       // H Extension fdw: hstatus.VTW
constexpr uint64_t HSTATUS_VTSR = 0x00400000ULL;      // H Extension fdw: hstatus.VTSR
constexpr uint64_t HSTATUS_HUKTE = 0x01000000ULL;     // H Extension fdw: hstatus.HUKTE
constexpr uint64_t HSTATUS_VSXL = 0x300000000ULL;     // H Extension fdw: hstatus.VSXL
constexpr uint64_t HSTATUS_HUPMM = 0x3000000000000ULL;  // H Extension fdw: hstatus.HUPMM
constexpr uint64_t HSTATUS_READ_MASK = HSTATUS_VSBE | HSTATUS_GVA | HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_HU |
                                       HSTATUS_VGEIN | HSTATUS_VTVM | HSTATUS_VTW | HSTATUS_VTSR | HSTATUS_HUKTE |
                                       HSTATUS_VSXL | HSTATUS_HUPMM;  // H Extension fdw: expose architecturally visible hstatus bits
constexpr uint64_t HSTATUS_WRITE_MASK = HSTATUS_GVA | HSTATUS_SPV | HSTATUS_SPVP | HSTATUS_HU | HSTATUS_VTVM |
                                        HSTATUS_VTW | HSTATUS_VTSR | HSTATUS_HUKTE |
                                        HSTATUS_HUPMM;  // H Extension fdw: keep VSXL hardwired while allowing software-managed control bits
constexpr uint64_t HENVCFG_FIOM = 0x0000000000000001ULL;   // H Extension fdw: henvcfg.FIOM
constexpr uint64_t HENVCFG_LPE = 0x0000000000000004ULL;    // H Extension fdw: henvcfg.LPE
constexpr uint64_t HENVCFG_SSE = 0x0000000000000008ULL;    // H Extension fdw: henvcfg.SSE
constexpr uint64_t HENVCFG_CBIE = 0x0000000000000030ULL;   // H Extension fdw: henvcfg.CBIE
constexpr uint64_t HENVCFG_CBCFE = 0x0000000000000040ULL;  // H Extension fdw: henvcfg.CBCFE
constexpr uint64_t HENVCFG_CBZE = 0x0000000000000080ULL;   // H Extension fdw: henvcfg.CBZE
constexpr uint64_t HENVCFG_PMM = 0x0000000300000000ULL;    // H Extension fdw: henvcfg.PMM
constexpr uint64_t HENVCFG_STCE = 0x8000000000000000ULL;   // H Extension fdw: henvcfg.STCE shares the same architected bit position as menvcfg.STCE
constexpr uint64_t HENVCFG_BASE_MASK = HENVCFG_FIOM | HENVCFG_LPE | HENVCFG_SSE | HENVCFG_CBIE |
                                       HENVCFG_CBCFE | HENVCFG_CBZE |
                                       HENVCFG_PMM;  // H Extension fdw: locally modeled henvcfg bits that are not gated by menvcfg
constexpr uint64_t MENVCFG_DTE = 0x0800000000000000ULL;    // H Extension fdw: menvcfg.DTE
constexpr uint64_t MENVCFG_CDE = 0x1000000000000000ULL;    // H Extension fdw: menvcfg.CDE
constexpr uint64_t MENVCFG_ADUE = 0x2000000000000000ULL;   // H Extension fdw: menvcfg.ADUE
constexpr uint64_t MENVCFG_PBMTE = 0x4000000000000000ULL;  // H Extension fdw: menvcfg.PBMTE
constexpr uint64_t MENVCFG_STCE = 0x8000000000000000ULL;   // H Extension fdw: menvcfg.STCE
constexpr uint64_t HENVCFG_GATED_MASK = MENVCFG_DTE | MENVCFG_CDE | MENVCFG_ADUE | MENVCFG_PBMTE |
                                        MENVCFG_STCE |
                                        HENVCFG_SSE;  // H Extension fdw: henvcfg bits whose visibility follows menvcfg policy

constexpr uint64_t MTVEC_MASK = ~2;

constexpr uint64_t MCOUNTEREN_MASK = 0b111;
constexpr uint64_t MCOUNTINHIBIT_MASK = 0b101;

constexpr uint64_t SEDELEG_MASK = 0b1011000111111111;
constexpr uint64_t SIDELEG_MASK = MIDELEG_MASK;

constexpr uint64_t MSTATUS_WRITE_MASK = 0b1000000000000000000000000000000000000000011111111111111110111011 | MSTATUS_GVA |
                                        MSTATUS_MPV;  // H Extension fdw: expose mstatus.GVA/MPV in RV64
constexpr uint64_t MSTATUS_READ_MASK = 0b1000000000000000000000000011111100000000011111111111111111111011 | MSTATUS_GVA |
                                       MSTATUS_MPV;  // H Extension fdw: expose mstatus.GVA/MPV in RV64
constexpr uint64_t SSTATUS_WRITE_MASK = 0b1000000000000000000000000000000000000000000011011110011100110011;
constexpr uint64_t SSTATUS_READ_MASK = 0b1000000000000000000000000000001100000000000011011110011101110011;
constexpr uint64_t VSSTATUS_WRITE_MASK = SSTATUS_WRITE_MASK;  // H Extension fdw: keep VSSTATUS write visibility explicit even while it matches SSTATUS today
constexpr uint64_t VSSTATUS_READ_MASK = SSTATUS_READ_MASK;    // H Extension fdw: keep VSSTATUS read visibility explicit even while it matches SSTATUS today
constexpr uint64_t USTATUS_WRITE_MASK = 0b0000000000000000000000000000000000000000000000000000000000010001;
constexpr uint64_t USTATUS_READ_MASK = 0b0000000000000000000000000000000000000000000000000000000001010001;

constexpr uint64_t PMPADDR_MASK = 0b0000000000111111111111111111111111111111111111111111111111111111;

constexpr uint64_t SATP_MASK = 0b1111000000000000000011111111111111111111111111111111111111111111;
constexpr uint64_t SATP_MODE = 0b1111000000000000000000000000000000000000000000000000000000000000;
constexpr uint64_t HGATP_PPN_MASK = 0x00000FFFFFFFFFFFULL;   // H Extension fdw: HGATP.PPN for RV64
constexpr uint64_t HGATP_VMID_MASK = 0x03FFF00000000000ULL;  // H Extension fdw: HGATP.VMID for RV64
constexpr uint64_t HGATP_MODE_MASK = 0xF000000000000000ULL;  // H Extension fdw: HGATP.MODE for RV64
constexpr uint64_t HGATP_MASK = HGATP_PPN_MASK | HGATP_VMID_MASK | HGATP_MODE_MASK;
constexpr uint64_t HGATP_MODE_OFF = 0x0ULL;                  // H Extension fdw: disable G-stage translation
constexpr uint64_t HGATP_MODE_SV39X4 = 0x8ULL;               // H Extension fdw: RV64 G-stage Sv39x4
constexpr uint64_t HGATP_MODE_SV48X4 = 0x9ULL;               // H Extension fdw: RV64 G-stage Sv48x4
constexpr uint64_t HGATP_MODE_SV57X4 = 0xAULL;               // H Extension fdw: RV64 G-stage Sv57x4

constexpr uint64_t FCSR_MASK = 0b11111111;
constexpr uint64_t VTYPE_MASK = 0b1000000000000000000000000000000000000000000000000000000011111111;
constexpr uint64_t VXRM_MASK = 0b11;
constexpr uint64_t VXSAT_MASK = 0b1;
constexpr uint64_t VCSR_MASK = 0b111;
// 64 bit timer csrs
constexpr unsigned CYCLE_ADDR = 0xC00;
constexpr unsigned TIME_ADDR = 0xC01;
constexpr unsigned INSTRET_ADDR = 0xC02;

// shadows for the above CSRs
constexpr unsigned MCYCLE_ADDR = 0xB00;
constexpr unsigned MTIME_ADDR = 0xB01;
constexpr unsigned MINSTRET_ADDR = 0xB02;

// 32 bit machine CSRs
constexpr unsigned MVENDORID_ADDR = 0xF11;
constexpr unsigned MARCHID_ADDR = 0xF12;
constexpr unsigned MIMPID_ADDR = 0xF13;
constexpr unsigned MHARTID_ADDR = 0xF14;

constexpr unsigned MSTATUS_ADDR = 0x300;
constexpr unsigned MISA_ADDR = 0x301;
constexpr unsigned MEDELEG_ADDR = 0x302;
constexpr unsigned MIDELEG_ADDR = 0x303;
constexpr unsigned MIE_ADDR = 0x304;
constexpr unsigned MTVEC_ADDR = 0x305;
constexpr unsigned MCOUNTEREN_ADDR = 0x306;
constexpr unsigned MENVCFG_ADDR = 0x30A;
constexpr unsigned MCOUNTINHIBIT_ADDR = 0x320;

constexpr unsigned MSCRATCH_ADDR = 0x340;
constexpr unsigned MEPC_ADDR = 0x341;
constexpr unsigned MCAUSE_ADDR = 0x342;
constexpr unsigned MTVAL_ADDR = 0x343;
constexpr unsigned MIP_ADDR = 0x344;
constexpr unsigned MTINST_ADDR = 0x34A;
constexpr unsigned MTVAL2_ADDR = 0x34B;

constexpr unsigned PMPCFG0_ADDR = 0x3A0;
constexpr unsigned PMPCFG1_ADDR = 0x3A1;
constexpr unsigned PMPCFG2_ADDR = 0x3A2;
constexpr unsigned PMPCFG3_ADDR = 0x3A3;

constexpr unsigned PMPADDR0_ADDR = 0x3B0;
constexpr unsigned PMPADDR1_ADDR = 0x3B1;
constexpr unsigned PMPADDR2_ADDR = 0x3B2;
constexpr unsigned PMPADDR3_ADDR = 0x3B3;
constexpr unsigned PMPADDR4_ADDR = 0x3B4;
constexpr unsigned PMPADDR5_ADDR = 0x3B5;
constexpr unsigned PMPADDR6_ADDR = 0x3B6;
constexpr unsigned PMPADDR7_ADDR = 0x3B7;
constexpr unsigned PMPADDR8_ADDR = 0x3B8;
constexpr unsigned PMPADDR9_ADDR = 0x3B9;
constexpr unsigned PMPADDR10_ADDR = 0x3BA;
constexpr unsigned PMPADDR11_ADDR = 0x3BB;
constexpr unsigned PMPADDR12_ADDR = 0x3BC;
constexpr unsigned PMPADDR13_ADDR = 0x3BD;
constexpr unsigned PMPADDR14_ADDR = 0x3BE;
constexpr unsigned PMPADDR15_ADDR = 0x3BF;

// 32 bit supervisor CSRs
constexpr unsigned SSTATUS_ADDR = 0x100;
constexpr unsigned SEDELEG_ADDR = 0x102;
constexpr unsigned SIDELEG_ADDR = 0x103;
constexpr unsigned SIE_ADDR = 0x104;
constexpr unsigned STVEC_ADDR = 0x105;
constexpr unsigned SCOUNTEREN_ADDR = 0x106;
constexpr unsigned SENVCFG_ADDR = 0x10A;      // H Extension fdw: align extended S-mode envcfg CSR address with QEMU
constexpr unsigned SCOUNTINHIBIT_ADDR = 0x120;  // H Extension fdw: align extended S-mode count inhibit CSR address with QEMU
constexpr unsigned SSCRATCH_ADDR = 0x140;
constexpr unsigned SEPC_ADDR = 0x141;
constexpr unsigned SCAUSE_ADDR = 0x142;
constexpr unsigned STVAL_ADDR = 0x143;
constexpr unsigned SIP_ADDR = 0x144;
constexpr unsigned STIMECMP_ADDR = 0x14D;     // H Extension fdw: align S-mode timer compare CSR address with QEMU
constexpr unsigned SISELECT_ADDR = 0x150;     // H Extension fdw: align S-mode indirect selector CSR address with QEMU
constexpr unsigned SIREG_ADDR = 0x151;        // H Extension fdw: align S-mode indirect register window with QEMU
constexpr unsigned SIREG2_ADDR = 0x152;       // H Extension fdw: align S-mode indirect register alias with QEMU
constexpr unsigned SIREG3_ADDR = 0x153;       // H Extension fdw: align S-mode indirect register alias with QEMU
constexpr unsigned SIPH_ADDR = 0x154;         // H Extension fdw: align RV32 AIA S-mode high-half pending CSR address with QEMU
constexpr unsigned SIREG4_ADDR = 0x155;       // H Extension fdw: align S-mode indirect register alias with QEMU
constexpr unsigned SIREG5_ADDR = 0x156;       // H Extension fdw: align S-mode indirect register alias with QEMU
constexpr unsigned SIREG6_ADDR = 0x157;       // H Extension fdw: align S-mode indirect register alias with QEMU
constexpr unsigned STOPEI_ADDR = 0x15C;       // H Extension fdw: align S-mode top external interrupt CSR address with QEMU
constexpr unsigned SATP_ADDR = 0x180;
constexpr unsigned STOPI_ADDR = 0xDB0;        // H Extension fdw: align S-mode top pending interrupt CSR address with QEMU
constexpr unsigned VSSTATUS_ADDR = 0x200;
constexpr unsigned VSTVEC_ADDR = 0x205;
constexpr unsigned VSSCRATCH_ADDR = 0x240;
constexpr unsigned VSEPC_ADDR = 0x241;
constexpr unsigned VSCAUSE_ADDR = 0x242;
constexpr unsigned VSTVAL_ADDR = 0x243;
constexpr unsigned VSATP_ADDR = 0x280;
constexpr unsigned VSISELECT_ADDR = 0x250;    // H Extension fdw: align VS indirect selector CSR address with QEMU
constexpr unsigned VSIREG_ADDR = 0x251;       // H Extension fdw: align VS indirect register window with QEMU
constexpr unsigned VSIREG2_ADDR = 0x252;      // H Extension fdw: align VS indirect register alias with QEMU
constexpr unsigned VSIREG3_ADDR = 0x253;      // H Extension fdw: align VS indirect register alias with QEMU
constexpr unsigned VSIREG4_ADDR = 0x255;      // H Extension fdw: align VS indirect register alias with QEMU
constexpr unsigned VSIREG5_ADDR = 0x256;      // H Extension fdw: align VS indirect register alias with QEMU
constexpr unsigned VSIREG6_ADDR = 0x257;      // H Extension fdw: align VS indirect register alias with QEMU
constexpr unsigned VSTOPEI_ADDR = 0x25C;      // H Extension fdw: align VS top external interrupt CSR address with QEMU
constexpr unsigned VSTOPI_ADDR = 0xEB0;       // H Extension fdw: align VS top pending interrupt CSR address with QEMU

// hypervisor CSRs
constexpr unsigned HSTATUS_ADDR = 0x600;
constexpr unsigned HEDELEG_ADDR = 0x602;
constexpr unsigned HIDELEG_ADDR = 0x603;
constexpr unsigned HIE_ADDR = 0x604;          // H Extension fdw: align HS interrupt-enable CSR address with QEMU
constexpr unsigned HTIMEDELTA_ADDR = 0x605;   // H Extension fdw: align HS time-delta CSR address with QEMU
constexpr unsigned HCOUNTEREN_ADDR = 0x606;   // H Extension fdw: align HS counter-enable CSR address with QEMU
constexpr unsigned HGEIE_ADDR = 0x607;        // H Extension fdw: align HS guest-external interrupt enable CSR address with QEMU
constexpr unsigned HVIEN_ADDR = 0x608;        // H Extension fdw: align HS virtual interrupt-enable CSR address with QEMU
constexpr unsigned HENVCFG_ADDR = 0x60A;      // H Extension fdw: align HS envcfg CSR address with QEMU
constexpr unsigned HGATP_ADDR = 0x680;
constexpr unsigned HTVAL_ADDR = 0x643;        // H Extension fdw: align HS trap value CSR address with QEMU
constexpr unsigned HIP_ADDR = 0x644;          // H Extension fdw: align HS interrupt-pending CSR address with QEMU
constexpr unsigned HVIP_ADDR = 0x645;         // H Extension fdw: align HS virtual interrupt-pending CSR address with QEMU
constexpr unsigned HTINST_ADDR = 0x64A;       // H Extension fdw: align HS trap instruction CSR address with QEMU
constexpr unsigned HGEIP_ADDR = 0xE12;        // H Extension fdw: align HS guest-external interrupt pending CSR address with QEMU
constexpr unsigned VSIE_ADDR = 0x204;         // H Extension fdw: align VS interrupt-enable CSR address with QEMU
constexpr unsigned VSIP_ADDR = 0x244;         // H Extension fdw: align VS interrupt-pending CSR address with QEMU
constexpr unsigned VSTIMECMP_ADDR = 0x24D;    // H Extension fdw: align VS timer compare CSR address with QEMU

// 32 bit user CSRs
constexpr unsigned USTATUS_ADDR = 0x000;
constexpr unsigned UIE_ADDR = 0x004;
constexpr unsigned UTVEC_ADDR = 0x005;
constexpr unsigned USCRATCH_ADDR = 0x040;
constexpr unsigned UEPC_ADDR = 0x041;
constexpr unsigned UCAUSE_ADDR = 0x042;
constexpr unsigned UTVAL_ADDR = 0x043;
constexpr unsigned UIP_ADDR = 0x044;

// floating point CSRs
constexpr unsigned FFLAGS_ADDR = 0x001;
constexpr unsigned FRM_ADDR = 0x002;
constexpr unsigned FCSR_ADDR = 0x003;

// performance counters
constexpr unsigned HPMCOUNTER3_ADDR = 0xC03;
constexpr unsigned HPMCOUNTER4_ADDR = 0xC04;
constexpr unsigned HPMCOUNTER5_ADDR = 0xC05;
constexpr unsigned HPMCOUNTER6_ADDR = 0xC06;
constexpr unsigned HPMCOUNTER7_ADDR = 0xC07;
constexpr unsigned HPMCOUNTER8_ADDR = 0xC08;
constexpr unsigned HPMCOUNTER9_ADDR = 0xC09;
constexpr unsigned HPMCOUNTER10_ADDR = 0xC0A;
constexpr unsigned HPMCOUNTER11_ADDR = 0xC0B;
constexpr unsigned HPMCOUNTER12_ADDR = 0xC0C;
constexpr unsigned HPMCOUNTER13_ADDR = 0xC0D;
constexpr unsigned HPMCOUNTER14_ADDR = 0xC0E;
constexpr unsigned HPMCOUNTER15_ADDR = 0xC0F;
constexpr unsigned HPMCOUNTER16_ADDR = 0xC10;
constexpr unsigned HPMCOUNTER17_ADDR = 0xC11;
constexpr unsigned HPMCOUNTER18_ADDR = 0xC12;
constexpr unsigned HPMCOUNTER19_ADDR = 0xC13;
constexpr unsigned HPMCOUNTER20_ADDR = 0xC14;
constexpr unsigned HPMCOUNTER21_ADDR = 0xC15;
constexpr unsigned HPMCOUNTER22_ADDR = 0xC16;
constexpr unsigned HPMCOUNTER23_ADDR = 0xC17;
constexpr unsigned HPMCOUNTER24_ADDR = 0xC18;
constexpr unsigned HPMCOUNTER25_ADDR = 0xC19;
constexpr unsigned HPMCOUNTER26_ADDR = 0xC1A;
constexpr unsigned HPMCOUNTER27_ADDR = 0xC1B;
constexpr unsigned HPMCOUNTER28_ADDR = 0xC1C;
constexpr unsigned HPMCOUNTER29_ADDR = 0xC1D;
constexpr unsigned HPMCOUNTER30_ADDR = 0xC1E;
constexpr unsigned HPMCOUNTER31_ADDR = 0xC1F;

constexpr unsigned HPMCOUNTER3H_ADDR = 0xC83;
constexpr unsigned HPMCOUNTER4H_ADDR = 0xC84;
constexpr unsigned HPMCOUNTER5H_ADDR = 0xC85;
constexpr unsigned HPMCOUNTER6H_ADDR = 0xC86;
constexpr unsigned HPMCOUNTER7H_ADDR = 0xC87;
constexpr unsigned HPMCOUNTER8H_ADDR = 0xC88;
constexpr unsigned HPMCOUNTER9H_ADDR = 0xC89;
constexpr unsigned HPMCOUNTER10H_ADDR = 0xC8A;
constexpr unsigned HPMCOUNTER11H_ADDR = 0xC8B;
constexpr unsigned HPMCOUNTER12H_ADDR = 0xC8C;
constexpr unsigned HPMCOUNTER13H_ADDR = 0xC8D;
constexpr unsigned HPMCOUNTER14H_ADDR = 0xC8E;
constexpr unsigned HPMCOUNTER15H_ADDR = 0xC8F;
constexpr unsigned HPMCOUNTER16H_ADDR = 0xC90;
constexpr unsigned HPMCOUNTER17H_ADDR = 0xC91;
constexpr unsigned HPMCOUNTER18H_ADDR = 0xC92;
constexpr unsigned HPMCOUNTER19H_ADDR = 0xC93;
constexpr unsigned HPMCOUNTER20H_ADDR = 0xC94;
constexpr unsigned HPMCOUNTER21H_ADDR = 0xC95;
constexpr unsigned HPMCOUNTER22H_ADDR = 0xC96;
constexpr unsigned HPMCOUNTER23H_ADDR = 0xC97;
constexpr unsigned HPMCOUNTER24H_ADDR = 0xC98;
constexpr unsigned HPMCOUNTER25H_ADDR = 0xC99;
constexpr unsigned HPMCOUNTER26H_ADDR = 0xC9A;
constexpr unsigned HPMCOUNTER27H_ADDR = 0xC9B;
constexpr unsigned HPMCOUNTER28H_ADDR = 0xC9C;
constexpr unsigned HPMCOUNTER29H_ADDR = 0xC9D;
constexpr unsigned HPMCOUNTER30H_ADDR = 0xC9E;
constexpr unsigned HPMCOUNTER31H_ADDR = 0xC9F;

constexpr unsigned MHPMCOUNTER3_ADDR = 0xB03;
constexpr unsigned MHPMCOUNTER4_ADDR = 0xB04;
constexpr unsigned MHPMCOUNTER5_ADDR = 0xB05;
constexpr unsigned MHPMCOUNTER6_ADDR = 0xB06;
constexpr unsigned MHPMCOUNTER7_ADDR = 0xB07;
constexpr unsigned MHPMCOUNTER8_ADDR = 0xB08;
constexpr unsigned MHPMCOUNTER9_ADDR = 0xB09;
constexpr unsigned MHPMCOUNTER10_ADDR = 0xB0A;
constexpr unsigned MHPMCOUNTER11_ADDR = 0xB0B;
constexpr unsigned MHPMCOUNTER12_ADDR = 0xB0C;
constexpr unsigned MHPMCOUNTER13_ADDR = 0xB0D;
constexpr unsigned MHPMCOUNTER14_ADDR = 0xB0E;
constexpr unsigned MHPMCOUNTER15_ADDR = 0xB0F;
constexpr unsigned MHPMCOUNTER16_ADDR = 0xB10;
constexpr unsigned MHPMCOUNTER17_ADDR = 0xB11;
constexpr unsigned MHPMCOUNTER18_ADDR = 0xB12;
constexpr unsigned MHPMCOUNTER19_ADDR = 0xB13;
constexpr unsigned MHPMCOUNTER20_ADDR = 0xB14;
constexpr unsigned MHPMCOUNTER21_ADDR = 0xB15;
constexpr unsigned MHPMCOUNTER22_ADDR = 0xB16;
constexpr unsigned MHPMCOUNTER23_ADDR = 0xB17;
constexpr unsigned MHPMCOUNTER24_ADDR = 0xB18;
constexpr unsigned MHPMCOUNTER25_ADDR = 0xB19;
constexpr unsigned MHPMCOUNTER26_ADDR = 0xB1A;
constexpr unsigned MHPMCOUNTER27_ADDR = 0xB1B;
constexpr unsigned MHPMCOUNTER28_ADDR = 0xB1C;
constexpr unsigned MHPMCOUNTER29_ADDR = 0xB1D;
constexpr unsigned MHPMCOUNTER30_ADDR = 0xB1E;
constexpr unsigned MHPMCOUNTER31_ADDR = 0xB1F;

constexpr unsigned MHPMCOUNTER3H_ADDR = 0xB83;
constexpr unsigned MHPMCOUNTER4H_ADDR = 0xB84;
constexpr unsigned MHPMCOUNTER5H_ADDR = 0xB85;
constexpr unsigned MHPMCOUNTER6H_ADDR = 0xB86;
constexpr unsigned MHPMCOUNTER7H_ADDR = 0xB87;
constexpr unsigned MHPMCOUNTER8H_ADDR = 0xB88;
constexpr unsigned MHPMCOUNTER9H_ADDR = 0xB89;
constexpr unsigned MHPMCOUNTER10H_ADDR = 0xB8A;
constexpr unsigned MHPMCOUNTER11H_ADDR = 0xB8B;
constexpr unsigned MHPMCOUNTER12H_ADDR = 0xB8C;
constexpr unsigned MHPMCOUNTER13H_ADDR = 0xB8D;
constexpr unsigned MHPMCOUNTER14H_ADDR = 0xB8E;
constexpr unsigned MHPMCOUNTER15H_ADDR = 0xB8F;
constexpr unsigned MHPMCOUNTER16H_ADDR = 0xB90;
constexpr unsigned MHPMCOUNTER17H_ADDR = 0xB91;
constexpr unsigned MHPMCOUNTER18H_ADDR = 0xB92;
constexpr unsigned MHPMCOUNTER19H_ADDR = 0xB93;
constexpr unsigned MHPMCOUNTER20H_ADDR = 0xB94;
constexpr unsigned MHPMCOUNTER21H_ADDR = 0xB95;
constexpr unsigned MHPMCOUNTER22H_ADDR = 0xB96;
constexpr unsigned MHPMCOUNTER23H_ADDR = 0xB97;
constexpr unsigned MHPMCOUNTER24H_ADDR = 0xB98;
constexpr unsigned MHPMCOUNTER25H_ADDR = 0xB99;
constexpr unsigned MHPMCOUNTER26H_ADDR = 0xB9A;
constexpr unsigned MHPMCOUNTER27H_ADDR = 0xB9B;
constexpr unsigned MHPMCOUNTER28H_ADDR = 0xB9C;
constexpr unsigned MHPMCOUNTER29H_ADDR = 0xB9D;
constexpr unsigned MHPMCOUNTER30H_ADDR = 0xB9E;
constexpr unsigned MHPMCOUNTER31H_ADDR = 0xB9F;

constexpr unsigned MHPMEVENT3_ADDR = 0x323;
constexpr unsigned MHPMEVENT4_ADDR = 0x324;
constexpr unsigned MHPMEVENT5_ADDR = 0x325;
constexpr unsigned MHPMEVENT6_ADDR = 0x326;
constexpr unsigned MHPMEVENT7_ADDR = 0x327;
constexpr unsigned MHPMEVENT8_ADDR = 0x328;
constexpr unsigned MHPMEVENT9_ADDR = 0x329;
constexpr unsigned MHPMEVENT10_ADDR = 0x32A;
constexpr unsigned MHPMEVENT11_ADDR = 0x32B;
constexpr unsigned MHPMEVENT12_ADDR = 0x32C;
constexpr unsigned MHPMEVENT13_ADDR = 0x32D;
constexpr unsigned MHPMEVENT14_ADDR = 0x32E;
constexpr unsigned MHPMEVENT15_ADDR = 0x32F;
constexpr unsigned MHPMEVENT16_ADDR = 0x330;
constexpr unsigned MHPMEVENT17_ADDR = 0x331;
constexpr unsigned MHPMEVENT18_ADDR = 0x332;
constexpr unsigned MHPMEVENT19_ADDR = 0x333;
constexpr unsigned MHPMEVENT20_ADDR = 0x334;
constexpr unsigned MHPMEVENT21_ADDR = 0x335;
constexpr unsigned MHPMEVENT22_ADDR = 0x336;
constexpr unsigned MHPMEVENT23_ADDR = 0x337;
constexpr unsigned MHPMEVENT24_ADDR = 0x338;
constexpr unsigned MHPMEVENT25_ADDR = 0x339;
constexpr unsigned MHPMEVENT26_ADDR = 0x33A;
constexpr unsigned MHPMEVENT27_ADDR = 0x33B;
constexpr unsigned MHPMEVENT28_ADDR = 0x33C;
constexpr unsigned MHPMEVENT29_ADDR = 0x33D;
constexpr unsigned MHPMEVENT30_ADDR = 0x33E;
constexpr unsigned MHPMEVENT31_ADDR = 0x33F;

// vector CSRs
constexpr unsigned VSTART_ADDR = 0x008;
constexpr unsigned VXSAT_ADDR = 0x009;
constexpr unsigned VXRM_ADDR = 0x00A;
constexpr unsigned VCSR_ADDR = 0x00F;
constexpr unsigned VL_ADDR = 0xC20;
constexpr unsigned VTYPE_ADDR = 0xC21;
constexpr unsigned VLENB_ADDR = 0xC22;
}  // namespace csr

struct csr_table {
	csr_64 cycle;
	csr_64 time;
	csr_64 instret;

	csr_mvendorid mvendorid;
	csr_64 marchid;
	csr_64 mimpid;
	csr_64 mhartid;

	csr_mstatus mstatus;
	csr_misa_64 misa;
	csr_64 medeleg;
	csr_64 mideleg;
	csr_mie mie;
	csr_mtvec mtvec;
	csr_mcounteren mcounteren;
	csr_menvcfg menvcfg;    // H Extension fdw: M-mode execution environment configuration
	csr_mcountinhibit mcountinhibit;

	csr_64 mscratch;
	csr_mepc mepc;
	csr_mcause mcause;
	csr_64 mtval;
	csr_mip mip;
	csr_64 mtinst;          // H Extension fdw: M-mode trap instruction value
	csr_64 mtval2;          // H Extension fdw: M-mode second trap value

	// pmp configuration
	std::array<csr_pmpaddr, 16> pmpaddr;
	std::array<csr_pmpcfg, 2> pmpcfg;

	// supervisor csrs (please note: some are already covered by the machine mode csrs, i.e. sstatus, sie and sip, and
	// some are required but have the same fields, hence the machine mode classes are used)
	csr_64 sedeleg;
	csr_64 sideleg;
	csr_mtvec stvec;
	csr_mcounteren scounteren;
	csr_64 senvcfg;          // H Extension fdw: S-mode execution environment configuration
	csr_64 scountinhibit;    // H Extension fdw: S-mode counter inhibit state
	csr_64 sscratch;
	csr_mepc sepc;
	csr_mcause scause;
	csr_64 stval;
	csr_64 stimecmp;         // H Extension fdw: S-mode timer compare value
	csr_64 siselect;         // H Extension fdw: S-mode indirect CSR selector window
	csr_64 sireg;
	csr_64 sireg2;
	csr_64 sireg3;
	csr_64 sireg4;
	csr_64 sireg5;
	csr_64 sireg6;
	csr_64 stopei;
	csr_64 stopi;
	csr_satp satp;
	csr_vsstatus vsstatus;
	csr_mtvec vstvec;
	csr_64 vsscratch;
	csr_64 vsepc;            // H Extension fdw: keep VSEPC as a plain XLEN-wide trap return address register
	csr_mcause vscause;
	csr_64 vstval;
	csr_satp vsatp;
	csr_64 vsie;             // H Extension fdw: VS interrupt-enable state
	csr_64 vsip;             // H Extension fdw: VS interrupt-pending state
	csr_64 vstimecmp;        // H Extension fdw: VS timer compare state
	csr_64 vsiselect;        // H Extension fdw: VS indirect CSR selector window
	csr_64 vsireg;
	csr_64 vsireg2;
	csr_64 vsireg3;
	csr_64 vsireg4;
	csr_64 vsireg5;
	csr_64 vsireg6;
	csr_64 vstopei;
	csr_64 vstopi;
	csr_hstatus hstatus;
	csr_64 hedeleg;
	csr_64 hideleg;
	csr_64 hie;              // H Extension fdw: HS interrupt-enable state
	csr_64 hip;              // H Extension fdw: HS interrupt-pending state
	csr_64 hvip;             // H Extension fdw: HS virtual interrupt-pending state
	csr_64 hvien;            // H Extension fdw: HS virtual interrupt-enable state
	csr_mcounteren hcounteren;  // H Extension fdw: HS counter-enable state
	csr_64 hgeie;            // H Extension fdw: HS guest external interrupt enable state
	csr_64 htval;            // H Extension fdw: HS trap value state
	csr_64 htinst;           // H Extension fdw: HS trap instruction state
	csr_64 hgeip;            // H Extension fdw: HS guest external interrupt pending state
	csr_hgatp hgatp;
	csr_64 htimedelta;       // H Extension fdw: HS virtual time delta
	csr_menvcfg henvcfg;     // H Extension fdw: HS execution environment configuration

	// user csrs (see above comment)
	csr_mtvec utvec;
	csr_64 uscratch;
	csr_mepc uepc;
	csr_mcause ucause;
	csr_64 utval;

	csr_fcsr fcsr;

	csr_vstart vstart;
	csr_vxsat vxsat;
	csr_vxrm vxrm;
	csr_vcsr vcsr;
	csr_vtype vtype;
	csr_vl vl;
	csr_vl vlenb;

	std::unordered_map<unsigned, uint64_t *> register_mapping;

	csr_table() {
		using namespace csr;

		register_mapping[CYCLE_ADDR] = &cycle.reg.val;
		register_mapping[TIME_ADDR] = &time.reg.val;
		register_mapping[INSTRET_ADDR] = &instret.reg.val;
		register_mapping[MCYCLE_ADDR] = &cycle.reg.val;
		register_mapping[MTIME_ADDR] = &time.reg.val;
		register_mapping[MINSTRET_ADDR] = &instret.reg.val;

		register_mapping[MVENDORID_ADDR] = &mvendorid.reg.val;
		register_mapping[MARCHID_ADDR] = &marchid.reg.val;
		register_mapping[MIMPID_ADDR] = &mimpid.reg.val;
		register_mapping[MHARTID_ADDR] = &mhartid.reg.val;

		register_mapping[MSTATUS_ADDR] = &mstatus.reg.val;
		register_mapping[MISA_ADDR] = &misa.reg.val;
		register_mapping[MEDELEG_ADDR] = &medeleg.reg.val;
		register_mapping[MIDELEG_ADDR] = &mideleg.reg.val;
		register_mapping[MIE_ADDR] = &mie.reg.val;
		register_mapping[MTVEC_ADDR] = &mtvec.reg.val;
		register_mapping[MCOUNTEREN_ADDR] = &mcounteren.reg.val;
		register_mapping[MENVCFG_ADDR] = &menvcfg.reg.val;
		register_mapping[MCOUNTINHIBIT_ADDR] = &mcountinhibit.reg.val;

		register_mapping[MSCRATCH_ADDR] = &mscratch.reg.val;
		register_mapping[MEPC_ADDR] = &mepc.reg.val;
		register_mapping[MCAUSE_ADDR] = &mcause.reg.val;
		register_mapping[MTVAL_ADDR] = &mtval.reg.val;
		register_mapping[MIP_ADDR] = &mip.reg.val;
		register_mapping[MTINST_ADDR] = &mtinst.reg.val;
		register_mapping[MTVAL2_ADDR] = &mtval2.reg.val;

		for (unsigned i = 0; i < 16; ++i) register_mapping[PMPADDR0_ADDR + i] = &pmpaddr[i].reg.val;

		for (unsigned i = 0; i < 4; ++i) register_mapping[PMPCFG0_ADDR + i] = &pmpcfg[i].reg.val;

		register_mapping[SEDELEG_ADDR] = &sedeleg.reg.val;
		register_mapping[SIDELEG_ADDR] = &sideleg.reg.val;
		register_mapping[STVEC_ADDR] = &stvec.reg.val;
		register_mapping[SCOUNTEREN_ADDR] = &scounteren.reg.val;
		register_mapping[SENVCFG_ADDR] = &senvcfg.reg.val;
		register_mapping[SCOUNTINHIBIT_ADDR] = &scountinhibit.reg.val;
		register_mapping[SSCRATCH_ADDR] = &sscratch.reg.val;
		register_mapping[SEPC_ADDR] = &sepc.reg.val;
		register_mapping[SCAUSE_ADDR] = &scause.reg.val;
		register_mapping[STVAL_ADDR] = &stval.reg.val;
		register_mapping[STIMECMP_ADDR] = &stimecmp.reg.val;  // H Extension fdw: expose stimecmp through the CSR table so Sstc probing sees a real register
		register_mapping[SISELECT_ADDR] = &siselect.reg.val;
		register_mapping[SIREG_ADDR] = &sireg.reg.val;
		register_mapping[SIREG2_ADDR] = &sireg2.reg.val;
		register_mapping[SIREG3_ADDR] = &sireg3.reg.val;
		register_mapping[SIREG4_ADDR] = &sireg4.reg.val;
		register_mapping[SIREG5_ADDR] = &sireg5.reg.val;
		register_mapping[SIREG6_ADDR] = &sireg6.reg.val;
		register_mapping[STOPEI_ADDR] = &stopei.reg.val;
		register_mapping[STOPI_ADDR] = &stopi.reg.val;
		register_mapping[SATP_ADDR] = &satp.reg.val;
		register_mapping[VSSTATUS_ADDR] = &vsstatus.reg.val;
		register_mapping[VSTVEC_ADDR] = &vstvec.reg.val;
		register_mapping[VSSCRATCH_ADDR] = &vsscratch.reg.val;
		register_mapping[VSEPC_ADDR] = &vsepc.reg.val;
		register_mapping[VSCAUSE_ADDR] = &vscause.reg.val;
		register_mapping[VSTVAL_ADDR] = &vstval.reg.val;
		register_mapping[VSATP_ADDR] = &vsatp.reg.val;
		register_mapping[VSIE_ADDR] = &vsie.reg.val;
		register_mapping[VSIP_ADDR] = &vsip.reg.val;
		register_mapping[VSTIMECMP_ADDR] = &vstimecmp.reg.val;  // H Extension fdw: expose vstimecmp through the CSR table so VS timer compare has a real backing register
		register_mapping[VSISELECT_ADDR] = &vsiselect.reg.val;
		register_mapping[VSIREG_ADDR] = &vsireg.reg.val;
		register_mapping[VSIREG2_ADDR] = &vsireg2.reg.val;
		register_mapping[VSIREG3_ADDR] = &vsireg3.reg.val;
		register_mapping[VSIREG4_ADDR] = &vsireg4.reg.val;
		register_mapping[VSIREG5_ADDR] = &vsireg5.reg.val;
		register_mapping[VSIREG6_ADDR] = &vsireg6.reg.val;
		register_mapping[VSTOPEI_ADDR] = &vstopei.reg.val;
		register_mapping[VSTOPI_ADDR] = &vstopi.reg.val;
		register_mapping[HSTATUS_ADDR] = &hstatus.reg.val;
		register_mapping[HEDELEG_ADDR] = &hedeleg.reg.val;
		register_mapping[HIDELEG_ADDR] = &hideleg.reg.val;
		register_mapping[HIE_ADDR] = &hie.reg.val;
		register_mapping[HIP_ADDR] = &hip.reg.val;
		register_mapping[HVIP_ADDR] = &hvip.reg.val;
		register_mapping[HVIEN_ADDR] = &hvien.reg.val;
		register_mapping[HCOUNTEREN_ADDR] = &hcounteren.reg.val;
		register_mapping[HGEIE_ADDR] = &hgeie.reg.val;
		register_mapping[HTVAL_ADDR] = &htval.reg.val;
		register_mapping[HTINST_ADDR] = &htinst.reg.val;
		register_mapping[HGEIP_ADDR] = &hgeip.reg.val;
		register_mapping[HGATP_ADDR] = &hgatp.reg.val;
		register_mapping[HTIMEDELTA_ADDR] = &htimedelta.reg.val;
		register_mapping[HENVCFG_ADDR] = &henvcfg.reg.val;

		register_mapping[UTVEC_ADDR] = &utvec.reg.val;
		register_mapping[USCRATCH_ADDR] = &uscratch.reg.val;
		register_mapping[UEPC_ADDR] = &uepc.reg.val;
		register_mapping[UCAUSE_ADDR] = &ucause.reg.val;
		register_mapping[UTVAL_ADDR] = &utval.reg.val;

		register_mapping[FCSR_ADDR] = &fcsr.reg.val;

		register_mapping[VSTART_ADDR] = &vstart.reg.val;
		register_mapping[VXSAT_ADDR] = &vxsat.reg.val;
		register_mapping[VXRM_ADDR] = &vxrm.reg.val;
		register_mapping[VCSR_ADDR] = &vcsr.reg.val;
		register_mapping[VL_ADDR] = &vl.reg.val;
		register_mapping[VTYPE_ADDR] = &vtype.reg.val;
		register_mapping[VLENB_ADDR] = &vlenb.reg.val;
	}

	bool is_valid_csr64_addr(unsigned addr) {
		return register_mapping.find(addr) != register_mapping.end();
	}

	void default_write64(unsigned addr, uint64_t value) {
		auto it = register_mapping.find(addr);
		ensure((it != register_mapping.end()) && "validate address before calling this function");
		*it->second = value;
	}

	uint64_t default_read64(unsigned addr) {
		auto it = register_mapping.find(addr);
		ensure((it != register_mapping.end()) && "validate address before calling this function");
		return *it->second;
	}
};

#define SWITCH_CASE_MATCH_ANY_HPMCOUNTER_RV64 \
	case HPMCOUNTER3_ADDR:                    \
	case HPMCOUNTER4_ADDR:                    \
	case HPMCOUNTER5_ADDR:                    \
	case HPMCOUNTER6_ADDR:                    \
	case HPMCOUNTER7_ADDR:                    \
	case HPMCOUNTER8_ADDR:                    \
	case HPMCOUNTER9_ADDR:                    \
	case HPMCOUNTER10_ADDR:                   \
	case HPMCOUNTER11_ADDR:                   \
	case HPMCOUNTER12_ADDR:                   \
	case HPMCOUNTER13_ADDR:                   \
	case HPMCOUNTER14_ADDR:                   \
	case HPMCOUNTER15_ADDR:                   \
	case HPMCOUNTER16_ADDR:                   \
	case HPMCOUNTER17_ADDR:                   \
	case HPMCOUNTER18_ADDR:                   \
	case HPMCOUNTER19_ADDR:                   \
	case HPMCOUNTER20_ADDR:                   \
	case HPMCOUNTER21_ADDR:                   \
	case HPMCOUNTER22_ADDR:                   \
	case HPMCOUNTER23_ADDR:                   \
	case HPMCOUNTER24_ADDR:                   \
	case HPMCOUNTER25_ADDR:                   \
	case HPMCOUNTER26_ADDR:                   \
	case HPMCOUNTER27_ADDR:                   \
	case HPMCOUNTER28_ADDR:                   \
	case HPMCOUNTER29_ADDR:                   \
	case HPMCOUNTER30_ADDR:                   \
	case HPMCOUNTER31_ADDR:                   \
	case MHPMCOUNTER3_ADDR:                   \
	case MHPMCOUNTER4_ADDR:                   \
	case MHPMCOUNTER5_ADDR:                   \
	case MHPMCOUNTER6_ADDR:                   \
	case MHPMCOUNTER7_ADDR:                   \
	case MHPMCOUNTER8_ADDR:                   \
	case MHPMCOUNTER9_ADDR:                   \
	case MHPMCOUNTER10_ADDR:                  \
	case MHPMCOUNTER11_ADDR:                  \
	case MHPMCOUNTER12_ADDR:                  \
	case MHPMCOUNTER13_ADDR:                  \
	case MHPMCOUNTER14_ADDR:                  \
	case MHPMCOUNTER15_ADDR:                  \
	case MHPMCOUNTER16_ADDR:                  \
	case MHPMCOUNTER17_ADDR:                  \
	case MHPMCOUNTER18_ADDR:                  \
	case MHPMCOUNTER19_ADDR:                  \
	case MHPMCOUNTER20_ADDR:                  \
	case MHPMCOUNTER21_ADDR:                  \
	case MHPMCOUNTER22_ADDR:                  \
	case MHPMCOUNTER23_ADDR:                  \
	case MHPMCOUNTER24_ADDR:                  \
	case MHPMCOUNTER25_ADDR:                  \
	case MHPMCOUNTER26_ADDR:                  \
	case MHPMCOUNTER27_ADDR:                  \
	case MHPMCOUNTER28_ADDR:                  \
	case MHPMCOUNTER29_ADDR:                  \
	case MHPMCOUNTER30_ADDR:                  \
	case MHPMCOUNTER31_ADDR:                  \
	case MHPMEVENT3_ADDR:                     \
	case MHPMEVENT4_ADDR:                     \
	case MHPMEVENT5_ADDR:                     \
	case MHPMEVENT6_ADDR:                     \
	case MHPMEVENT7_ADDR:                     \
	case MHPMEVENT8_ADDR:                     \
	case MHPMEVENT9_ADDR:                     \
	case MHPMEVENT10_ADDR:                    \
	case MHPMEVENT11_ADDR:                    \
	case MHPMEVENT12_ADDR:                    \
	case MHPMEVENT13_ADDR:                    \
	case MHPMEVENT14_ADDR:                    \
	case MHPMEVENT15_ADDR:                    \
	case MHPMEVENT16_ADDR:                    \
	case MHPMEVENT17_ADDR:                    \
	case MHPMEVENT18_ADDR:                    \
	case MHPMEVENT19_ADDR:                    \
	case MHPMEVENT20_ADDR:                    \
	case MHPMEVENT21_ADDR:                    \
	case MHPMEVENT22_ADDR:                    \
	case MHPMEVENT23_ADDR:                    \
	case MHPMEVENT24_ADDR:                    \
	case MHPMEVENT25_ADDR:                    \
	case MHPMEVENT26_ADDR:                    \
	case MHPMEVENT27_ADDR:                    \
	case MHPMEVENT28_ADDR:                    \
	case MHPMEVENT29_ADDR:                    \
	case MHPMEVENT30_ADDR:                    \
	case MHPMEVENT31_ADDR

}  // namespace rv64
