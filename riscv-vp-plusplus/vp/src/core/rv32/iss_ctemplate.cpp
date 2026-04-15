/*
 * NEVER BUILD THIS FILE DIRECTLY!!!
 *
 * NOTE RVxx.2: C-style macros
 * concrete implementatins are are derived in iss.cpp (by iss_ctemplate_handle.h)
 * see NOTE RVxx.2 in iss_ctemplate_handle.h for more details
 */

#include "iss.h"

// std::replace
#include <algorithm>
// to save *cout* format setting, see *ISS_CT::show*
#include <boost/io/ios_state.hpp>
// for safe down-cast
#include <boost/lexical_cast.hpp>

#include "util/propertytree.h"

namespace rv32 {

#define VExt VExtension<ISS_CT>

#define RAISE_ILLEGAL_INSTRUCTION() raise_trap(EXC_ILLEGAL_INSTR, instr.data());

#define RD instr.rd()
#define RS1 instr.rs1()
#define RS2 instr.rs2()
#define RS3 instr.rs3()

ISS_CT::ISS_CT(RV_ISA_Config *isa_config, uxlen_t hart_id)
    : isa_config(isa_config), stats(hart_id), v_ext(*this), systemc_name("Core-" + std::to_string(hart_id)) {
	csrs.mhartid.reg.val = hart_id;
	csrs.misa.reg.fields.extensions = isa_config->get_misa_extensions();

	/* get config properties from global property tree (or use default) */
	VPPP_PROPERTY_GET("ISS." + name(), "clock_cycle_period", sc_core::sc_time, prop_clock_cycle_period);

	sc_core::sc_time qt = tlm::tlm_global_quantum::instance().get();

	assert(qt >= prop_clock_cycle_period);
	assert(qt % prop_clock_cycle_period == sc_core::SC_ZERO_TIME);

	/*
	 * NOTE: The cycle model below is a static cycle model -> Value changes at
	 * runtime may have no effect (since cycles may be cached)
	 * If you want to add a dynamic cycle model, you have add this in the
	 * operation implementations below (OPCASE)
	 */

	/* Enable the use of the legacy, hard-coded instruction clock cycle model
	 * If use_legacy_instr_clock_cycle_model is not set (default = 1) or set to >0 explicitly, then the
	 * instr_clock_cycle values of the legacy, hard-coded model are used as default values for instructions
	 * not explicitly in the PropertyTree.
	 */
	uint64_t use_legacy_cycle_model = 1;
	VPPP_PROPERTY_GET("ISS." + name(), "use_legacy_instr_clock_cycle_model", uint64_t, use_legacy_cycle_model);

	/* Default instruction clock cycles
	 * If the legacy model is disabled (use_legacy_cycle_model = 0), this value is used as the default for
	 * instructions where instr_clock_cycles is not explicitly specified in the property tree.
	 */
	uint64_t default_instr_clock_cycles = 1;
	VPPP_PROPERTY_GET("ISS." + name(), "default_instr_clock_cycles", uint64_t, default_instr_clock_cycles);

	/*
	 * Example 1 (override values of legacy model):
	 * ```
	 *     "vppp.ISS.Core-0.LW_instr_clock_cycles": "0x6",
	 *     "vppp.ISS.Core-0.XOR_instr_clock_cycles": "0x1",
	 * ```
	 *  * "use_legacy_instr_clock_cycle_model" is not explicitly set -> defaults
	 *    to 1 (enabled) -> use legacy model values as default
	 *  * LW explicitly set to 6 cycles
	 *  * XOR explicitly set to 1 cycle
	 *  * cycles for all other instructions set according to legacy model (e.g. LB to 4 cycles)
	 *
	 * Example 2 (override values of legacy model):
	 * ```
	 *     "vppp.ISS.Core-0.use_legacy_instr_clock_cycle_model": "0x1",
	 *     "vppp.ISS.Core-0.LW_instr_clock_cycles": "0x6",
	 *     "vppp.ISS.Core-0.XOR_instr_clock_cycles": "0x1",
	 *     "vppp.ISS.Core-0.default_instr_clock_cycles": "0x1",
	 * ```
	 *  * "use_legacy_instr_clock_cycle_model" is explicitly set to 1 -> use
	 *    legacy model values as default -> use legacy model values as default
	 *  * LW explicitly set to 6 cycles (same behavior as in Example 1)
	 *  * XOR explicitly set to 1 cycle (same behavior as in Example 1)
	 *  * cycles for all other instructions set by legacy model (e.g. LB to 4
	 *    cycles) (same behavior as in Example 1)
	 *  * "default_instr_clock_cycles" is ignored
	 *
	 * Example 3 (describe full custom model)
	 * ```
	 *     "vppp.ISS.Core-0.use_legacy_instr_clock_cycle_model": "0x0",
	 *     "vppp.ISS.Core-0.LW_instr_clock_cycles": "0x6",
	 *     "vppp.ISS.Core-0.XOR_instr_clock_cycles": "0x1",
	 *     "vppp.ISS.Core-0.default_instr_clock_cycles": "0x1",
	 * ```
	 *  * "use_legacy_instr_clock_cycle_model" is explicitly set to 0 -> legacy model
	 *    is ignored
	 *  * LW explicitly set to 6 cycles
	 *  * XOR explicitly set to 1 cycle
	 *  * cycles for all other instructions set to value of
	 *    "default_instr_clock_cycles", i.e. to 1
	 */

	/* initialize opMap including timing model */
	for (unsigned int opId = 0; opId < Operation::OpId::NUMBER_OF_OPERATIONS; ++opId) {
		uint64_t instr_clock_cycles = default_instr_clock_cycles;

		/* set instruction id and reset the jump label */
		opMap[opId].opId = (Operation::OpId)opId;
		opMap[opId].labelPtr = nullptr;

		/* use legacy model (see above) -- DEPRECATED! */
		if (use_legacy_cycle_model) {
			switch (opId) {
				case Operation::OpId::LB:
				case Operation::OpId::LBU:
				case Operation::OpId::LH:
				case Operation::OpId::LHU:
				case Operation::OpId::LW:
				case Operation::OpId::SB:
				case Operation::OpId::SH:
				case Operation::OpId::SW:
					instr_clock_cycles = 4;
					break;

				case Operation::OpId::MUL:
				case Operation::OpId::MULH:
				case Operation::OpId::MULHU:
				case Operation::OpId::MULHSU:
				case Operation::OpId::DIV:
				case Operation::OpId::DIVU:
				case Operation::OpId::REM:
				case Operation::OpId::REMU:
					instr_clock_cycles = 8;
					break;

				default:
					instr_clock_cycles = 1;
			}
		}

		/* try to load cycle model from the global property map (or use default) */
		std::string desc = std::string(Operation::opIdStr[opId]);
		std::replace(desc.begin(), desc.end(), '.', '_');
		std::replace(desc.begin(), desc.end(), ' ', '_');
		VPPP_PROPERTY_GET("ISS." + name(), desc + "_instr_clock_cycles", uint64_t, instr_clock_cycles);

		/* set instruction time */
		opMap[opId].instr_time = instr_clock_cycles * prop_clock_cycle_period.value(); /* ps */
	}
}

void ISS_CT::print_trace() {
	uint32_t mem_word = dbbcache.get_mem_word();

	/*
	 * NOTE:
	 *  * we decode here again, but efficiency does not matter much for this kind of tracing.
	 *    -> TODO: optimize (e.g. table from jump label to op)
	 *  * every compressed instruction was already converted to a normal instruction.
	 *    -> it is safe to use decode_normal here
	 */
	Operation::OpId opId = instr.decode_normal(ARCH, *isa_config);

	printf("core %2u: prv %1x: pc %8x (%8x): %s ", csrs.mhartid.reg.val, prv, dbbcache.get_last_pc_before_callback(),
	       mem_word, Operation::opIdStr.at(opId));
	switch (Operation::getType(opId)) {
		case Operation::Type::R:
			printf(COLORFRMT ", " COLORFRMT ", " COLORFRMT,
			       COLORPRINT(regcolors[instr.rd()], RegFile::regnames[instr.rd()]),
			       COLORPRINT(regcolors[instr.rs1()], RegFile::regnames[instr.rs1()]),
			       COLORPRINT(regcolors[instr.rs2()], RegFile::regnames[instr.rs2()]));
			break;
		case Operation::Type::R4:
			printf(COLORFRMT ", " COLORFRMT ", " COLORFRMT ", " COLORFRMT,
			       COLORPRINT(regcolors[instr.rd()], RegFile::regnames[instr.rd()]),
			       COLORPRINT(regcolors[instr.rs1()], RegFile::regnames[instr.rs1()]),
			       COLORPRINT(regcolors[instr.rs2()], RegFile::regnames[instr.rs2()]),
			       COLORPRINT(regcolors[instr.rs3()], RegFile::regnames[instr.rs3()]));
			break;
		case Operation::Type::I:
			printf(COLORFRMT ", " COLORFRMT ", 0x%x", COLORPRINT(regcolors[instr.rd()], RegFile::regnames[instr.rd()]),
			       COLORPRINT(regcolors[instr.rs1()], RegFile::regnames[instr.rs1()]), instr.I_imm());
			break;
		case Operation::Type::S:
			printf(COLORFRMT ", " COLORFRMT ", 0x%x",
			       COLORPRINT(regcolors[instr.rs1()], RegFile::regnames[instr.rs1()]),
			       COLORPRINT(regcolors[instr.rs2()], RegFile::regnames[instr.rs2()]), instr.S_imm());
			break;
		case Operation::Type::B:
			printf(COLORFRMT ", " COLORFRMT ", 0x%x",
			       COLORPRINT(regcolors[instr.rs1()], RegFile::regnames[instr.rs1()]),
			       COLORPRINT(regcolors[instr.rs2()], RegFile::regnames[instr.rs2()]), instr.B_imm());
			break;
		case Operation::Type::U:
			printf(COLORFRMT ", 0x%x", COLORPRINT(regcolors[instr.rd()], RegFile::regnames[instr.rd()]), instr.U_imm());
			break;
		case Operation::Type::J:
			printf(COLORFRMT ", 0x%x", COLORPRINT(regcolors[instr.rd()], RegFile::regnames[instr.rd()]), instr.J_imm());
			break;
		default:;
	}
	puts("");
}
}  // namespace rv32

/*
 * label generation
 * Fetch, Decode and Dispatch (FDD) variants
 */

/* helpers for ctemplate label handling */
/* prefix for all static variables, sections and labels: "<ARCH>_<TEMPLATE_INSTANCE_NAME>_" (ensure uniqueness) */
#define OP_PREFIX M_JOIN(ISS_CT_ARCH, M_JOIN(_, M_JOIN(ISS_CT, _)))
/* op code entry section handling */
#define OP_LABEL_ENTRIES_SECNAME M_JOIN(OP_PREFIX, op_label_entries)
#define OP_LABLE_ENTRIES_SEC_STR M_DEFINE2STR(OP_LABEL_ENTRIES_SECNAME)
#define OP_LABEL_ENTIRES_SEC_START M_JOIN(__start_, OP_LABEL_ENTRIES_SECNAME)
#define OP_LABEL_ENTIRES_SEC_STOP M_JOIN(__stop_, OP_LABEL_ENTRIES_SECNAME)
/* fast_abort_and_fdd_labelPtr handling */
#define OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_NAME M_JOIN(OP_PREFIX, op_global_fast_abort_and_fdd_labelPtr)
#define OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_STR M_DEFINE2STR(OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_NAME)
#define OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_START M_JOIN(__start_, OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_NAME)
/* explicitly defined labels */
#define OP_LABEL(_name) M_JOIN(OP_PREFIX, M_JOIN(op_label_, _name))
#define OP_LABEL_ENTRY(_name) M_JOIN(OP_PREFIX, M_JOIN(op_label_, _name))
/* for labels generated in OP_CASE */
#define OP_LABEL_OP(_op) M_JOIN(OP_LABEL(op_), _op)
#define OP_LABEL_ENTRY_OP(_op) M_JOIN(OP_LABEL(entry_), _op)

extern void *const OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_START;
extern const struct op_label_entry OP_LABEL_ENTIRES_SEC_START;
extern const struct op_label_entry OP_LABEL_ENTIRES_SEC_STOP;

namespace rv32 {
void *ISS_CT::genOpMap() {
	bool error = false;
	struct op_label_entry *entry = (struct op_label_entry *)&OP_LABEL_ENTIRES_SEC_START;
	struct op_label_entry *end = (struct op_label_entry *)&OP_LABEL_ENTIRES_SEC_STOP;

	// fill op labels (all others are already initialized with nullptr)
	while (entry < end) {
		if ((unsigned int)entry->opId >= Operation::OpId::NUMBER_OF_OPERATIONS) {
			std::cerr << "[ISS] Error: Invalid operation (" << entry->opId << ") in op_lable_entry section at 0x"
			          << std::hex << entry << std::dec << std::endl;
			error = true;
			break;
		}
		if (opMap[entry->opId].labelPtr != nullptr) {
			std::cerr << "[ISS] Error: Multiple implementations for operation " << entry->opId << " ("
			          << Operation::opIdStr.at(entry->opId) << ")" << std::endl;
			error = true;
		}
		opMap[entry->opId].labelPtr = entry->labelPtr;
		entry++;
	}
	if (error) {
		throw std::runtime_error("[ISS] Multiple implementations for operation(s) (see above)");
	}

	// check for unimplemented opcodes
	for (unsigned int opId = 0; opId < Operation::OpId::NUMBER_OF_OPERATIONS; opId++) {
		if (opMap[opId].labelPtr == nullptr) {
			std::cerr << "[ISS] Error: Unimplemented operation " << opId << " (" << Operation::opIdStr.at(opId) << ")"
			          << std::endl;
			error = true;
		}
	}
	if (error) {
		throw std::runtime_error("[ISS] Unimplemented operation(s) (see above)");
	}

	return OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_START;
}

#define OP_SLOW_FDD()                                                       \
	assert(((pc & ~pc_alignment_mask()) == 0) && "misaligned instruction"); \
	stats.inc_cnt();                                                        \
	stats.inc_slow_fdd();                                                   \
	void *opLabelPtr = dbbcache.fetch_decode(pc, instr);                    \
	if (trace) {                                                            \
		print_trace();                                                      \
		/* always stay in slow path if trace enabled */                     \
		force_slow_path();                                                  \
	}                                                                       \
	goto *opLabelPtr;

#define OP_MED_FDD()     \
	stats.inc_cnt();     \
	stats.inc_med_fdd(); \
	goto *dbbcache.fetch_decode(pc, instr);

#define OP_FAST_FDD()     \
	stats.inc_cnt();      \
	stats.inc_fast_fdd(); \
	goto *dbbcache.fetch_decode_fast(instr);

/* fast operation finalization and fdd (TODO: move ninstr check to control flow ops?) */
#define OP_FAST_FINALIZE_AND_FDD() \
	ninstr++;                      \
	OP_FAST_FDD();

#define OP_GLOBAL_FDD() OP_LABEL(op_global_fdd) :

/* switch / case structure emulation */

#define OP_SWITCH_BEGIN()
#define OP_SWITCH_END()                                                            \
	OP_LABEL(op_global_fast_abort_and_fdd)                                         \
	    : static void * OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_NAME                    \
	      __attribute__((used, section(OP_GLOBAL_FAST_ABORT_AND_FDD_LABEL_STR))) = \
	    &&OP_LABEL(op_global_fast_abort_and_fdd);                                  \
	dbbcache.abort_fetch_decode_fast();                                            \
	stats.dec_cnt();                                                               \
	stats.inc_fast_fdd_abort();                                                    \
	goto OP_LABEL(op_global_fdd);                                                  \
	OP_LABEL(op_global_fast_finalize_and_fdd) : __attribute__((unused));           \
	{OP_FAST_FINALIZE_AND_FDD()}

#define OP_CASE(_op)                                                                                                 \
	OP_LABEL_OP(_op)                                                                                                 \
	    : static struct op_label_entry OP_LABEL_ENTRY_OP(_op)                                                        \
	          __attribute__((used, section(OP_LABLE_ENTRIES_SEC_STR))) = {Operation::OpId::_op, &&OP_LABEL_OP(_op)}; \
	stats.inc_op(Operation::OpId::_op);

#define OP_INVALID_END()                                                                                             \
	if (trace) {                                                                                                     \
		std::cout << "[ISS] WARNING: RV64 instruction not supported on RV32 " << std::to_string(instr.data())        \
		          << "' at address '" << std::to_string(dbbcache.get_last_pc_before_callback()) << "'" << std::endl; \
	}                                                                                                                \
	RAISE_ILLEGAL_INSTRUCTION();

#undef OP_CASE_NOP
#undef OP_CASE_INVALID
#ifdef ISS_CT_STATS_ENABLED
/* ensure correct counting of opIds (see OP_CASE) -> treat each nop as dedicated case with its own OP_END (no
 * fallthough, but higher overhead (cache)) */
#define OP_CASE_NOP(_op) \
	OP_CASE(_op)         \
	stats.inc_nops();    \
	OP_END();
#define OP_CASE_INVALID(_op) \
	OP_CASE(_op)             \
	OP_INVALID_END();

#else /* ISS_CT_STATS_ENABLED */
/* disabled stats -> fall through to common OP_END (less overhead -> more efficient (cache)) */
#define OP_CASE_NOP(_op) OP_CASE(_op)
#define OP_CASE_INVALID(_op) OP_CASE(_op)
#endif /* ISS_CT_STATS_ENABLED */

#ifdef ISS_CT_OP_TAIL_FAST_FDD_ENABLED
#define OP_END() OP_FAST_FINALIZE_AND_FDD()
#else
#define OP_END() goto OP_LABEL(op_global_fast_finalize_and_fdd)
#endif


/*
 * supress "ISO C++ forbids computed gotos [-Wpedantic]" warings by disabling -Wpedantic for exec_steps
 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
void ISS_CT::exec_steps(const bool debug_single_step) {
	/* keep track if step was done */
	bool debug_single_step_done = false;

	// TODO: remove?
	assert(regs.read(0) == 0);

	/*
	 * Check quantum in fast path roughly every tenth of a quantum (heuristic)
	 * Uncertainties: time is also increasing outside of ISS; not every instruction has cycle_time
	 */
	const uint64_t fast_quantum_ins_granularity =
	    quantum_keeper.get_global_quantum().value() / prop_clock_cycle_period.value() / 10;
	uint64_t ninstr = 0;

	// TODO: remove?
	assert(((pc & ~pc_alignment_mask()) == 0) && "misaligned instruction");

	/* start in slow_path */
	force_slow_path();

	do {
		try {
			/* global (slow and med) operation fetch, decode and dispatch (FDD) */
			OP_GLOBAL_FDD() {
				pc = dbbcache.get_pc_maybe_after_callback();

				if (unlikely(iss_slow_path)) {
					iss_slow_path = false;

					/* update counters by local fast counters */
					commit_instructions(ninstr);
					commit_cycles();

					/* call interrupt handling */
					handle_interrupt();

					// TODO: CHECK!
					/* Do not use a check *pc == last_pc* here. The reason is that due to */
					/* interrupts *pc* can be set to *last_pc* accidentally (when jumping back */
					/* to *mepc*). */
					if (shall_exit) {
						set_status(CoreExecStatus::Terminated);
					}
					/* run a single step until either a breakpoint is hit or the execution terminates */
					if (status != CoreExecStatus::Runnable) {
						break;
					}

					if (lr_sc_counter != 0) {
						stats.inc_lr_sc();
						--lr_sc_counter;
						assert(lr_sc_counter >= 0);
						if (lr_sc_counter == 0) {
							release_lr_sc_reservation();
						} else {
							// again
							force_slow_path();
						}
					} else {
						// match SystemC sync with bus unlocking in a tight LR_W/SC_W loop
						stats.inc_qk_need_sync();
						if (quantum_keeper.need_sync()) {
							stats.inc_qk_sync();
							quantum_keeper.sync();
						}
					}

					/* speeds up the execution performance (non debug mode) significantly by */
					/* checking the additional flag first */
					if (debug_mode) {
						/* always stay in slow path if debug enabled */
						force_slow_path();

						/* stop after single step */
						if (debug_single_step && debug_single_step_done) {
							break;
						}

						/* stop on breakpoint */
						if (breakpoints.find(pc) != breakpoints.end()) {
							set_status(CoreExecStatus::HitBreakpoint);
							break;
						}
						debug_single_step_done = true;
					}

					OP_SLOW_FDD();
					// UNREACHABLE
					assert(false);
				}

				if (unlikely(ninstr > fast_quantum_ins_granularity)) {
					///* perform slow path next time -> update instr/cyclc counters and check if quantum needs sync */
					// iss_slow_path = true;
					commit_instructions(ninstr);
					commit_cycles();
					stats.inc_qk_need_sync();
					if (quantum_keeper.need_sync()) {
						// TODO: must also be done for transactions (keeper in common/mem.h) ?!
						/* sync pc member variable before potential SysC context switch */
						// pc = dbbcache.get_pc_maybe_after_callback();
						stats.inc_qk_sync();
						quantum_keeper.sync();
					}
				}

				OP_MED_FDD();
				// UNREACHABLE
				assert(false);
			}

			/* operation implementations and implicit fast operation fetch, decode and dispatch (FFD) */
			OP_SWITCH_BEGIN() {
				OP_CASE(UNDEF) {
					if (trace)
						std::cout << "[ISS] WARNING: unknown instruction '" << std::to_string(instr.data())
						          << "' at address '" << std::to_string(dbbcache.get_last_pc_before_callback()) << "'"
						          << std::endl;
					RAISE_ILLEGAL_INSTRUCTION();
				}
				OP_END();

				OP_CASE(UNSUP) {
					if (trace)
						std::cout << "[ISS] WARNING: instruction not supported (e.g. extension disabled in misa csr) '"
						          << std::to_string(instr.data()) << "' at address '"
						          << std::to_string(dbbcache.get_last_pc_before_callback()) << "'" << std::endl;
					RAISE_ILLEGAL_INSTRUCTION();
				}
				OP_END();

				/*
				 * RV64 instructions not supported on RV32
				 */
				// RV64I
				OP_CASE_INVALID(SD)
				OP_CASE_INVALID(LD)
				OP_CASE_INVALID(LWU)
				OP_CASE_INVALID(ADDIW)
				OP_CASE_INVALID(ADDIW_NOP)
				OP_CASE_INVALID(SLLIW)
				OP_CASE_INVALID(SLLIW_NOP)
				OP_CASE_INVALID(SRLIW)
				OP_CASE_INVALID(SRLIW_NOP)
				OP_CASE_INVALID(SRAIW)
				OP_CASE_INVALID(SRAIW_NOP)
				OP_CASE_INVALID(ADDW)
				OP_CASE_INVALID(ADDW_NOP)
				OP_CASE_INVALID(SUBW)
				OP_CASE_INVALID(SUBW_NOP)
				OP_CASE_INVALID(SLLW)
				OP_CASE_INVALID(SLLW_NOP)
				OP_CASE_INVALID(SRLW)
				OP_CASE_INVALID(SRLW_NOP)
				OP_CASE_INVALID(SRAW)
				OP_CASE_INVALID(SRAW_NOP)
				// RV64M
				OP_CASE_INVALID(MULW)
				OP_CASE_INVALID(MULW_NOP)
				OP_CASE_INVALID(DIVW)
				OP_CASE_INVALID(DIVW_NOP)
				OP_CASE_INVALID(DIVUW)
				OP_CASE_INVALID(DIVUW_NOP)
				OP_CASE_INVALID(REMW)
				OP_CASE_INVALID(REMW_NOP)
				OP_CASE_INVALID(REMUW)
				OP_CASE_INVALID(REMUW_NOP)
				// RV64A
				OP_CASE_INVALID(LR_D)
				OP_CASE_INVALID(SC_D)
				OP_CASE_INVALID(AMOSWAP_D)
				OP_CASE_INVALID(AMOADD_D)
				OP_CASE_INVALID(AMOXOR_D)
				OP_CASE_INVALID(AMOAND_D)
				OP_CASE_INVALID(AMOOR_D)
				OP_CASE_INVALID(AMOMIN_D)
				OP_CASE_INVALID(AMOMINU_D)
				OP_CASE_INVALID(AMOMAX_D)
				OP_CASE_INVALID(AMOMAXU_D)
				// RV64Zfh
				OP_CASE_INVALID(FCVT_L_H)
				OP_CASE_INVALID(FCVT_LU_H)
				OP_CASE_INVALID(FCVT_H_L)
				OP_CASE_INVALID(FCVT_H_LU)
				// RV64F
				OP_CASE_INVALID(FCVT_L_S)
				OP_CASE_INVALID(FCVT_LU_S)
				OP_CASE_INVALID(FCVT_S_L)
				OP_CASE_INVALID(FCVT_S_LU)
				// RV64D
				OP_CASE_INVALID(FCVT_L_D)
				OP_CASE_INVALID(FCVT_LU_D)
				OP_CASE_INVALID(FMV_X_D)
				OP_CASE_INVALID(FCVT_D_L)
				OP_CASE_INVALID(FCVT_D_LU)
				OP_CASE_INVALID(FMV_D_X)
				OP_INVALID_END(); /* needed for fallthrough (see definition of OP_CASE_INVALID above) */

				/*
				 * NOP instruction variants
				 * instructions decoded with rd == zero/x0 and no side effects -> nothing to do
				 */
				OP_CASE_NOP(LUI_NOP)
				OP_CASE_NOP(AUIPC_NOP)
				OP_CASE_NOP(ADDI_NOP)
				OP_CASE_NOP(SLTI_NOP)
				OP_CASE_NOP(SLTIU_NOP)
				OP_CASE_NOP(XORI_NOP)
				OP_CASE_NOP(ORI_NOP)
				OP_CASE_NOP(ANDI_NOP)
				OP_CASE_NOP(SLLI_NOP)
				OP_CASE_NOP(SRLI_NOP)
				OP_CASE_NOP(SRAI_NOP)
				OP_CASE_NOP(ADD_NOP)
				OP_CASE_NOP(SUB_NOP)
				OP_CASE_NOP(SLL_NOP)
				OP_CASE_NOP(SLT_NOP)
				OP_CASE_NOP(SLTU_NOP)
				OP_CASE_NOP(XOR_NOP)
				OP_CASE_NOP(SRL_NOP)
				OP_CASE_NOP(SRA_NOP)
				OP_CASE_NOP(OR_NOP)
				OP_CASE_NOP(AND_NOP)
				OP_CASE_NOP(MUL_NOP)
				OP_CASE_NOP(MULH_NOP)
				OP_CASE_NOP(MULHSU_NOP)
				OP_CASE_NOP(MULHU_NOP)
				OP_CASE_NOP(DIV_NOP)
				OP_CASE_NOP(DIVU_NOP)
				OP_CASE_NOP(REM_NOP)
				OP_CASE_NOP(REMU_NOP)
				OP_END(); /* needed for fallthrough (see definition of OP_CASE_NOP above) */

				OP_CASE(ADDI) {
					regs[instr.rd()] = regs[instr.rs1()] + instr.I_imm();
				}
				OP_END();

				OP_CASE(SLTI) {
					regs[instr.rd()] = regs[instr.rs1()] < instr.I_imm();
				}
				OP_END();

				OP_CASE(SLTIU) {
					regs[instr.rd()] = ((uxlen_t)regs[instr.rs1()]) < ((uxlen_t)instr.I_imm());
				}
				OP_END();

				OP_CASE(XORI) {
					regs[instr.rd()] = regs[instr.rs1()] ^ instr.I_imm();
				}
				OP_END();

				OP_CASE(ORI) {
					regs[instr.rd()] = regs[instr.rs1()] | instr.I_imm();
				}
				OP_END();

				OP_CASE(ANDI) {
					regs[instr.rd()] = regs[instr.rs1()] & instr.I_imm();
				}
				OP_END();

				OP_CASE(ADD) {
					regs[instr.rd()] = regs[instr.rs1()] + regs[instr.rs2()];
				}
				OP_END();

				OP_CASE(SUB) {
					regs[instr.rd()] = regs[instr.rs1()] - regs[instr.rs2()];
				}
				OP_END();

				OP_CASE(SLL) {
					regs[instr.rd()] = regs[instr.rs1()] << regs.shamt(instr.rs2());
				}
				OP_END();

				OP_CASE(SLT) {
					regs[instr.rd()] = regs[instr.rs1()] < regs[instr.rs2()];
				}
				OP_END();

				OP_CASE(SLTU) {
					regs[instr.rd()] = ((uxlen_t)regs[instr.rs1()]) < ((uxlen_t)regs[instr.rs2()]);
				}
				OP_END();

				OP_CASE(SRL) {
					regs[instr.rd()] = ((uxlen_t)regs[instr.rs1()]) >> regs.shamt(instr.rs2());
				}
				OP_END();

				OP_CASE(SRA) {
					regs[instr.rd()] = regs[instr.rs1()] >> regs.shamt(instr.rs2());
				}
				OP_END();

				OP_CASE(XOR) {
					regs[instr.rd()] = regs[instr.rs1()] ^ regs[instr.rs2()];
				}
				OP_END();

				OP_CASE(OR) {
					regs[instr.rd()] = regs[instr.rs1()] | regs[instr.rs2()];
				}
				OP_END();

				OP_CASE(AND) {
					regs[instr.rd()] = regs[instr.rs1()] & regs[instr.rs2()];
				}
				OP_END();

				OP_CASE(SLLI) {
					regs[instr.rd()] = regs[instr.rs1()] << instr.shamt();
				}
				OP_END();

				OP_CASE(SRLI) {
					regs[instr.rd()] = ((uxlen_t)regs[instr.rs1()]) >> instr.shamt();
				}
				OP_END();

				OP_CASE(SRAI) {
					regs[instr.rd()] = regs[instr.rs1()] >> instr.shamt();
				}
				OP_END();

				OP_CASE(LUI) {
					regs[instr.rd()] = instr.U_imm();
				}
				OP_END();

				OP_CASE(AUIPC) {
					regs[instr.rd()] = dbbcache.get_last_pc_before_callback() + instr.U_imm();
				}
				OP_END();

				OP_CASE(J) {
					stats.inc_j();
					dbbcache.jump(instr.J_imm());
					if (unlikely(ninstr > fast_quantum_ins_granularity)) {
						ninstr++;
						goto OP_LABEL(op_global_fdd);
					}
				}
				OP_END();

				OP_CASE(JAL) {
					stats.inc_jal();
					regs[instr.rd()] = dbbcache.jump_and_link(instr.J_imm());
					if (unlikely(ninstr > fast_quantum_ins_granularity)) {
						ninstr++;
						goto OP_LABEL(op_global_fdd);
					}
				}
				OP_END();

				OP_CASE(JR) {
					stats.inc_jr();
					uxlen_t pc = (regs[instr.rs1()] + instr.I_imm()) & ~1;

					if (unlikely((pc & 0x3) && (!csrs.misa.has_C_extension()))) {
						// NOTE: misaligned instruction address not possible on machines supporting compressed
						// instructions
						raise_trap(EXC_INSTR_ADDR_MISALIGNED, pc);
					}

					dbbcache.jump_dyn(pc);
					if (unlikely(ninstr > fast_quantum_ins_granularity)) {
						ninstr++;
						goto OP_LABEL(op_global_fdd);
					}
				}
				OP_END();

				OP_CASE(JALR) {
					stats.inc_jalr();
					uxlen_t pc = (regs[instr.rs1()] + instr.I_imm()) & ~1;

					if (unlikely((pc & 0x3) && (!csrs.misa.has_C_extension()))) {
						// NOTE: misaligned instruction address not possible on machines supporting compressed
						// instructions
						raise_trap(EXC_INSTR_ADDR_MISALIGNED, pc);
					}

					regs[instr.rd()] = dbbcache.jump_dyn_and_link(pc);
					if (unlikely(ninstr > fast_quantum_ins_granularity)) {
						ninstr++;
						goto OP_LABEL(op_global_fdd);
					}
				}
				OP_END();

				OP_CASE(SB) {
					uxlen_t addr = regs[instr.rs1()] + instr.S_imm();
					lscache.store_byte(addr, regs[instr.rs2()]);
				}
				OP_END();

				OP_CASE(SH) {
					uxlen_t addr = regs[instr.rs1()] + instr.S_imm();
					trap_check_addr_alignment<2, false>(addr);
					lscache.store_half(addr, regs[instr.rs2()]);
				}
				OP_END();

				OP_CASE(SW) {
					uxlen_t addr = regs[instr.rs1()] + instr.S_imm();
					trap_check_addr_alignment<4, false>(addr);
					lscache.store_word(addr, regs[instr.rs2()]);
				}
				OP_END();

				OP_CASE(LB) {
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					regs[instr.rd()] = lscache.load_byte(addr);
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(LH) {
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					trap_check_addr_alignment<2, true>(addr);
					regs[instr.rd()] = lscache.load_half(addr);
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(LW) {
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					trap_check_addr_alignment<4, true>(addr);
					regs[instr.rd()] = lscache.load_word(addr);
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(LBU) {
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					regs[instr.rd()] = lscache.load_ubyte(addr);
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(LHU) {
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					trap_check_addr_alignment<2, true>(addr);
					regs[instr.rd()] = lscache.load_uhalf(addr);
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(BEQ) {
					if (regs[instr.rs1()] == regs[instr.rs2()]) {
						dbbcache.branch_taken(instr.B_imm());
						if (unlikely(ninstr > fast_quantum_ins_granularity)) {
							ninstr++;
							goto OP_LABEL(op_global_fdd);
						}
					} else {
						dbbcache.branch_not_taken(pc);
					}
				}
				OP_END();

				OP_CASE(BNE) {
					if (regs[instr.rs1()] != regs[instr.rs2()]) {
						dbbcache.branch_taken(instr.B_imm());
						if (unlikely(ninstr > fast_quantum_ins_granularity)) {
							ninstr++;
							goto OP_LABEL(op_global_fdd);
						}
					} else {
						dbbcache.branch_not_taken(pc);
					}
				}
				OP_END();

				OP_CASE(BLT) {
					if (regs[instr.rs1()] < regs[instr.rs2()]) {
						dbbcache.branch_taken(instr.B_imm());
						if (unlikely(ninstr > fast_quantum_ins_granularity)) {
							ninstr++;
							goto OP_LABEL(op_global_fdd);
						}
					} else {
						dbbcache.branch_not_taken(pc);
					}
				}
				OP_END();

				OP_CASE(BGE) {
					if (regs[instr.rs1()] >= regs[instr.rs2()]) {
						dbbcache.branch_taken(instr.B_imm());
						if (unlikely(ninstr > fast_quantum_ins_granularity)) {
							ninstr++;
							goto OP_LABEL(op_global_fdd);
						}
					} else {
						dbbcache.branch_not_taken(pc);
					}
				}
				OP_END();

				OP_CASE(BLTU) {
					if ((uxlen_t)regs[instr.rs1()] < (uxlen_t)regs[instr.rs2()]) {
						dbbcache.branch_taken(instr.B_imm());
						if (unlikely(ninstr > fast_quantum_ins_granularity)) {
							ninstr++;
							goto OP_LABEL(op_global_fdd);
						}
					} else {
						dbbcache.branch_not_taken(pc);
					}
				}
				OP_END();

				OP_CASE(BGEU) {
					if ((uxlen_t)regs[instr.rs1()] >= (uxlen_t)regs[instr.rs2()]) {
						dbbcache.branch_taken(instr.B_imm());
						if (unlikely(ninstr > fast_quantum_ins_granularity)) {
							ninstr++;
							goto OP_LABEL(op_global_fdd);
						}
					} else {
						dbbcache.branch_not_taken(pc);
					}
				}
				OP_END();

				/* rd != x0/zero variants */
				OP_CASE(FENCE) {
					lscache.fence();
				}
				OP_END();

				OP_CASE(FENCE_I) {
					dbbcache.fence_i(pc);
					stats.inc_fence_i();
				}
				OP_END();

				OP_CASE(ECALL) {
					if (sys) {
						sys->execute_syscall(this);
					} else {
						uxlen_t last_pc = dbbcache.get_last_pc_before_callback();
						switch (prv) {
							case MachineMode:
								raise_trap(EXC_ECALL_M_MODE, last_pc);
								break;
							case SupervisorMode:
								if (virt) {
									raise_trap(EXC_ECALL_VS_MODE, last_pc);  // H Extension fdw: VS ECALL must report virtual-supervisor-ecall
								} else {
									raise_trap(EXC_ECALL_S_MODE, last_pc);
								}
								break;
							case UserMode:
								raise_trap(EXC_ECALL_U_MODE, last_pc);
								break;
							default:
								throw std::runtime_error("unknown privilege level " + std::to_string(prv));
						}
					}
				}
				OP_END();

				OP_CASE(EBREAK) {
					if (debug_mode) {
						set_status(CoreExecStatus::HitBreakpoint);
					} else {
						// TODO: also raise trap if we are in debug mode?
						raise_trap(EXC_BREAKPOINT, dbbcache.get_last_pc_before_callback());
					}
				}
				OP_END();

				OP_CASE(CSRRW) {
					stats.inc_csr();
					auto addr = instr.csr();
					if (is_invalid_csr_access(addr, true)) {
						RAISE_ILLEGAL_INSTRUCTION();
					} else {
						auto rd = instr.rd();
						auto rs1_val = regs[instr.rs1()];
						if (rd != RegFile::zero) {
							commit_instructions(ninstr);
							regs[instr.rd()] = get_csr_value(addr);
						}
						set_csr_value(addr, rs1_val);
					}
				}
				OP_END();

				OP_CASE(CSRRS) {
					stats.inc_csr();
					auto addr = instr.csr();
					auto rs1 = instr.rs1();
					auto write = rs1 != RegFile::zero;
					if (is_invalid_csr_access(addr, write)) {
						RAISE_ILLEGAL_INSTRUCTION();
					} else {
						auto rd = instr.rd();
						auto rs1_val = regs[rs1];
						commit_instructions(ninstr);
						auto csr_val = get_csr_value(addr);
						if (rd != RegFile::zero)
							regs[rd] = csr_val;
						if (write)
							set_csr_value(addr, csr_val | rs1_val);
					}
				}
				OP_END();

				OP_CASE(CSRRC) {
					stats.inc_csr();
					auto addr = instr.csr();
					auto rs1 = instr.rs1();
					auto write = rs1 != RegFile::zero;
					if (is_invalid_csr_access(addr, write)) {
						RAISE_ILLEGAL_INSTRUCTION();
					} else {
						auto rd = instr.rd();
						auto rs1_val = regs[rs1];
						commit_instructions(ninstr);
						auto csr_val = get_csr_value(addr);
						if (rd != RegFile::zero)
							regs[rd] = csr_val;
						if (write)
							set_csr_value(addr, csr_val & ~rs1_val);
					}
				}
				OP_END();

				OP_CASE(CSRRWI) {
					stats.inc_csr();
					auto addr = instr.csr();
					if (is_invalid_csr_access(addr, true)) {
						RAISE_ILLEGAL_INSTRUCTION();
					} else {
						auto rd = instr.rd();
						if (rd != RegFile::zero) {
							commit_instructions(ninstr);
							regs[rd] = get_csr_value(addr);
						}
						set_csr_value(addr, instr.zimm());
					}
				}
				OP_END();

				OP_CASE(CSRRSI) {
					stats.inc_csr();
					auto addr = instr.csr();
					auto zimm = instr.zimm();
					auto write = zimm != 0;
					if (is_invalid_csr_access(addr, write)) {
						RAISE_ILLEGAL_INSTRUCTION();
					} else {
						commit_instructions(ninstr);
						auto csr_val = get_csr_value(addr);
						auto rd = instr.rd();
						if (rd != RegFile::zero)
							regs[rd] = csr_val;
						if (write)
							set_csr_value(addr, csr_val | zimm);
					}
				}
				OP_END();

				OP_CASE(CSRRCI) {
					stats.inc_csr();
					auto addr = instr.csr();
					auto zimm = instr.zimm();
					auto write = zimm != 0;
					if (is_invalid_csr_access(addr, write)) {
						RAISE_ILLEGAL_INSTRUCTION();
					} else {
						commit_instructions(ninstr);
						auto csr_val = get_csr_value(addr);
						auto rd = instr.rd();
						if (rd != RegFile::zero)
							regs[rd] = csr_val;
						if (write)
							set_csr_value(addr, csr_val & ~zimm);
					}
				}
				OP_END();

				/* rd != x0/zero variants */
				OP_CASE(MUL) {
					int64_t ans = (int64_t)regs[instr.rs1()] * (int64_t)regs[instr.rs2()];
					regs[instr.rd()] = ans & 0xFFFFFFFF;
				}
				OP_END();

				OP_CASE(MULH) {
					int64_t ans = (int64_t)regs[instr.rs1()] * (int64_t)regs[instr.rs2()];
					regs[instr.rd()] = (ans & 0xFFFFFFFF00000000) >> 32;
				}
				OP_END();

				OP_CASE(MULHU) {
					int64_t ans = ((uint64_t)(uxlen_t)regs[instr.rs1()]) * (uint64_t)((uxlen_t)regs[instr.rs2()]);
					regs[instr.rd()] = (ans & 0xFFFFFFFF00000000) >> 32;
				}
				OP_END();

				OP_CASE(MULHSU) {
					int64_t ans = (int64_t)regs[instr.rs1()] * (uint64_t)((uxlen_t)regs[instr.rs2()]);
					regs[instr.rd()] = (ans & 0xFFFFFFFF00000000) >> 32;
				}
				OP_END();

				OP_CASE(DIV) {
					auto a = regs[instr.rs1()];
					auto b = regs[instr.rs2()];
					if (b == 0) {
						regs[instr.rd()] = -1;
					} else if (a == REG_MIN && b == -1) {
						regs[instr.rd()] = a;
					} else {
						regs[instr.rd()] = a / b;
					}
				}
				OP_END();

				OP_CASE(DIVU) {
					auto a = regs[instr.rs1()];
					auto b = regs[instr.rs2()];
					if (b == 0) {
						regs[instr.rd()] = -1;
					} else {
						regs[instr.rd()] = (uxlen_t)a / (uxlen_t)b;
					}
				}
				OP_END();

				OP_CASE(REM) {
					auto a = regs[instr.rs1()];
					auto b = regs[instr.rs2()];
					if (b == 0) {
						regs[instr.rd()] = a;
					} else if (a == REG_MIN && b == -1) {
						regs[instr.rd()] = 0;
					} else {
						regs[instr.rd()] = a % b;
					}
				}
				OP_END();

				OP_CASE(REMU) {
					auto a = regs[instr.rs1()];
					auto b = regs[instr.rs2()];
					if (b == 0) {
						regs[instr.rd()] = a;
					} else {
						regs[instr.rd()] = (uxlen_t)a % (uxlen_t)b;
					}
				}
				OP_END();

				/*
				 * A Extension
				 * Note: handling of x0/zero
				 *  * for AMO instructoins is done in execute_amo* implementations -> writes to zero/x0 are ignored
				 *  * for lr/sc instructions is done within case -> zero/x0 is reset to zero after op
				 */
				OP_CASE(LR_W) {
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<4, true>(addr);
					mem->set_next_lr_sc(instr.aq(), instr.rl());
					regs[instr.rd()] = mem->atomic_load_reserved_word(addr);
					if (lr_sc_counter == 0) {
						lr_sc_counter = 17;  // this instruction + 16 additional ones, (an over-approximation) to cover
						                     // the RISC-V forward progress property
						force_slow_path();
					}
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(SC_W) {
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<4, false>(addr);
					uint32_t val = regs[instr.rs2()];
					mem->set_next_lr_sc(instr.aq(), instr.rl());
					const auto sc_ok = mem->atomic_store_conditional_word(addr, val);
					// H Extension fdw: defer SC writeback until after the store path completes so traps do not
					// transiently corrupt x0/rd state before trap-entry CSR code runs.
					if (instr.rd() != RegFile::zero)
						regs[instr.rd()] = sc_ok ? 0 : 1;
					lr_sc_counter = 0;
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(AMOSWAP_W) {
					execute_amo_w(instr, PmaAmoClass::AMOSwap, TlmAmoOp::Swap, [](int32_t a, int32_t b) {
						(void)a;
						return b;
					});
				}
				OP_END();

				OP_CASE(AMOADD_W) {
					execute_amo_w(instr, PmaAmoClass::AMOArithmetic, TlmAmoOp::Add, [](int32_t a, int32_t b) { return a + b; });
				}
				OP_END();

				OP_CASE(AMOXOR_W) {
					execute_amo_w(instr, PmaAmoClass::AMOLogical, TlmAmoOp::Xor, [](int32_t a, int32_t b) { return a ^ b; });
				}
				OP_END();

				OP_CASE(AMOAND_W) {
					execute_amo_w(instr, PmaAmoClass::AMOLogical, TlmAmoOp::And, [](int32_t a, int32_t b) { return a & b; });
				}
				OP_END();

				OP_CASE(AMOOR_W) {
					execute_amo_w(instr, PmaAmoClass::AMOLogical, TlmAmoOp::Or, [](int32_t a, int32_t b) { return a | b; });
				}
				OP_END();

				OP_CASE(AMOMIN_W) {
					execute_amo_w(instr, PmaAmoClass::AMOArithmetic, TlmAmoOp::MinSigned, [](int32_t a, int32_t b) { return std::min(a, b); });
				}
				OP_END();

				OP_CASE(AMOMINU_W) {
					execute_amo_w(instr, PmaAmoClass::AMOArithmetic, TlmAmoOp::MinUnsigned,
					              [](int32_t a, int32_t b) { return std::min((uint32_t)a, (uint32_t)b); });
				}
				OP_END();

				OP_CASE(AMOMAX_W) {
					execute_amo_w(instr, PmaAmoClass::AMOArithmetic, TlmAmoOp::MaxSigned, [](int32_t a, int32_t b) { return std::max(a, b); });
				}
				OP_END();

				OP_CASE(AMOMAXU_W) {
					execute_amo_w(instr, PmaAmoClass::AMOArithmetic, TlmAmoOp::MaxUnsigned,
					              [](int32_t a, int32_t b) { return std::max((uint32_t)a, (uint32_t)b); });
				}
				OP_END();

				// RV Zfh extension

				OP_CASE(FLH) {
					fp_prepare_instr();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					trap_check_addr_alignment<2, true>(addr);
					fp_regs.write(RD, float16_t{(uint16_t)lscache.load_uhalf(addr)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSH) {
					fp_prepare_instr();
					uxlen_t addr = regs[instr.rs1()] + instr.S_imm();
					trap_check_addr_alignment<2, false>(addr);
					lscache.store_half(addr, fp_regs.u16(RS2));
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FADD_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_add(fp_regs.f16(RS1), fp_regs.f16(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSUB_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_sub(fp_regs.f16(RS1), fp_regs.f16(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMUL_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_mul(fp_regs.f16(RS1), fp_regs.f16(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FDIV_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_div(fp_regs.f16(RS1), fp_regs.f16(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSQRT_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_sqrt(fp_regs.f16(RS1)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMIN_H) {
					fp_prepare_instr();
					/*
					 * 0. check for signaling NaN input -> raise invalid operation execution flag
					 * 1. check if input is NaN
					 *    1.a check if both inputs are NaN -> return canonical NaN
					 *    1.b else return input, that is not NaN
					 */
					bool rs1_smaller = f16_lt_quiet(fp_regs.f16(RS1), fp_regs.f16(RS2)) ||
					                   (f16_eq(fp_regs.f16(RS1), fp_regs.f16(RS2)) && f16_isNegative(fp_regs.f16(RS1)));
					bool rs1_isnan = f16_isNaN(fp_regs.f16(RS1));
					bool rs2_isnan = f16_isNaN(fp_regs.f16(RS2));
					if (rs1_isnan && rs2_isnan) {
						fp_regs.write(RD, f16_defaultNaN);
					} else {
						if (rs1_smaller || rs2_isnan)
							fp_regs.write(RD, fp_regs.f16(RS1));
						else
							fp_regs.write(RD, fp_regs.f16(RS2));
					}

					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMAX_H) {
					fp_prepare_instr();
					/*
					 * 0. check for signaling NaN input -> raise invalid operation execution flag
					 * 1. check if input is NaN
					 *    1.a check if both inputs are NaN -> return canonical NaN
					 *    1.b else return input, that is not NaN
					 */
					bool rs1_greater = f16_lt_quiet(fp_regs.f16(RS2), fp_regs.f16(RS1)) ||
					                   (f16_eq(fp_regs.f16(RS2), fp_regs.f16(RS1)) && f16_isNegative(fp_regs.f16(RS2)));
					bool rs1_isnan = f16_isNaN(fp_regs.f16(RS1));
					bool rs2_isnan = f16_isNaN(fp_regs.f16(RS2));
					if (rs1_isnan && rs2_isnan) {
						fp_regs.write(RD, f16_defaultNaN);
					} else {
						if (rs1_greater || rs2_isnan)
							fp_regs.write(RD, fp_regs.f16(RS1));
						else
							fp_regs.write(RD, fp_regs.f16(RS2));
					}

					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMADD_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_mulAdd(fp_regs.f16(RS1), fp_regs.f16(RS2), fp_regs.f16(RS3)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMSUB_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_mulAdd(fp_regs.f16(RS1), fp_regs.f16(RS2), f16_neg(fp_regs.f16(RS3))));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FNMADD_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD,
					              f16_mulAdd(f16_neg(fp_regs.f16(RS1)), fp_regs.f16(RS2), f16_neg(fp_regs.f16(RS3))));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FNMSUB_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_mulAdd(f16_neg(fp_regs.f16(RS1)), fp_regs.f16(RS2), fp_regs.f16(RS3)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSGNJ_H) {
					fp_prepare_instr();
					auto f1 = fp_regs.f16(RS1);
					auto f2 = fp_regs.f16(RS2);
					uint16_t a = (f1.v & ~F16_SIGN_BIT) | (f2.v & F16_SIGN_BIT);
					fp_regs.write(RD, float16_t{a});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSGNJN_H) {
					fp_prepare_instr();
					auto f1 = fp_regs.f16(RS1);
					auto f2 = fp_regs.f16(RS2);
					uint16_t a = (f1.v & ~F16_SIGN_BIT) | (~f2.v & F16_SIGN_BIT);
					fp_regs.write(RD, float16_t{a});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSGNJX_H) {
					fp_prepare_instr();
					auto f1 = fp_regs.f16(RS1);
					auto f2 = fp_regs.f16(RS2);
					uint16_t a = f1.v ^ (f2.v & F16_SIGN_BIT);
					fp_regs.write(RD, float16_t{a});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FEQ_H) {
					fp_prepare_instr();
					regs[RD] = f16_eq(fp_regs.f16(RS1), fp_regs.f16(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FLT_H) {
					fp_prepare_instr();
					regs[RD] = f16_lt(fp_regs.f16(RS1), fp_regs.f16(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FLE_H) {
					fp_prepare_instr();
					regs[RD] = f16_le(fp_regs.f16(RS1), fp_regs.f16(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCLASS_H) {
					fp_prepare_instr();
					regs[RD] = (int16_t)f16_classify(fp_regs.f16(RS1));
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FMV_H_X) {
					fp_prepare_instr();
					fp_regs.write(RD, float16_t{(uint16_t)regs[RS1]});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FMV_X_H) {
					fp_prepare_instr();
					regs[RD] = (xlen_t)((int16_t)fp_regs.u32(RS1));
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_W_H) {
					fp_prepare_instr();
					fp_setup_rm();
					regs[RD] = f16_to_i32(fp_regs.f16(RS1), softfloat_roundingMode, true);
					fp_finish_instr();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_WU_H) {
					fp_prepare_instr();
					fp_setup_rm();
					regs[RD] = (int32_t)f16_to_ui32(fp_regs.f16(RS1), softfloat_roundingMode, true);
					fp_finish_instr();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_H_W) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, i32_to_f16((int32_t)regs[RS1]));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_H_WU) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, ui32_to_f16((int32_t)regs[RS1]));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_S_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_to_f32(fp_regs.f16(RS1)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_H_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_to_f16(fp_regs.f32(RS1)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_H_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_to_f16(fp_regs.f64(RS1)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_D_H) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f16_to_f64(fp_regs.f16(RS1)));
					fp_finish_instr();
				}
				OP_END();

				// RV64 F/D extension

				OP_CASE(FLW) {
					fp_prepare_instr();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					trap_check_addr_alignment<4, true>(addr);
					fp_regs.write(RD, float32_t{(uint32_t)lscache.load_uword(addr)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSW) {
					fp_prepare_instr();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.S_imm();
					trap_check_addr_alignment<4, false>(addr);
					lscache.store_word(addr, fp_regs.u32(RS2));
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FADD_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_add(fp_regs.f32(RS1), fp_regs.f32(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSUB_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_sub(fp_regs.f32(RS1), fp_regs.f32(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMUL_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_mul(fp_regs.f32(RS1), fp_regs.f32(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FDIV_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_div(fp_regs.f32(RS1), fp_regs.f32(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSQRT_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_sqrt(fp_regs.f32(RS1)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMIN_S) {
					fp_prepare_instr();
					/*
					 * 0. check for signaling NaN input -> raise invalid operation execution flag
					 * 1. check if input is NaN
					 *    1.a check if both inputs are NaN -> return canonical NaN
					 *    1.b else return input, that is not NaN
					 */
					bool rs1_smaller = f32_lt_quiet(fp_regs.f32(RS1), fp_regs.f32(RS2)) ||
					                   (f32_eq(fp_regs.f32(RS1), fp_regs.f32(RS2)) && f32_isNegative(fp_regs.f32(RS1)));
					bool rs1_isnan = f32_isNaN(fp_regs.f32(RS1));
					bool rs2_isnan = f32_isNaN(fp_regs.f32(RS2));
					if (rs1_isnan && rs2_isnan) {
						fp_regs.write(RD, f32_defaultNaN);
					} else {
						if (rs1_smaller || rs2_isnan)
							fp_regs.write(RD, fp_regs.f32(RS1));
						else
							fp_regs.write(RD, fp_regs.f32(RS2));
					}

					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMAX_S) {
					fp_prepare_instr();
					/*
					 * 0. check for signaling NaN input -> raise invalid operation execution flag
					 * 1. check if input is NaN
					 *    1.a check if both inputs are NaN -> return canonical NaN
					 *    1.b else return input, that is not NaN
					 */
					bool rs1_greater = f32_lt_quiet(fp_regs.f32(RS2), fp_regs.f32(RS1)) ||
					                   (f32_eq(fp_regs.f32(RS2), fp_regs.f32(RS1)) && f32_isNegative(fp_regs.f32(RS2)));
					bool rs1_isnan = f32_isNaN(fp_regs.f32(RS1));
					bool rs2_isnan = f32_isNaN(fp_regs.f32(RS2));
					if (rs1_isnan && rs2_isnan) {
						fp_regs.write(RD, f32_defaultNaN);
					} else {
						if (rs1_greater || rs2_isnan)
							fp_regs.write(RD, fp_regs.f32(RS1));
						else
							fp_regs.write(RD, fp_regs.f32(RS2));
					}

					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMADD_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_mulAdd(fp_regs.f32(RS1), fp_regs.f32(RS2), fp_regs.f32(RS3)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMSUB_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_mulAdd(fp_regs.f32(RS1), fp_regs.f32(RS2), f32_neg(fp_regs.f32(RS3))));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FNMADD_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD,
					              f32_mulAdd(f32_neg(fp_regs.f32(RS1)), fp_regs.f32(RS2), f32_neg(fp_regs.f32(RS3))));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FNMSUB_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_mulAdd(f32_neg(fp_regs.f32(RS1)), fp_regs.f32(RS2), fp_regs.f32(RS3)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_W_S) {
					fp_prepare_instr();
					fp_setup_rm();
					regs[RD] = f32_to_i32(fp_regs.f32(RS1), softfloat_roundingMode, true);
					fp_finish_instr();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_WU_S) {
					fp_prepare_instr();
					fp_setup_rm();
					regs[RD] = f32_to_ui32(fp_regs.f32(RS1), softfloat_roundingMode, true);
					fp_finish_instr();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_S_W) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, i32_to_f32(regs[RS1]));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_S_WU) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, ui32_to_f32(regs[RS1]));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSGNJ_S) {
					fp_prepare_instr();
					auto f1 = fp_regs.f32(RS1);
					auto f2 = fp_regs.f32(RS2);
					fp_regs.write(RD, float32_t{(f1.v & ~F32_SIGN_BIT) | (f2.v & F32_SIGN_BIT)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSGNJN_S) {
					fp_prepare_instr();
					auto f1 = fp_regs.f32(RS1);
					auto f2 = fp_regs.f32(RS2);
					fp_regs.write(RD, float32_t{(f1.v & ~F32_SIGN_BIT) | (~f2.v & F32_SIGN_BIT)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSGNJX_S) {
					fp_prepare_instr();
					auto f1 = fp_regs.f32(RS1);
					auto f2 = fp_regs.f32(RS2);
					fp_regs.write(RD, float32_t{f1.v ^ (f2.v & F32_SIGN_BIT)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FMV_W_X) {
					fp_prepare_instr();
					fp_regs.write(RD, float32_t{(uint32_t)regs[RS1]});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FMV_X_W) {
					fp_prepare_instr();
					regs[RD] = fp_regs.u32(RS1);
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FEQ_S) {
					fp_prepare_instr();
					regs[RD] = f32_eq(fp_regs.f32(RS1), fp_regs.f32(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FLT_S) {
					fp_prepare_instr();
					regs[RD] = f32_lt(fp_regs.f32(RS1), fp_regs.f32(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FLE_S) {
					fp_prepare_instr();
					regs[RD] = f32_le(fp_regs.f32(RS1), fp_regs.f32(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCLASS_S) {
					fp_prepare_instr();
					regs[RD] = f32_classify(fp_regs.f32(RS1));
					reset_reg_zero();
				}
				OP_END();

				// RV32D Extension

				OP_CASE(FLD) {
					fp_prepare_instr();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.I_imm();
					trap_check_addr_alignment<8, true>(addr);
					fp_regs.write(RD, float64_t{(uint64_t)lscache.load_double(addr)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSD) {
					fp_prepare_instr();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()] + instr.S_imm();
					trap_check_addr_alignment<8, false>(addr);
					lscache.store_double(addr, fp_regs.u64(RS2));
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FADD_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_add(fp_regs.f64(RS1), fp_regs.f64(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSUB_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_sub(fp_regs.f64(RS1), fp_regs.f64(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMUL_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_mul(fp_regs.f64(RS1), fp_regs.f64(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FDIV_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_div(fp_regs.f64(RS1), fp_regs.f64(RS2)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSQRT_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_sqrt(fp_regs.f64(RS1)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMIN_D) {
					fp_prepare_instr();
					/*
					 * 0. check for signaling NaN input -> raise invalid operation execution flag
					 * 1. check if input is NaN
					 *    1.a check if both inputs are NaN -> return canonical NaN
					 *    1.b else return input, that is not NaN
					 */
					bool rs1_smaller = f64_lt_quiet(fp_regs.f64(RS1), fp_regs.f64(RS2)) ||
					                   (f64_eq(fp_regs.f64(RS1), fp_regs.f64(RS2)) && f64_isNegative(fp_regs.f64(RS1)));
					bool rs1_isnan = f64_isNaN(fp_regs.f64(RS1));
					bool rs2_isnan = f64_isNaN(fp_regs.f64(RS2));
					if (rs1_isnan && rs2_isnan) {
						fp_regs.write(RD, f64_defaultNaN);
					} else {
						if (rs1_smaller || rs2_isnan)
							fp_regs.write(RD, fp_regs.f64(RS1));
						else
							fp_regs.write(RD, fp_regs.f64(RS2));
					}

					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMAX_D) {
					fp_prepare_instr();
					/*
					 * 0. check for signaling NaN input -> raise invalid operation execution flag
					 * 1. check if input is NaN
					 *    1.a check if both inputs are NaN -> return canonical NaN
					 *    1.b else return input, that is not NaN
					 */
					bool rs1_greater = f64_lt_quiet(fp_regs.f64(RS2), fp_regs.f64(RS1)) ||
					                   (f64_eq(fp_regs.f64(RS2), fp_regs.f64(RS1)) && f64_isNegative(fp_regs.f64(RS2)));
					bool rs1_isnan = f64_isNaN(fp_regs.f64(RS1));
					bool rs2_isnan = f64_isNaN(fp_regs.f64(RS2));
					if (rs1_isnan && rs2_isnan) {
						fp_regs.write(RD, f64_defaultNaN);
					} else {
						if (rs1_greater || rs2_isnan)
							fp_regs.write(RD, fp_regs.f64(RS1));
						else
							fp_regs.write(RD, fp_regs.f64(RS2));
					}

					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMADD_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_mulAdd(fp_regs.f64(RS1), fp_regs.f64(RS2), fp_regs.f64(RS3)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FMSUB_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_mulAdd(fp_regs.f64(RS1), fp_regs.f64(RS2), f64_neg(fp_regs.f64(RS3))));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FNMADD_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD,
					              f64_mulAdd(f64_neg(fp_regs.f64(RS1)), fp_regs.f64(RS2), f64_neg(fp_regs.f64(RS3))));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FNMSUB_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_mulAdd(f64_neg(fp_regs.f64(RS1)), fp_regs.f64(RS2), fp_regs.f64(RS3)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FSGNJ_D) {
					fp_prepare_instr();
					auto f1 = fp_regs.f64(RS1);
					auto f2 = fp_regs.f64(RS2);
					fp_regs.write(RD, float64_t{(f1.v & ~F64_SIGN_BIT) | (f2.v & F64_SIGN_BIT)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSGNJN_D) {
					fp_prepare_instr();
					auto f1 = fp_regs.f64(RS1);
					auto f2 = fp_regs.f64(RS2);
					fp_regs.write(RD, float64_t{(f1.v & ~F64_SIGN_BIT) | (~f2.v & F64_SIGN_BIT)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FSGNJX_D) {
					fp_prepare_instr();
					auto f1 = fp_regs.f64(RS1);
					auto f2 = fp_regs.f64(RS2);
					fp_regs.write(RD, float64_t{f1.v ^ (f2.v & F64_SIGN_BIT)});
					fp_set_dirty();
				}
				OP_END();

				OP_CASE(FEQ_D) {
					fp_prepare_instr();
					regs[RD] = f64_eq(fp_regs.f64(RS1), fp_regs.f64(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FLT_D) {
					fp_prepare_instr();
					regs[RD] = f64_lt(fp_regs.f64(RS1), fp_regs.f64(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FLE_D) {
					fp_prepare_instr();
					regs[RD] = f64_le(fp_regs.f64(RS1), fp_regs.f64(RS2));
					fp_update_exception_flags();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCLASS_D) {
					fp_prepare_instr();
					regs[RD] = (int64_t)f64_classify(fp_regs.f64(RS1));
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_W_D) {
					fp_prepare_instr();
					fp_setup_rm();
					regs[RD] = f64_to_i32(fp_regs.f64(RS1), softfloat_roundingMode, true);
					fp_finish_instr();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_WU_D) {
					fp_prepare_instr();
					fp_setup_rm();
					regs[RD] = (int32_t)f64_to_ui32(fp_regs.f64(RS1), softfloat_roundingMode, true);
					fp_finish_instr();
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(FCVT_D_W) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, i32_to_f64((int32_t)regs[RS1]));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_D_WU) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, ui32_to_f64((int32_t)regs[RS1]));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_S_D) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f64_to_f32(fp_regs.f64(RS1)));
					fp_finish_instr();
				}
				OP_END();

				OP_CASE(FCVT_D_S) {
					fp_prepare_instr();
					fp_setup_rm();
					fp_regs.write(RD, f32_to_f64(fp_regs.f32(RS1)));
					fp_finish_instr();
				}
				OP_END();

				/*
				 * RV-V Extension
				 * Note: handling of x0/zero is done in v_ext implementation (writes to zero/x0 are ignored)
				 */
				OP_CASE(VSETVLI) {
					v_ext.prepInstr(true, false, false);
					v_ext.v_set_operation(instr.rd(), instr.rs1(), instr.zimm_10(), 0);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSETIVLI) {
					v_ext.prepInstr(true, false, false);
					v_ext.v_set_operation(instr.rd(), 0, instr.zimm_9(), instr.rs1());
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSETVL) {
					v_ext.prepInstr(true, false, false);
					v_ext.v_set_operation(instr.rd(), instr.rs1(), regs[instr.rs2()], 0);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLM_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::masked);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSM_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::masked);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXEI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXEI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXEI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXEI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXEI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXEI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXEI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXEI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXEI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXEI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXEI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXEI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXEI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXEI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXEI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXEI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLE64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG2E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG2E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG2E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG2E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG2E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG2E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG2E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG2E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG2E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG2E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG2E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG2E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG2EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG2EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG2EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG2EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG2EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG2EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG2EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG2EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG2EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG2EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG2EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG2EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG2EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG2EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG2EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG2EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG2E64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG3E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG3E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG3E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG3E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG3E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG3E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG3E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG3E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG3E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG3E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG3E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG3E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG3EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG3EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG3EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG3EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG3EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG3EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG3EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG3EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG3EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG3EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG3EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG3EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG3EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG3EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG3EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG3EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG3E64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG4E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG4E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG4E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG4E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG4E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG4E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG4E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG4E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG4E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG4E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG4E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG4E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG4EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG4EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG4EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG4EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG4EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG4EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG4EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG4EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG4EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG4EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG4EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG4EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG4EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG4EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG4EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG4EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG4E64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG5E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG5E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG5E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG5E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG5E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG5E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG5E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG5E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG5E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG5E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG5E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG5E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG5EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG5EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG5EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG5EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG5EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG5EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG5EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG5EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG5EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG5EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG5EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG5EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG5EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG5EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG5EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG5EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG5E64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG6E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG6E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG6E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG6E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG6E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG6E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG6E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG6E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG6E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG6E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG6E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG6E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG6EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG6EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG6EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG6EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG6EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG6EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG6EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG6EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG6EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG6EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG6EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG6EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG6EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG6EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG6EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG6EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG6E64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG7E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG7E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG7E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG7E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG7E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG7E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG7E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG7E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG7E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG7E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG7E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG7E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG7EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG7EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG7EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG7EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG7EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG7EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG7EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG7EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG7EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG7EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG7EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG7EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG7EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG7EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG7EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG7EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG7E64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG8E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG8E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG8E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSEG8E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG8E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG8E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG8E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSSEG8E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG8E8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG8E16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG8E32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSSEG8E64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::standard_reg);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG8EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG8EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG8EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLUXSEG8EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG8EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG8EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG8EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLOXSEG8EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG8EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG8EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG8EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUXSEG8EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG8EI8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG8EI16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 16, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG8EI32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 32, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSOXSEG8EI64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 64, VExt::load_store_type_t::indexed);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E8FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E16FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E32FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VLSEG8E64FF_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, true, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::fofl);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL1RE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL1RE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL1RE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL1RE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VS1R_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL2RE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL2RE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL2RE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL2RE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VS2R_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL4RE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL4RE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL4RE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL4RE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VS4R_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL8RE8_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL8RE16_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 16, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL8RE32_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 32, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VL8RE64_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::load, 64, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VS8R_V) {
					stats.inc_loadstore();
					v_ext.prepInstr(true, false, false);
					v_ext.vLoadStore(VExt::load_store_t::store, 8, VExt::load_store_type_t::whole);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VADD_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VADD_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VADD_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUB_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSUB_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VRSUB_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vRSub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VRSUB_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vRSub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADD_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADD_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUB_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUB_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADDU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADDU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUBU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUBU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADD_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wwxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADD_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wwxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUB_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wwxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUB_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wwxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADDU_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWADDU_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAdd(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUBU_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWSUBU_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSub(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VZEXT_VF2) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vExt(2), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSEXT_VF2) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vExt(2), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VZEXT_VF4) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vExt(4), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSEXT_VF4) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vExt(4), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VZEXT_VF8) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vExt(8), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSEXT_VF8) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vExt(8), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VADC_VVM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vAdc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VADC_VXM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vAdc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VADC_VIM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vAdc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADC_VVM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMadc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADC_VXM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMadc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADC_VIM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMadc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADC_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMadc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADC_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMadc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADC_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMadc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSBC_VVM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vSbc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSBC_VXM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vSbc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSBC_VVM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMsbc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSBC_VXM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMsbc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSBC_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMsbc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSBC_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAll(v_ext.vMsbc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VAND_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAnd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VAND_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAnd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VAND_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAnd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VOR_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vOr(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VOR_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vOr(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VOR_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vOr(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VXOR_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vXor(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VXOR_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vXor(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VXOR_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vXor(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSLL_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSLL_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSLL_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSRL_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSRL_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSRL_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSRA_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSRA_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSRA_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNSRL_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNSRL_WI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNSRL_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNSRA_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xwxssu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNSRA_WI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xwxssu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNSRA_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShift(true), VExt::elem_sel_t::xwxssu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSEQ_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::eq), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSEQ_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::eq), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSEQ_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::eq), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSNE_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::ne), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSNE_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::ne), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSNE_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::ne), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLTU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::lt), VExt::elem_sel_t::xxxuuu,
					                     VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLTU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::lt), VExt::elem_sel_t::xxxuuu,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLT_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::lt), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLT_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::lt), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLEU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::le), VExt::elem_sel_t::xxxuuu,
					                     VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLEU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::le), VExt::elem_sel_t::xxxuuu,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLEU_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::le), VExt::elem_sel_t::xxxuus,
					                     VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLE_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::le), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLE_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::le), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSLE_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::le), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSGTU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::gt), VExt::elem_sel_t::xxxuuu,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSGTU_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::gt), VExt::elem_sel_t::xxxuus,
					                     VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSGT_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::gt), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSGT_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExtVoid(v_ext.vCompInt(VExt::int_compare_t::gt), VExt::elem_sel_t::xxxsss,
					                     VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMINU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMin(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMINU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMin(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMIN_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMin(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMIN_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMin(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMAXU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMax(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMAXU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMax(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMAX_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMax(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMAX_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMax(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMUL_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMUL_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMULH_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMulh(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMULH_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMulh(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMULHU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMulh(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMULHU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMulh(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMULHSU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMulh(), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMULHSU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMulh(), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VDIVU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vDiv(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VDIVU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vDiv(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VDIV_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vDiv(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VDIV_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vDiv(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREMU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vRem(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREMU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vRem(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREM_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vRem(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREM_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vRem(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMUL_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMUL_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMULU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMULU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMULSU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::wxxusu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMULSU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMul(), VExt::elem_sel_t::wxxusu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMACC_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMACC_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNMSAC_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vNmsac(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNMSAC_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vNmsac(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADD_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMadd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMADD_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMadd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNMSUB_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vNmsub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNMSUB_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vNmsub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMACCU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMACCU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMACC_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMACC_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMACCSU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::wxxuus, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMACCSU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::wxxuus, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWMACCUS_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVdExt(v_ext.vMacc(), VExt::elem_sel_t::wxxusu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMERGE_VVM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vMerge(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMERGE_VXM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vMerge(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMERGE_VIM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExtCarry(v_ext.vMerge(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMV_V_V) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMv(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMV_V_X) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMv(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMV_V_I) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vMv(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSADDU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSaddu(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSADDU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSaddu(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSADDU_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSaddu(), VExt::elem_sel_t::xxxuus, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSADD_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSadd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSADD_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSadd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSADD_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSadd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSUBU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSsubu(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSUBU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSsubu(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSUB_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSsub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSUB_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSsub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VAADDU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAadd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VAADDU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAadd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VAADD_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAadd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VAADD_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAadd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VASUBU_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAsub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VASUBU_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAsub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VASUB_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAsub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VASUB_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vAsub(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSMUL_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSmul(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSMUL_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vSmul(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSRL_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSRL_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSRL_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSRA_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(false), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSRA_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(false), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSSRA_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(false), VExt::elem_sel_t::xxxssu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNCLIPU_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNCLIPU_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNCLIPU_WI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNCLIP_WV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(true), VExt::elem_sel_t::xwxssu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNCLIP_WX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(true), VExt::elem_sel_t::xwxssu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VNCLIP_WI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoop(v_ext.vShiftRight(true), VExt::elem_sel_t::xwxssu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VFADD_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfAdd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFADD_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfAdd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSUB_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSUB_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFRSUB_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfrSub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWADD_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwAdd(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWADD_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwAdd(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWSUB_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwSub(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWSUB_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwSub(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWADD_WV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwAddw(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWADD_WF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwAddw(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWSUB_WV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwSubw(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWSUB_WF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwSubw(), VExt::elem_sel_t::wwxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMUL_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMul(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMUL_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMul(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFDIV_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfDiv(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFDIV_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfDiv(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFRDIV_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfrDiv(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWMUL_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwMul(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWMUL_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwMul(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMACC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMacc(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMACC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMacc(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMACC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmacc(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMACC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmacc(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMSAC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMsac(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMSAC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMsac(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMSAC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmsac(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMSAC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmsac(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMADD_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMadd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMADD_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMadd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMADD_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmadd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMADD_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmadd(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMSUB_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMsub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMSUB_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMsub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMSUB_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmsub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNMSUB_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfNmsub(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWMACC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwMacc(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWMACC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwMacc(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWNMACC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwNmacc(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWNMACC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwNmacc(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWMSAC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwMsac(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWMSAC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwMsac(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWNMSAC_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwNmsac(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWNMSAC_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfwNmsac(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSQRT_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSqrt(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFRSQRT7_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfRsqrt7(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFREC7_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfFrec7(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMIN_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMin(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMIN_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMin(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMAX_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMax(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMAX_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfMax(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSGNJ_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSgnj(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSGNJ_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSgnj(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSGNJN_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSgnjn(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSGNJN_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSgnjn(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSGNJX_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSgnjx(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFSGNJX_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfSgnjx(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFEQ_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfeq(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFEQ_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfeq(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFNE_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfneq(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFNE_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfneq(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFLT_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMflt(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFLT_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMflt(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFLE_VV) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfle(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFLE_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfle(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFGT_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfgt(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMFGE_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExtVoid(v_ext.vMfge(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFCLASS_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfClass(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMERGE_VFM) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopExtCarry(v_ext.vMerge(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMV_V_F) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoop(v_ext.vfMv(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFCVT_XU_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtXF(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFCVT_X_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtXF(false), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFCVT_RTZ_XU_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtXF(true), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFCVT_RTZ_X_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtXF(true), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFCVT_F_XU_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoop(v_ext.vfCvtFX(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFCVT_F_X_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoop(v_ext.vfCvtFX(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWCVT_XU_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtwXF(false), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWCVT_X_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtwXF(false), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWCVT_RTZ_XU_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtwXF(true), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWCVT_RTZ_X_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtwXF(true), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWCVT_F_XU_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoop(v_ext.vfCvtFX(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWCVT_F_X_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoop(v_ext.vfCvtFX(), VExt::elem_sel_t::wxxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWCVT_F_F_V) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtwFF(), VExt::elem_sel_t::wxxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_XU_F_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtnXF(false), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_X_F_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtnXF(false), VExt::elem_sel_t::xwxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_RTZ_XU_F_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtnXF(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_RTZ_X_F_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtnXF(true), VExt::elem_sel_t::xwxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_F_XU_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoop(v_ext.vfCvtFX(), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_F_X_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoop(v_ext.vfCvtFX(), VExt::elem_sel_t::xwxsss, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_F_F_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtnFF(false), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFNCVT_ROD_F_F_W) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVdExt(v_ext.vfCvtnFF(true), VExt::elem_sel_t::xwxuuu, VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VREDSUM_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedSum(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREDMAXU_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedMax(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREDMAX_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedMax(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREDMINU_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedMin(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREDMIN_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedMin(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREDAND_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedAnd(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREDOR_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedOr(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VREDXOR_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedXor(), VExt::elem_sel_t::xxxsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWREDSUMU_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedSum(), VExt::elem_sel_t::wxwuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VWREDSUM_VS) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopRed(v_ext.vRedSum(), VExt::elem_sel_t::wxwsss, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VFREDUSUM_VS) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopRed(v_ext.vfRedSum(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFREDOSUM_VS) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopRed(v_ext.vfRedSum(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFREDMAX_VS) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopRed(v_ext.vfRedMax(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFREDMIN_VS) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopRed(v_ext.vfRedMin(), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWREDUSUM_VS) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopRed(v_ext.vfwRedSum(), VExt::elem_sel_t::wxwuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFWREDOSUM_VS) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopRed(v_ext.vfwRedSum(), VExt::elem_sel_t::wxwuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VMAND_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_and));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMNAND_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_nand));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMANDN_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_andn));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMXOR_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_xor));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMOR_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_or));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMNOR_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_nor));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMORN_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_orn));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMXNOR_MM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidAllMask(v_ext.vMask(VExt::maskOperation::m_xnor));
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VCPOP_M) {
					v_ext.prepInstr(true, true, false);
					v_ext.vCpop();
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VFIRST_M) {
					v_ext.prepInstr(true, true, false);
					v_ext.vFirst();
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSBF_M) {
					v_ext.prepInstr(true, true, false);
					v_ext.vMs(VExt::vms_type_t::sbf);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSIF_M) {
					v_ext.prepInstr(true, true, false);
					v_ext.vMs(VExt::vms_type_t::sif);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMSOF_M) {
					v_ext.prepInstr(true, true, false);
					v_ext.vMs(VExt::vms_type_t::sof);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VIOTA_M) {
					v_ext.prepInstr(true, true, false);
					v_ext.vIota();
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VID_V) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoid(v_ext.vId(), VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMV_X_S) {
					v_ext.prepInstr(true, true, false);
					v_ext.vMvXs();
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMV_S_X) {
					v_ext.prepInstr(true, true, false);
					v_ext.vMvSx();
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VFMV_F_S) {
					v_ext.prepInstr(true, true, true);
					v_ext.vMvFs();
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VFMV_S_F) {
					v_ext.prepInstr(true, true, true);
					v_ext.vMvSf();
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VSLIDEUP_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidNoOverlap(v_ext.vSlideUp(regs[instr.rs1()]), VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSLIDEUP_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidNoOverlap(v_ext.vSlideUp(instr.rs1()), VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSLIDEDOWN_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoid(v_ext.vSlideDown(regs[instr.rs1()]), VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSLIDEDOWN_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoid(v_ext.vSlideDown(instr.rs1()), VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VSLIDE1UP_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoidNoOverlap(v_ext.vSlide1Up(VExt::param_sel_t::vx), VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VFSLIDE1UP_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVoidNoOverlap(v_ext.vSlide1Up(VExt::param_sel_t::vf), VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VSLIDE1DOWN_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopVoid(v_ext.vSlide1Down(VExt::param_sel_t::vx), VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VFSLIDE1DOWN_VF) {
					v_ext.prepInstr(true, true, true);
					v_ext.vLoopVoid(v_ext.vSlide1Down(VExt::param_sel_t::vf), VExt::param_sel_t::vf);
					v_ext.finishInstr(true);
				}
				OP_END();

				OP_CASE(VRGATHER_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExt(v_ext.vGather(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VRGATHEREI16_VV) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExt(v_ext.vGather(true), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vv);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VRGATHER_VX) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExt(v_ext.vGather(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vx);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VRGATHER_VI) {
					v_ext.prepInstr(true, true, false);
					v_ext.vLoopExt(v_ext.vGather(false), VExt::elem_sel_t::xxxuuu, VExt::param_sel_t::vi);
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VCOMPRESS_VM) {
					v_ext.prepInstr(true, true, false);
					v_ext.vCompress();
					v_ext.finishInstr(false);
				}
				OP_END();

				OP_CASE(VMV_NR_R_V) {
					v_ext.prepInstr(true, true, false);
					v_ext.vMvNr();
					v_ext.finishInstr(false);
				}
				OP_END();
				// RV-V Extension End -- Placeholder 6
				// privileged instructions

				OP_CASE(WFI) {
					// NOTE: only a hint, can be implemented as NOP
					// std::cout << "[sim:wfi] CSR mstatus.mie " << csrs.mstatus->mie << std::endl;
					release_lr_sc_reservation();

					if (s_mode() && csrs.mstatus.reg.fields.tw)
						RAISE_ILLEGAL_INSTRUCTION();

					if (u_mode() && csrs.misa.has_supervisor_mode_extension())
						RAISE_ILLEGAL_INSTRUCTION();

					stats.inc_wfi();
					if (!ignore_wfi) {
						while (!has_local_pending_enabled_interrupts()) {
							sc_core::wait(wfi_event);
						}
					}
				}
				OP_END();

				OP_CASE(SFENCE_VMA) {
					if (s_mode() && csrs.mstatus.reg.fields.tvm)
						RAISE_ILLEGAL_INSTRUCTION();
					dbbcache.fence_vma(pc);
					lscache.fence_vma();
					stats.inc_fence_vma();
				}
				OP_END();

				OP_CASE(HFENCE_GVMA) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: HFENCE.GVMA executed with V=1 must raise virtual-instruction before privilege checks
					if (s_mode() && csrs.mstatus.reg.fields.tvm)
						RAISE_ILLEGAL_INSTRUCTION();  // H Extension fdw: HFENCE.GVMA follows TVM-gated HS/M privilege rules in non-virtual execution
					if (!(s_mode() || prv == MachineMode))
						RAISE_ILLEGAL_INSTRUCTION();
					dbbcache.fence_vma(pc);
					lscache.fence_vma();
					stats.inc_fence_vma();
				}
				OP_END();

				OP_CASE(HFENCE_VVMA) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: HFENCE.VVMA executed with V=1 must raise virtual-instruction before privilege checks
					if (!(s_mode() || prv == MachineMode))
						RAISE_ILLEGAL_INSTRUCTION();
					dbbcache.fence_vma(pc);
					lscache.fence_vma();
					stats.inc_fence_vma();
				}
				OP_END();

				OP_CASE(HLV_B) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: HLV/HLVX must trap as virtual-instruction in VS/VU before any further privilege checks
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();  // H Extension fdw: U-mode guest-load access is only legal when hstatus.HU authorizes HLV/HLVX from U
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					regs[instr.rd()] = mem->guest_load_byte(addr);  // H Extension fdw: route HLV.B through the shared guest-translation memory path instead of trapping unconditionally
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLV_BU) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					regs[instr.rd()] = mem->guest_load_ubyte(addr);  // H Extension fdw: route HLV.BU through the shared guest-translation memory path
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLV_H) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<2, true>(addr);
					regs[instr.rd()] = mem->guest_load_half(addr);  // H Extension fdw: route HLV.H through the shared guest-translation memory path
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLV_HU) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<2, true>(addr);
					regs[instr.rd()] = mem->guest_load_uhalf(addr);  // H Extension fdw: route HLV.HU through the shared guest-translation memory path
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLVX_HU) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<2, true>(addr);
					regs[instr.rd()] = mem->guest_load_exec_uhalf(addr);  // H Extension fdw: route HLVX.HU through the shared guest-load path with execute-like permission checks
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLV_W) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<4, true>(addr);
					regs[instr.rd()] = mem->guest_load_word(addr);  // H Extension fdw: route HLV.W through the shared guest-translation memory path
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLV_WU) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<4, true>(addr);
					regs[instr.rd()] = mem->guest_load_uword(addr);  // H Extension fdw: route HLV.WU through the shared guest-translation memory path
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLVX_WU) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<4, true>(addr);
					regs[instr.rd()] = mem->guest_load_exec_uword(addr);  // H Extension fdw: route HLVX.WU through the shared guest-load path with execute-like permission checks
					reset_reg_zero();
				}
				OP_END();

				OP_CASE(HLV_D) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					RAISE_ILLEGAL_INSTRUCTION();      // H Extension fdw: keep RV32 HLV.D illegal for now; the shared guest path added here only covers byte/half/word forms
				}
				OP_END();

				OP_CASE(HSV_B) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: HSV must trap as virtual-instruction in VS/VU before any further privilege checks
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();  // H Extension fdw: U-mode guest-store access is only legal when hstatus.HU authorizes HSV from U
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					mem->guest_store_byte(addr, regs[instr.rs2()]);  // H Extension fdw: route HSV.B through the shared guest-translation memory path instead of trapping unconditionally
				}
				OP_END();

				OP_CASE(HSV_H) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<2, false>(addr);
					mem->guest_store_half(addr, regs[instr.rs2()]);  // H Extension fdw: route HSV.H through the shared guest-translation memory path
				}
				OP_END();

				OP_CASE(HSV_W) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					stats.inc_loadstore();
					uxlen_t addr = regs[instr.rs1()];
					trap_check_addr_alignment<4, false>(addr);
					mem->guest_store_word(addr, regs[instr.rs2()]);  // H Extension fdw: route HSV.W through the shared guest-translation memory path
				}
				OP_END();

				OP_CASE(HSV_D) {
					if (virt)
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());
					if (u_mode() && !csrs.hstatus.reg.fields.hu)
						RAISE_ILLEGAL_INSTRUCTION();
					RAISE_ILLEGAL_INSTRUCTION();      // H Extension fdw: keep RV32 HSV.D illegal for now; the shared guest path added here only covers byte/half/word forms
				}
				OP_END();

				OP_CASE(URET) {
					if (!csrs.misa.has_user_mode_extension())
						RAISE_ILLEGAL_INSTRUCTION();
					return_from_trap_handler(UserMode);
					stats.inc_uret();
				}
				OP_END();

				OP_CASE(SRET) {
					if (!csrs.misa.has_supervisor_mode_extension() || (s_mode() && csrs.mstatus.reg.fields.tsr))
						RAISE_ILLEGAL_INSTRUCTION();
					return_from_trap_handler(SupervisorMode);
					stats.inc_sret();
				}
				OP_END();

				OP_CASE(MRET) {
					return_from_trap_handler(MachineMode);
					stats.inc_mret();
				}
				OP_END();
			}
			OP_SWITCH_END();

		} catch (SimulationTrap &e) {
			uxlen_t last_pc = dbbcache.get_last_pc_exception_safe();

			if (trace) {
				std::cout << "take trap " << e.reason << ", mtval=" << boost::format("%x") % e.mtval
				          << ", pc=" << boost::format("%x") % last_pc << std::endl;
			}
			stats.inc_trap(e.reason);

			/*
			 * If a instruction traps, the number of instructions and cycles are
			 * not updated by the above flow.
			 * This is definitly wrong behavior, e.g. for ECALL, EBREAK, etc.
			 * where trapping is their intended behavior, but it difficult e.g.
			 * for faulting load/store instructions (e.g. page fault) and
			 * similar cases.
			 * There is no perfect solution for this, but for the moment, we decide
			 * to count faulting instructions.
			 */
			ninstr++;

			handle_trap(e, last_pc);
		}
	} while (1);

	/*
	 * ensure everything is synchronized before we leave
	 */

	/* update pc member variable */
	pc = dbbcache.get_pc_maybe_after_callback();

	/* update counters by local fast counters */
	commit_instructions(ninstr);
	commit_cycles();

	/* sync quantum: make sure that no action is missed */
	stats.inc_qk_sync();
	quantum_keeper.sync();
}
/*
 * end of exec_steps
 * restore diagnostic (see above)
 */
#pragma GCC diagnostic pop

uint64_t ISS_CT::_compute_and_get_current_cycles() {
	assert(cycle_counter % prop_clock_cycle_period == sc_core::SC_ZERO_TIME);
	assert(cycle_counter.value() % prop_clock_cycle_period.value() == 0);

	uint64_t num_cycles = cycle_counter.value() / prop_clock_cycle_period.value();

	return num_cycles;
}

bool ISS_CT::is_invalid_csr_access(uxlen_t csr_addr, bool is_write) {
	if (csr_addr == csr::FFLAGS_ADDR || csr_addr == csr::FRM_ADDR || csr_addr == csr::FCSR_ADDR) {
		fp_require_not_off();
	}
	if (csr_addr == csr::VSTART_ADDR || csr_addr == csr::VXSAT_ADDR || csr_addr == csr::VXRM_ADDR ||
	    csr_addr == csr::VCSR_ADDR || csr_addr == csr::VL_ADDR || csr_addr == csr::VTYPE_ADDR ||
	    csr_addr == csr::VLENB_ADDR) {
		v_ext.requireNotOff();
	}
	const PrivilegeLevel csr_prv = (0x300 & csr_addr) >> 8;
	// H Extension fdw: treat non-virtualized S-mode as HS when comparing against H/VS CSR privilege encodings.
	const PrivilegeLevel effective_prv = (!virt && prv == SupervisorMode) ? HypervisorMode : prv;
	const bool csr_readonly = ((0xC00 & csr_addr) >> 10) == 3;
	const bool missing_supervisor_extension = (csr_prv == SupervisorMode) && !csrs.misa.has_supervisor_mode_extension();
	const bool missing_user_extension = (csr_prv == UserMode) && !csrs.misa.has_user_mode_extension();

	if ((is_write && csr_readonly) || missing_supervisor_extension || missing_user_extension) {
		return true;
	}

	if (effective_prv < csr_prv) {
		if (virt && csr_prv <= HypervisorMode) {
			raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: VS-mode insufficient-privilege CSR accesses to S/HS-visible CSRs must raise virtual-instruction
		}
		return true;
	}

	return false;
}

void ISS_CT::validate_csr_counter_read_access_rights(uxlen_t addr) {
	// match against counter CSR addresses, see RISC-V privileged spec for the address definitions
	if ((addr >= 0xC00 && addr <= 0xC1F) || (addr >= 0xC80 && addr <= 0xC9F)) {
		auto cnt = addr & 0x1F;  // 32 counter in total, naturally aligned with the mcounteren and scounteren CSRs

		if (s_mode() && !csr::is_bitset(csrs.mcounteren, cnt))
			RAISE_ILLEGAL_INSTRUCTION();

		if (u_mode() && (!csr::is_bitset(csrs.mcounteren, cnt) || !csr::is_bitset(csrs.scounteren, cnt)))
			RAISE_ILLEGAL_INSTRUCTION();
	}
}

uxlen_t ISS_CT::get_csr_value(uxlen_t addr) {
	validate_csr_counter_read_access_rights(addr);

	auto read = [=](auto &x, uxlen_t mask) { return x.reg.val & mask; };

	using namespace csr;

	switch (addr) {
		case TIME_ADDR:
		case MTIME_ADDR: {
			uint64_t mtime = clint->update_and_get_mtime();
			if (virt)
				mtime += csrs.htimedelta.reg.val;  // H Extension fdw: reading time with V=1 must add htimedelta to the underlying machine time
			csrs.time.reg.val = mtime;
			return csrs.time.reg.words.low;
		}

		case TIMEH_ADDR:
		case MTIMEH_ADDR: {
			uint64_t mtime = clint->update_and_get_mtime();
			if (virt)
				mtime += csrs.htimedelta.reg.val;  // H Extension fdw: reading timeh with V=1 must add htimedelta before selecting the high half
			csrs.time.reg.val = mtime;
			return csrs.time.reg.words.high;
		}

		case CYCLE_ADDR:
		case MCYCLE_ADDR:
			commit_cycles();
			csrs.cycle.reg.val = _compute_and_get_current_cycles();
			return csrs.cycle.reg.words.low;

		case CYCLEH_ADDR:
		case MCYCLEH_ADDR:
			commit_cycles();
			csrs.cycle.reg.val = _compute_and_get_current_cycles();
			return csrs.cycle.reg.words.high;

		case MINSTRET_ADDR:
			return csrs.instret.reg.words.low;

		case MINSTRETH_ADDR:
			return csrs.instret.reg.words.high;

		SWITCH_CASE_MATCH_ANY_HPMCOUNTER_RV32:  // not implemented
			return 0;

		case MSTATUS_ADDR:
			return read(csrs.mstatus, MSTATUS_READ_MASK);

		case SSTATUS_ADDR:
			if (virt && prv == SupervisorMode)
				return read(csrs.vsstatus, VSSTATUS_READ_MASK);  // H Extension fdw: remap sstatus to vsstatus while executing in VS and keep VSSTATUS masking explicit
			return read(csrs.mstatus, SSTATUS_READ_MASK);
		case VSSTATUS_ADDR:
			return read(csrs.vsstatus, VSSTATUS_READ_MASK);  // H Extension fdw: route direct vsstatus reads through an explicit VSSTATUS mask
		case SCAUSE_ADDR:
			if (virt && prv == SupervisorMode)
				return csrs.vscause.reg.val;
			break;
		case VSCAUSE_ADDR:
			return csrs.vscause.reg.val;
		case SEPC_ADDR:
			if (virt && prv == SupervisorMode)
				return csrs.vsepc.reg.val;
			break;
		case STVEC_ADDR:
			if (virt && prv == SupervisorMode)
				return read(csrs.vstvec, MTVEC_MASK);
			break;
		case SSCRATCH_ADDR:
			if (virt && prv == SupervisorMode)
				return csrs.vsscratch.reg.val;
			break;
		case STVAL_ADDR:
			if (virt && prv == SupervisorMode)
				return csrs.vstval.reg.val;
			break;

		case USTATUS_ADDR:
			return read(csrs.mstatus, USTATUS_READ_MASK);

		case MSTATUSH_ADDR:
			return read(csrs.mstatush, csrs.misa.has_hypervisor_extension() ? MSTATUSH_READ_MASK
			                                                               : (MSTATUSH_READ_MASK & ~(MSTATUSH_GVA | MSTATUSH_MPV)));  // H Extension fdw: only expose GVA/MPV when misa.H is enabled

		case MIP_ADDR:
			return read(csrs.mip, MIP_READ_MASK) | (csrs.hvip.reg.val & csr::HVIP_MASK);  // H Extension fdw: mip must expose injected VS pending bits in the architected positions
		case SIP_ADDR:
			if (virt && prv == SupervisorMode)
				return compute_vs_visible_interrupts() & compute_vs_interrupt_pending_read_mask();  // H Extension fdw: expose only architecturally visible VS interrupt-pending bits through sip while executing in VS
			return read(csrs.mip, compute_hs_interrupt_pending_read_mask());  // H Extension fdw: keep non-visible HS interrupt-pending bits read-only zero in sip
		case VSIP_ADDR:
			return compute_vs_visible_interrupts() & compute_vs_interrupt_pending_read_mask();  // H Extension fdw: report only the effective VS pending bits that are architecturally visible
		case HIP_ADDR:
			return (read(csrs.mip, MIP_READ_MASK) | (csrs.hvip.reg.val & csr::HVIP_MASK)) & 0x1444;  // H Extension fdw: hip is the HS-visible pending subset sourced from mip plus injected VS pending bits
		case UIP_ADDR:
			return read(csrs.mip, UIP_MASK);

		case MIE_ADDR:
			return read(csrs.mie, MIE_MASK);
		case SIE_ADDR:
			if (virt && prv == SupervisorMode)
				return compute_vs_visible_interrupt_enables() & compute_vs_interrupt_enable_mask();  // H Extension fdw: expose the effective VS interrupt-enable view through sie while executing in VS, including delegated alias bits from mie
			return read(csrs.mie, compute_hs_interrupt_enable_mask());  // H Extension fdw: keep non-delegated S interrupt-enable bits read-only zero in sie
		case VSIE_ADDR:
			return compute_vs_visible_interrupt_enables();  // H Extension fdw: vsie must alias delegated enable bits out of mie and retain any explicit VS-only state in the VS view
		case HIE_ADDR:
			return read(csrs.mie, 0x1444);  // H Extension fdw: hie is the HS-visible interrupt-enable subset of mie
		case UIE_ADDR:
			return read(csrs.mie, UIE_MASK);

		case SATP_ADDR:
			if (virt && prv == SupervisorMode)
				return read(csrs.vsatp, SATP_MASK);
			if (csrs.mstatus.reg.fields.tvm)
				RAISE_ILLEGAL_INSTRUCTION();
			break;
		case STIMECMP_ADDR:
			if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
				RAISE_ILLEGAL_INSTRUCTION();
			if (virt && prv == SupervisorMode) {
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
					raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: VS-visible stimecmp accesses must raise virtual-instruction when henvcfg.STCE blocks the alias
				return csrs.vstimecmp.reg.words.low;  // H Extension fdw: route RV32 stimecmp low-half reads to vstimecmp while executing in VS
			}
			return csrs.stimecmp.reg.words.low;  // H Extension fdw: expose the real RV32 stimecmp low half once menvcfg.STCE enables Sstc
		case STIMECMPH_ADDR:
			if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
				RAISE_ILLEGAL_INSTRUCTION();
			if (virt && prv == SupervisorMode) {
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
					raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: VS-visible stimecmph accesses must raise virtual-instruction when henvcfg.STCE blocks the alias
				return csrs.vstimecmp.reg.words.high;  // H Extension fdw: route RV32 stimecmp high-half reads to vstimecmp while executing in VS
			}
			return csrs.stimecmp.reg.words.high;  // H Extension fdw: expose the real RV32 stimecmp high half once menvcfg.STCE enables Sstc
		case VSATP_ADDR:
			return read(csrs.vsatp, SATP_MASK);
		case VSTIMECMP_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
				RAISE_ILLEGAL_INSTRUCTION();
			if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
				raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: direct vstimecmp low-half accesses must fault when the hypervisor withholds VS timer compare visibility
			return csrs.vstimecmp.reg.words.low;  // H Extension fdw: expose the explicit RV32 low half of vstimecmp once both STCE gates are enabled
		case VSTIMECMPH_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
				RAISE_ILLEGAL_INSTRUCTION();
			if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
				raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: direct vstimecmph accesses must fault when the hypervisor withholds VS timer compare visibility
			return csrs.vstimecmp.reg.words.high;  // H Extension fdw: expose the explicit RV32 high half of vstimecmp once both STCE gates are enabled

		case HSTATUS_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return read(csrs.hstatus, HSTATUS_READ_MASK);  // H Extension fdw: route hstatus accesses through an explicit mask instead of default CSR storage
		case HGATP_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return read(csrs.hgatp, HGATP_MASK);  // H Extension fdw: expose only architecturally visible HGATP fields
		case HTVAL_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.htval.reg.val;  // H Extension fdw: expose HTVAL as an explicit full-width trap auxiliary CSR
		case HTINST_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.htinst.reg.val;  // H Extension fdw: expose HTINST as an explicit full-width trap auxiliary CSR
		case HVIP_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return (csrs.hvip.reg.val & (csr::HVIP_MASK & ~0x4)) | (csrs.mip.reg.val & 0x4);  // H Extension fdw: hvip.VSSIP aliases mip.VSSIP while VSTIP/VSEIP remain in explicit HVIP storage
		case HVIEN_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return read(csrs.hvien, HVIEN_MASK);  // H Extension fdw: expose only the VS-interrupt subset of HVIEN used by the current virtual interrupt path
		case HTIMEDELTA_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.htimedelta.reg.words.low;  // H Extension fdw: expose the low half of htimedelta explicitly in RV32
		case HENVCFG_ADDR: {
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			const uint64_t mask = HENVCFG_BASE_MASK | (csrs.menvcfg.reg.val & HENVCFG_GATED_MASK);
			return uint32_t(csrs.henvcfg.reg.val & mask);  // H Extension fdw: expose the low half of henvcfg after menvcfg-gated masking
		}
		case HTIMEDELTAH_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.htimedelta.reg.words.high;  // H Extension fdw: expose the high half of htimedelta explicitly in RV32
		case HGEIE_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.hgeie.reg.val & ~uxlen_t(1);  // H Extension fdw: hgeie[0] is architecturally read-only zero
		case HGEIP_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.hgeip.reg.val & ~uxlen_t(1);  // H Extension fdw: hgeip[0] is architecturally read-only zero
		case HENVCFGH_ADDR: {
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			const uint64_t mask = HENVCFG_BASE_MASK | (csrs.menvcfg.reg.val & HENVCFG_GATED_MASK);
			return uint32_t((csrs.henvcfg.reg.val & mask) >> 32);  // H Extension fdw: expose the high half of henvcfg after menvcfg-gated masking
		}

		case HEDELEG_ADDR:
			return read(csrs.hedeleg, HEDELEG_MASK);
		case HIDELEG_ADDR:
			return (read(csrs.hideleg, HIDELEG_MASK) << 1);  // H Extension fdw: hideleg is stored in the internal VS-view bit positions, but the architectural CSR exposes VS interrupt bits in the shifted physical positions
		case MIDELEG_ADDR:
			return read(csrs.mideleg, MIDELEG_MASK) | 0x1444;  // H Extension fdw: mideleg hardwires VSEIP/VSSIP/VSTIP/SGEIP to one when H is implemented

		case FCSR_ADDR:
			return read(csrs.fcsr, FCSR_MASK);

		case FFLAGS_ADDR:
			return csrs.fcsr.reg.fields.fflags;

		case FRM_ADDR:
			return csrs.fcsr.reg.fields.frm;

		case VCSR_ADDR:
			/* mirror vxrm and vxsat in vcsr */
			csrs.vcsr.reg.fields.vxrm = csrs.vxrm.reg.fields.vxrm;
			csrs.vcsr.reg.fields.vxsat = csrs.vxsat.reg.fields.vxsat;
			return csrs.vcsr.reg.val;
		case MTVAL2_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.mtval2.reg.val;  // H Extension fdw: expose MTVAL2 as an explicit full-width trap auxiliary CSR
		case MTINST_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			return csrs.mtinst.reg.val;  // H Extension fdw: expose MTINST as an explicit full-width trap auxiliary CSR

		// debug CSRs not supported, thus hardwired
		case TSELECT_ADDR:
			return 1;  // if a zero write by SW is preserved, then debug mode is supported (thus hardwire to non-zero)
		case TDATA1_ADDR:
		case TDATA2_ADDR:
		case TDATA3_ADDR:
		case DCSR_ADDR:
		case DPC_ADDR:
		case DSCRATCH0_ADDR:
		case DSCRATCH1_ADDR:
			return 0;
	}

	if (!csrs.is_valid_csr32_addr(addr))
		RAISE_ILLEGAL_INSTRUCTION();

	return csrs.default_read32(addr);
}

void ISS_CT::set_csr_value(uxlen_t addr, uxlen_t value) {
	auto write = [=](auto &x, uxlen_t mask) { x.reg.val = (x.reg.val & ~mask) | (value & mask); };

	using namespace csr;

	switch (addr) {
		case MISA_ADDR:                         // currently, read-only, thus cannot be changed at runtime
		SWITCH_CASE_MATCH_ANY_HPMCOUNTER_RV32:  // not implemented
			break;

		case SATP_ADDR: {
			if (virt && prv == SupervisorMode) {
				auto mode = csrs.vsatp.reg.fields.mode;
				write(csrs.vsatp, SATP_MASK);
				if (csrs.vsatp.reg.fields.mode != SATP_MODE_BARE && csrs.vsatp.reg.fields.mode != SATP_MODE_SV32)
					csrs.vsatp.reg.fields.mode = mode;
				break;
			}
			if (csrs.mstatus.reg.fields.tvm)
				RAISE_ILLEGAL_INSTRUCTION();
			write(csrs.satp, SATP_MASK);
			// std::cout << "[iss] satp=" << boost::format("%x") % csrs.satp.reg << std::endl;
		} break;
			case STIMECMP_ADDR:
				if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
					RAISE_ILLEGAL_INSTRUCTION();
				if (virt && prv == SupervisorMode) {
					if (!csrs.misa.has_hypervisor_extension())
						RAISE_ILLEGAL_INSTRUCTION();
					if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: VS-visible stimecmp writes must raise virtual-instruction when henvcfg.STCE blocks the alias
					csrs.vstimecmp.reg.words.low = value;  // H Extension fdw: route RV32 stimecmp low-half writes to vstimecmp while executing in VS
					break;
				}
				csrs.stimecmp.reg.words.low = value;  // H Extension fdw: keep the RV32 stimecmp low half explicitly writable once menvcfg.STCE enables Sstc
				break;
			case STIMECMPH_ADDR:
				if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
					RAISE_ILLEGAL_INSTRUCTION();
				if (virt && prv == SupervisorMode) {
					if (!csrs.misa.has_hypervisor_extension())
						RAISE_ILLEGAL_INSTRUCTION();
					if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
						raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: VS-visible stimecmph writes must raise virtual-instruction when henvcfg.STCE blocks the alias
					csrs.vstimecmp.reg.words.high = value;  // H Extension fdw: route RV32 stimecmp high-half writes to vstimecmp while executing in VS
					break;
				}
				csrs.stimecmp.reg.words.high = value;  // H Extension fdw: keep the RV32 stimecmp high half explicitly writable once menvcfg.STCE enables Sstc
				break;
			case VSATP_ADDR: {
				auto mode = csrs.vsatp.reg.fields.mode;
				write(csrs.vsatp, SATP_MASK);
				if (csrs.vsatp.reg.fields.mode != SATP_MODE_BARE && csrs.vsatp.reg.fields.mode != SATP_MODE_SV32)
					csrs.vsatp.reg.fields.mode = mode;
			} break;
			case VSTIMECMP_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
					RAISE_ILLEGAL_INSTRUCTION();
				if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
					raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: direct vstimecmp low-half writes must fault when the hypervisor withholds VS timer compare visibility
				csrs.vstimecmp.reg.words.low = value;  // H Extension fdw: keep the explicit RV32 low half of vstimecmp writable once both STCE gates are enabled
				break;
			case VSTIMECMPH_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				if (!(csrs.menvcfg.reg.val & MENVCFG_STCE))
					RAISE_ILLEGAL_INSTRUCTION();
				if (!(csrs.henvcfg.reg.val & HENVCFG_STCE))
					raise_trap(EXC_VIRT_INSTRUCTION_FAULT, instr.data());  // H Extension fdw: direct vstimecmph writes must fault when the hypervisor withholds VS timer compare visibility
				csrs.vstimecmp.reg.words.high = value;  // H Extension fdw: keep the explicit RV32 high half of vstimecmp writable once both STCE gates are enabled
				break;

			case HSTATUS_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				write(csrs.hstatus, HSTATUS_WRITE_MASK);  // H Extension fdw: keep hstatus writes explicit so reserved bits stay masked in RV32
				break;
			case HGATP_ADDR: {
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				auto mode = csrs.hgatp.reg.fields.mode;
				write(csrs.hgatp, HGATP_MASK);  // H Extension fdw: keep HGATP writes explicit so reserved bits stay masked and mode can be WARL-filtered
				if (csrs.hgatp.reg.fields.mode != HGATP_MODE_OFF && csrs.hgatp.reg.fields.mode != HGATP_MODE_SV32X4)
					csrs.hgatp.reg.fields.mode = mode;  // H Extension fdw: preserve previous HGATP mode if software writes an unsupported encoding
				break;
			}
			case HTVAL_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				csrs.htval.reg.val = value;  // H Extension fdw: keep HTVAL as an explicit full-width writable CSR for trap debugging and emulation state
				break;
			case HTINST_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				csrs.htinst.reg.val = value;  // H Extension fdw: keep HTINST as an explicit full-width writable CSR for trap debugging and emulation state
				break;
			case HVIP_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				csrs.mip.reg.val = (csrs.mip.reg.val & ~0x4) | (value & 0x4);  // H Extension fdw: hvip.VSSIP aliases mip.VSSIP on writes
				csrs.hvip.reg.val = (csrs.hvip.reg.val & ~(csr::HVIP_MASK & ~0x4)) | (value & (csr::HVIP_MASK & ~0x4));  // H Extension fdw: keep VSTIP/VSEIP in explicit HVIP storage while preserving the VSSIP alias semantics
				break;
			case HVIEN_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				write(csrs.hvien, HVIEN_MASK);  // H Extension fdw: keep HVIEN writes explicit so only the VS interrupt subset participates in injection
				break;
			case HTIMEDELTA_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				csrs.htimedelta.reg.words.low = value;  // H Extension fdw: keep the low half of htimedelta explicitly writable in RV32
				break;
			case HENVCFG_ADDR: {
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				const uint64_t mask = HENVCFG_BASE_MASK | (csrs.menvcfg.reg.val & HENVCFG_GATED_MASK);
				const uint64_t merged = (csrs.henvcfg.reg.val & 0xffffffff00000000ULL) | uint32_t(value);
				csrs.henvcfg.reg.val = (csrs.henvcfg.reg.val & ~mask) | (merged & mask);  // H Extension fdw: constrain the low half of henvcfg writes to locally modeled bits and menvcfg-gated capabilities
				break;
			}
			case HTIMEDELTAH_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				csrs.htimedelta.reg.words.high = value;  // H Extension fdw: keep the high half of htimedelta explicitly writable in RV32
				break;
			case HGEIE_ADDR:
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				csrs.hgeie.reg.val = value & ~uxlen_t(1);  // H Extension fdw: keep hgeie[0] read-only zero while leaving the implementation-defined GEILEN-controlled bits writable
				break;
			case HENVCFGH_ADDR: {
				if (!csrs.misa.has_hypervisor_extension())
					RAISE_ILLEGAL_INSTRUCTION();
				const uint64_t mask = HENVCFG_BASE_MASK | (csrs.menvcfg.reg.val & HENVCFG_GATED_MASK);
				const uint64_t merged = (csrs.henvcfg.reg.val & 0x00000000ffffffffULL) | (uint64_t(value) << 32);
				csrs.henvcfg.reg.val = (csrs.henvcfg.reg.val & ~mask) | (merged & mask);  // H Extension fdw: constrain the high half of henvcfg writes to locally modeled bits and menvcfg-gated capabilities
				break;
			}

			case HEDELEG_ADDR:
				write(csrs.hedeleg, HEDELEG_MASK);
				break;
			case HIDELEG_ADDR:
				csrs.hideleg.reg.val = (csrs.hideleg.reg.val & ~HIDELEG_MASK) | ((value >> 1) & HIDELEG_MASK);  // H Extension fdw: map architectural hideleg VS interrupt bit positions back into the internal VS-view storage
			break;

		case MTVEC_ADDR:
			write(csrs.mtvec, MTVEC_MASK);
			break;
		case STVEC_ADDR:
			if (virt && prv == SupervisorMode) {
				write(csrs.vstvec, MTVEC_MASK);
				break;
			}
			write(csrs.stvec, MTVEC_MASK);
			break;
		case VSTVEC_ADDR:
			write(csrs.vstvec, MTVEC_MASK);
			break;
		case UTVEC_ADDR:
			write(csrs.utvec, MTVEC_MASK);
			break;

		case MEPC_ADDR:
			write(csrs.mepc, pc_alignment_mask());
			break;
		case SEPC_ADDR:
			if (virt && prv == SupervisorMode) {
				write(csrs.vsepc, pc_alignment_mask());
				break;
			}
			write(csrs.sepc, pc_alignment_mask());
			break;
		case VSEPC_ADDR:
			write(csrs.vsepc, pc_alignment_mask());
			break;
		case UEPC_ADDR:
			write(csrs.uepc, pc_alignment_mask());
			break;

		case MSTATUS_ADDR:
			write(csrs.mstatus, MSTATUS_WRITE_MASK);
			break;
		case SSTATUS_ADDR:
			if (virt && prv == SupervisorMode) {
				write(csrs.vsstatus, VSSTATUS_WRITE_MASK);  // H Extension fdw: remap sstatus writes to vsstatus while executing in VS and keep VSSTATUS masking explicit
				break;
			}
			write(csrs.mstatus, SSTATUS_WRITE_MASK);
			break;
		case VSSTATUS_ADDR:
			write(csrs.vsstatus, VSSTATUS_WRITE_MASK);  // H Extension fdw: route direct vsstatus writes through an explicit VSSTATUS mask
			break;
		case SCAUSE_ADDR:
			if (virt && prv == SupervisorMode) {
				csrs.vscause.reg.val = value;
				break;
			}
			csrs.scause.reg.val = value;
			break;
		case VSCAUSE_ADDR:
			csrs.vscause.reg.val = value;
			break;
		case SSCRATCH_ADDR:
			if (virt && prv == SupervisorMode) {
				csrs.vsscratch.reg.val = value;
				break;
			}
			csrs.sscratch.reg.val = value;
			break;
		case STVAL_ADDR:
			if (virt && prv == SupervisorMode) {
				csrs.vstval.reg.val = value;
				break;
			}
			csrs.stval.reg.val = value;
			break;
		case USTATUS_ADDR:
			write(csrs.mstatus, USTATUS_WRITE_MASK);
			break;

		case MSTATUSH_ADDR:
			write(csrs.mstatush, csrs.misa.has_hypervisor_extension() ? MSTATUSH_WRITE_MASK
			                                                          : (MSTATUSH_WRITE_MASK & ~(MSTATUSH_GVA | MSTATUSH_MPV)));  // H Extension fdw: only allow GVA/MPV writes when misa.H is enabled
			break;

		case MIP_ADDR:
			write(csrs.mip, MIP_WRITE_MASK);
			break;
		case SIP_ADDR:
			if (virt && prv == SupervisorMode) {
				write(csrs.vsip, compute_vs_interrupt_pending_write_mask());  // H Extension fdw: route sip writes only to the writable VS software-pending subset while executing in VS
				break;
			}
			write(csrs.mip, compute_hs_interrupt_pending_write_mask());  // H Extension fdw: only delegated HS software-pending bits are writable through sip
			break;
		case VSIP_ADDR:
			csrs.mip.reg.val = (csrs.mip.reg.val & ~0x4) | ((((value & csrs.hideleg.reg.val & csr::SIP_MASK) << 1) & 0x4));  // H Extension fdw: delegated vsip software-pending writes alias mip.VSSIP in the architected shifted position
			csrs.vsip.reg.val = (csrs.vsip.reg.val & ~((((csrs.hvien.reg.val >> 1) & ~csrs.hideleg.reg.val) & csr::SIP_MASK))) |
			                    (value & (((csrs.hvien.reg.val >> 1) & ~csrs.hideleg.reg.val) & csr::SIP_MASK));  // H Extension fdw: preserve only non-delegated injected-visible VS software-pending state in the explicit VS view
			break;
		case HIP_ADDR:
			csrs.mip.reg.val = (csrs.mip.reg.val & ~0x4) | (value & 0x4);  // H Extension fdw: hip writes only the architecturally writable VSSIP alias in mip
			break;
		case UIP_ADDR:
			write(csrs.mip, UIP_MASK);
			break;

		case MIE_ADDR:
			write(csrs.mie, MIE_MASK);
			break;
		case SIE_ADDR:
			if (virt && prv == SupervisorMode) {
				write(csrs.vsie, compute_vs_interrupt_enable_mask());  // H Extension fdw: route sie writes to the writable VS interrupt-enable subset while executing in VS
				break;
			}
			write(csrs.mie, compute_hs_interrupt_enable_mask());  // H Extension fdw: only delegated HS interrupt-enable bits are writable through sie
			break;
		case VSIE_ADDR:
			csrs.mie.reg.val = (csrs.mie.reg.val & ~((csrs.hideleg.reg.val & csr::SIE_MASK) << 1)) |
			                   ((value & csrs.hideleg.reg.val & csr::SIE_MASK) << 1);  // H Extension fdw: delegated vsie writes alias into mie at the architected VS interrupt-enable positions
			csrs.vsie.reg.val = (csrs.vsie.reg.val & ~((((csrs.hvien.reg.val >> 1) & ~csrs.hideleg.reg.val) & csr::SIE_MASK))) |
			                    (value & (((csrs.hvien.reg.val >> 1) & ~csrs.hideleg.reg.val) & csr::SIE_MASK));  // H Extension fdw: retain any non-delegated injected-visible VS enable bits in the explicit VS view
			break;
		case HIE_ADDR:
			csrs.mie.reg.val = (csrs.mie.reg.val & ~0x1444) | (value & 0x1444);  // H Extension fdw: hie writes alias the HS-visible virtual interrupt-enable subset of mie
			break;
		case UIE_ADDR:
			write(csrs.mie, UIE_MASK);
			break;

		case MIDELEG_ADDR:
			write(csrs.mideleg, MIDELEG_MASK);
			csrs.mideleg.reg.val |= 0x1444;  // H Extension fdw: preserve the architecturally forced HS interrupt delegation bits as read-only one
			break;

		case MEDELEG_ADDR:
			write(csrs.medeleg, MEDELEG_MASK);
			break;
		case MTVAL2_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			csrs.mtval2.reg.val = value;  // H Extension fdw: keep MTVAL2 as an explicit full-width writable CSR for trap debugging and emulation state
			break;
		case MTINST_ADDR:
			if (!csrs.misa.has_hypervisor_extension())
				RAISE_ILLEGAL_INSTRUCTION();
			csrs.mtinst.reg.val = value;  // H Extension fdw: keep MTINST as an explicit full-width writable CSR for trap debugging and emulation state
			break;

		case SIDELEG_ADDR:
			write(csrs.sideleg, SIDELEG_MASK);
			break;

		case SEDELEG_ADDR:
			write(csrs.sedeleg, SEDELEG_MASK);
			break;

		case MCOUNTEREN_ADDR:
			write(csrs.mcounteren, MCOUNTEREN_MASK);
			break;

		case SCOUNTEREN_ADDR:
			write(csrs.scounteren, MCOUNTEREN_MASK);
			break;

		case MCOUNTINHIBIT_ADDR:
			commit_cycles();
			write(csrs.mcountinhibit, MCOUNTINHIBIT_MASK);
			break;

		case FCSR_ADDR:
			write(csrs.fcsr, FCSR_MASK);
			break;

		case FFLAGS_ADDR:
			csrs.fcsr.reg.fields.fflags = value;
			break;

		case FRM_ADDR:
			csrs.fcsr.reg.fields.frm = value;
			break;

		case VXSAT_ADDR:
			write(csrs.vxsat, VXSAT_MASK);
			/* mirror vxsat in vcsr */
			csrs.vcsr.reg.fields.vxsat = csrs.vxsat.reg.fields.vxsat;
			break;

		case VXRM_ADDR:
			write(csrs.vxrm, VXRM_MASK);
			/* mirror vxrm in vcsr */
			csrs.vcsr.reg.fields.vxrm = csrs.vxrm.reg.fields.vxrm;
			break;

		case VCSR_ADDR:
			write(csrs.vcsr, VCSR_MASK);
			/* mirror vxrm and vxsat in vcsr */
			csrs.vxrm.reg.fields.vxrm = csrs.vcsr.reg.fields.vxrm;
			csrs.vxsat.reg.fields.vxsat = csrs.vcsr.reg.fields.vxsat;
			break;

		case VTYPE_ADDR:
			write(csrs.vtype, VTYPE_MASK);

		// debug CSRs not supported, thus hardwired
		case TSELECT_ADDR:
		case TDATA1_ADDR:
		case TDATA2_ADDR:
		case TDATA3_ADDR:
		case DCSR_ADDR:
		case DPC_ADDR:
		case DSCRATCH0_ADDR:
		case DSCRATCH1_ADDR:
			break;

		default:
			if (!csrs.is_valid_csr32_addr(addr))
				RAISE_ILLEGAL_INSTRUCTION();

			csrs.default_write32(addr, value);
	}

	/*
	 * interrupt enables may have changed
	 * TODO: optimize -> move to specific csrs above
	 */
	maybe_interrupt_pending();
}

void ISS_CT::init(instr_memory_if *instr_mem, bool use_dbbcache, data_memory_if *data_mem, bool use_lscache,
                  clint_if *clint, uxlen_t entrypoint, uxlen_t sp_base) {
	this->instr_mem = instr_mem;
	this->mem = data_mem;
	this->clint = clint;
	regs[RegFile::sp] = rv_align_stack_pointer_address(sp_base);
	pc = entrypoint;

	/* TODO: make const? (make all label ptrs const?) */
	void *fast_abort_and_fdd_labelPtr = genOpMap();

	uint64_t hartId = get_hart_id();
	dbbcache.init(use_dbbcache, isa_config, hartId, instr_mem, opMap, fast_abort_and_fdd_labelPtr, entrypoint);
	lscache.init(use_lscache, hartId, data_mem);
	cycle_counter_raw_last = 0;
}

void ISS_CT::sys_exit() {
	shall_exit = true;
	force_slow_path();
}

unsigned ISS_CT::get_syscall_register_index() {
	if (csrs.misa.has_E_base_isa())
		return RegFile::a5;
	else
		return RegFile::a7;
}

uint64_t ISS_CT::read_register(unsigned idx) {
	return (uint32_t)regs.read(idx);  // NOTE: zero extend
}

void ISS_CT::write_register(unsigned idx, uint64_t value) {
	// Since the value parameter in the function prototype is
	// a uint64_t, signed integer values (e.g. -1) get promoted
	// to values within this range. For example, -1 would be
	// promoted to (2**64)-1. As such, we cannot perform a
	// Boost lexical or numeric cast to uint32_t here as
	// these perform bounds checks. Instead, we perform a C
	// cast without bounds checks.
	regs.write(idx, (uint32_t)value);
}

uint64_t ISS_CT::get_progam_counter(void) {
	return pc;
}

void ISS_CT::block_on_wfi(bool block) {
	ignore_wfi = !block;
}

void ISS_CT::set_status(CoreExecStatus s) {
	status = s;
	force_slow_path();
}

void ISS_CT::enable_debug(void) {
	debug_mode = true;
	force_slow_path();
}

void ISS_CT::insert_breakpoint(uint64_t addr) {
	breakpoints.insert(addr);
}

void ISS_CT::remove_breakpoint(uint64_t addr) {
	breakpoints.erase(addr);
}

uint64_t ISS_CT::get_hart_id() {
	return csrs.mhartid.reg.val;
}

std::vector<uint64_t> ISS_CT::get_registers(void) {
	std::vector<uint64_t> regvals;

	for (auto v : regs.regs) regvals.push_back((uint32_t)v);  // NOTE: zero extend

	return regvals;
}

void ISS_CT::fp_finish_instr() {
	fp_set_dirty();
	fp_update_exception_flags();
}

void ISS_CT::fp_prepare_instr() {
	assert(softfloat_exceptionFlags == 0);
	fp_require_not_off();
}

void ISS_CT::fp_set_dirty() {
	csrs.mstatus.reg.fields.sd = 1;
	csrs.mstatus.reg.fields.fs = FS_DIRTY;
}

void ISS_CT::fp_update_exception_flags() {
	if (softfloat_exceptionFlags) {
		fp_set_dirty();
		csrs.fcsr.reg.fields.fflags |= softfloat_exceptionFlags;
		softfloat_exceptionFlags = 0;
	}
}

void ISS_CT::fp_setup_rm() {
	auto rm = instr.frm();
	if (rm == FRM_DYN)
		rm = csrs.fcsr.reg.fields.frm;
	if (rm > FRM_RMM)
		RAISE_ILLEGAL_INSTRUCTION();
	softfloat_roundingMode = rm;
}

void ISS_CT::fp_require_not_off() {
	if (csrs.mstatus.reg.fields.fs == FS_OFF)
		RAISE_ILLEGAL_INSTRUCTION();
}

void ISS_CT::return_from_trap_handler(PrivilegeLevel return_mode) {
	constexpr uxlen_t STATUS_SIE = uxlen_t(1) << 1;    // H Extension fdw: shared S/VS interrupt-enable bit position
	constexpr uxlen_t STATUS_SPIE = uxlen_t(1) << 5;   // H Extension fdw: shared S/VS previous interrupt-enable bit position
	constexpr uxlen_t STATUS_SPP = uxlen_t(1) << 8;    // H Extension fdw: shared S/VS previous privilege bit position

	switch (return_mode) {
		case MachineMode:
			prv = csrs.mstatus.reg.fields.mpp;
			virt = ((csrs.get_full_mstatus() & csr::MSTATUS_MPV) != 0) &&
			       (prv != MachineMode);  // H Extension fdw: MPV only re-enables virtualization when MRET leaves M-mode
			csrs.mstatus.reg.fields.mie = csrs.mstatus.reg.fields.mpie;
			csrs.mstatus.reg.fields.mpie = 1;
			pc = csrs.mepc.reg.val;
			csrs.set_full_mstatus(csrs.get_full_mstatus() & ~csr::MSTATUS_MPV);  // H Extension fdw: clear MPV through logical 64-bit mstatus view
			if (csrs.misa.has_user_mode_extension())
				csrs.mstatus.reg.fields.mpp = UserMode;
			else
				csrs.mstatus.reg.fields.mpp = MachineMode;
			break;

		case SupervisorMode:
			if (virt) {
				// H Extension fdw: VS SRET consumes vsstatus/vsepc and stays in virtualized privilege space.
				prv = (csrs.vsstatus.reg.val & STATUS_SPP) ? SupervisorMode : UserMode;
				csrs.vsstatus.reg.val = (csrs.vsstatus.reg.val & ~STATUS_SIE) |
				                        ((csrs.vsstatus.reg.val & STATUS_SPIE) ? STATUS_SIE : 0);
				csrs.vsstatus.reg.val |= STATUS_SPIE;
				if (csrs.misa.has_user_mode_extension())
					csrs.vsstatus.reg.val &= ~STATUS_SPP;
				else
					csrs.vsstatus.reg.val |= STATUS_SPP;
				pc = csrs.vsepc.reg.val;
				virt = true;
			} else {
				// H Extension fdw: HS SRET consumes hstatus.SPV and returns through HS-owned sepc.
				prv = csrs.mstatus.reg.fields.spp;
				virt = (csrs.hstatus.reg.val & csr::HSTATUS_SPV) != 0;
				csrs.hstatus.reg.val &= ~csr::HSTATUS_SPV;
				csrs.mstatus.reg.fields.sie = csrs.mstatus.reg.fields.spie;
				csrs.mstatus.reg.fields.spie = 1;
				pc = csrs.sepc.reg.val;
				if (csrs.misa.has_user_mode_extension())
					csrs.mstatus.reg.fields.spp = UserMode;
				else
					csrs.mstatus.reg.fields.spp = SupervisorMode;
			}
			break;

		case UserMode:
			prv = UserMode;
			virt = false;
			csrs.mstatus.reg.fields.uie = csrs.mstatus.reg.fields.upie;
			csrs.mstatus.reg.fields.upie = 1;
			pc = csrs.uepc.reg.val;
			break;

		default:
			throw std::runtime_error("unknown privilege level " + std::to_string(return_mode));
	}

	if (trace)
		printf("[vp::iss] return from trap handler, time %s, pc %8x, prv %1x\n",
		       quantum_keeper.get_current_time().to_string().c_str(), pc, prv);

	dbbcache.ret_trap(pc);
	force_slow_path();
}

void ISS_CT::trigger_external_interrupt(PrivilegeLevel level) {
	stats.inc_irq_trig_ext(level);

	if (trace)
		std::cout << "[vp::iss] trigger external interrupt, " << sc_core::sc_time_stamp() << std::endl;

	switch (level) {
		case UserMode:
			csrs.mip.reg.fields.ueip = true;
			break;
		case SupervisorMode:
			csrs.mip.reg.fields.seip = true;
			break;
		case MachineMode:
			csrs.mip.reg.fields.meip = true;
			break;
	}

	maybe_interrupt_pending();
}

void ISS_CT::clear_external_interrupt(PrivilegeLevel level) {
	if (trace)
		std::cout << "[vp::iss] clear external interrupt, " << sc_core::sc_time_stamp() << std::endl;

	switch (level) {
		case UserMode:
			csrs.mip.reg.fields.ueip = false;
			break;
		case SupervisorMode:
			csrs.mip.reg.fields.seip = false;
			break;
		case MachineMode:
			csrs.mip.reg.fields.meip = false;
			break;
	}
}

void ISS_CT::trigger_timer_interrupt() {
	stats.inc_irq_trig_timer();

	if (trace)
		std::cout << "[vp::iss] trigger timer interrupt, " << sc_core::sc_time_stamp() << std::endl;
	csrs.mip.reg.fields.mtip = true;
	maybe_interrupt_pending();
}

void ISS_CT::clear_timer_interrupt() {
	if (trace)
		std::cout << "[vp::iss] clear timer interrupt, " << sc_core::sc_time_stamp() << std::endl;
	csrs.mip.reg.fields.mtip = false;
}

void ISS_CT::trigger_software_interrupt() {
	stats.inc_irq_trig_sw();

	if (trace)
		std::cout << "[vp::iss] trigger software interrupt, " << sc_core::sc_time_stamp() << std::endl;
	csrs.mip.reg.fields.msip = true;
	maybe_interrupt_pending();
}

void ISS_CT::clear_software_interrupt() {
	if (trace)
		std::cout << "[vp::iss] clear software interrupt, " << sc_core::sc_time_stamp() << std::endl;
	csrs.mip.reg.fields.msip = false;
}

void ISS_CT::halt() {
	if (debug_mode) {
		this->status = CoreExecStatus::HitBreakpoint;
	}
}

std::string ISS_CT::name() {
	return this->systemc_name;
}

PrivilegeLevel ISS_CT::prepare_trap(SimulationTrap &e, uxlen_t last_pc) {
	// undo any potential pc update (for traps the pc should point to the originating instruction and not it's
	// successor)
	pc = last_pc;
	trap_to_virtual_supervisor = false;
	unsigned exc_bit = (1 << e.reason);

	if (virt && prv <= SupervisorMode && (exc_bit & csrs.medeleg.reg.val) && (exc_bit & csrs.hedeleg.reg.val)) {
		// H Extension fdw: delegated virtualized traps stay in VS and use the VS trap CSRs.
		csrs.vscause.reg.fields.interrupt = 0;
		csrs.vscause.reg.fields.exception_code = e.reason;
		csrs.vsepc.reg.val = boost::lexical_cast<uxlen_t>(pc);
		csrs.vstval.reg.val = boost::lexical_cast<uxlen_t>(e.mtval);
		trap_to_virtual_supervisor = true;
		return SupervisorMode;
	}

	// 1) machine mode execution takes any traps, independent of delegation setting
	// 2) non-delegated traps are processed in machine mode, independent of current execution mode
	if (prv == MachineMode || !(exc_bit & csrs.medeleg.reg.val)) {
		csrs.mcause.reg.fields.interrupt = 0;
		csrs.mcause.reg.fields.exception_code = e.reason;
		csrs.mtval.reg.val = boost::lexical_cast<uxlen_t>(e.mtval);
		return MachineMode;
	}

	if (virt) {
		// H Extension fdw: non-VS traps taken while V=1 always land in HS once M-mode delegation allows them.
		csrs.scause.reg.fields.interrupt = 0;
		csrs.scause.reg.fields.exception_code = e.reason;
		csrs.stval.reg.val = boost::lexical_cast<uxlen_t>(e.mtval);
		return SupervisorMode;
	}

	// see above machine mode comment
	if (prv == SupervisorMode || !(exc_bit & csrs.sedeleg.reg.val)) {
		csrs.scause.reg.fields.interrupt = 0;
		csrs.scause.reg.fields.exception_code = e.reason;
		csrs.stval.reg.val = boost::lexical_cast<uxlen_t>(e.mtval);
		return SupervisorMode;
	}

	assert(prv == UserMode && (exc_bit & csrs.medeleg.reg.val) && (exc_bit & csrs.sedeleg.reg.val));
	csrs.ucause.reg.fields.interrupt = 0;
	csrs.ucause.reg.fields.exception_code = e.reason;
	csrs.utval.reg.val = boost::lexical_cast<uxlen_t>(e.mtval);
	return UserMode;
}

void ISS_CT::prepare_interrupt(const PendingInterrupts &e) {
	if (trace) {
		std::cout << "[vp::iss] prepare interrupt, pending=" << e.pending << ", target-mode=" << e.target_mode
		          << std::endl;
	}

	csr_mip x{e.pending};
	trap_to_virtual_supervisor = false;
	const bool has_virtual_supervisor_source = (e.source_pending & csr::HVIP_MASK) != 0;

	ExceptionCode exc;
	if (x.reg.fields.meip)
		exc = EXC_M_EXTERNAL_INTERRUPT;
	else if (x.reg.fields.msip)
		exc = EXC_M_SOFTWARE_INTERRUPT;
	else if (x.reg.fields.mtip)
		exc = EXC_M_TIMER_INTERRUPT;
	else if (x.reg.fields.seip)
		exc = EXC_S_EXTERNAL_INTERRUPT;
	else if (x.reg.fields.ssip)
		exc = EXC_S_SOFTWARE_INTERRUPT;
	else if (x.reg.fields.stip)
		exc = EXC_S_TIMER_INTERRUPT;
	else if (x.reg.fields.ueip)
		exc = EXC_U_EXTERNAL_INTERRUPT;
	else if (x.reg.fields.usip)
		exc = EXC_U_SOFTWARE_INTERRUPT;
	else if (x.reg.fields.utip)
		exc = EXC_U_TIMER_INTERRUPT;
	else
		throw std::runtime_error("some pending interrupt must be available here");

	if (e.target_mode == SupervisorMode) {
		// H Extension fdw: delegated timer/software/external interrupts retain the
		// physical pending source in mip/hvip, but HS/VS trap handlers must observe
		// supervisor-level interrupt causes.
		if (exc == EXC_M_EXTERNAL_INTERRUPT)
			exc = EXC_S_EXTERNAL_INTERRUPT;
		else if (exc == EXC_M_SOFTWARE_INTERRUPT)
			exc = EXC_S_SOFTWARE_INTERRUPT;
		else if (exc == EXC_M_TIMER_INTERRUPT)
			exc = EXC_S_TIMER_INTERRUPT;
	}

	switch (e.target_mode) {
		case MachineMode:
			csrs.mcause.reg.fields.exception_code = exc;
			csrs.mcause.reg.fields.interrupt = 1;
			break;

		case SupervisorMode:
			if (virt && (e.pending & csrs.hideleg.reg.val)) {
				// H Extension fdw: delegated virtual interrupts target VS instead of HS.
				csrs.vscause.reg.fields.exception_code = exc;
				csrs.vscause.reg.fields.interrupt = 1;
				trap_to_virtual_supervisor = true;
			} else {
				if (has_virtual_supervisor_source) {
					// H Extension fdw: HS must report legacy VSSIP/VSTIP/VSEIP sources using the virtual-supervisor interrupt cause encodings rather than collapsing them into ordinary supervisor interrupts.
					if (e.source_pending & 0x400)
						exc = EXC_VS_EXTERNAL_INTERRUPT;
					else if (e.source_pending & 0x4)
						exc = EXC_VS_SOFTWARE_INTERRUPT;
					else if (e.source_pending & 0x40)
						exc = EXC_VS_TIMER_INTERRUPT;
				}
				csrs.scause.reg.fields.exception_code = exc;
				csrs.scause.reg.fields.interrupt = 1;
			}
			break;

		case UserMode:
			csrs.ucause.reg.fields.exception_code = exc;
			csrs.ucause.reg.fields.interrupt = 1;
			break;

		default:
			throw std::runtime_error("unknown privilege level " + std::to_string(e.target_mode));
	}
}

#ifndef RV32_VS_LEGACY_INTERRUPT_HELPERS
#define RV32_VS_LEGACY_INTERRUPT_HELPERS
static inline uxlen_t normalize_vs_legacy_interrupt_bits(uxlen_t pending) {
	// H Extension fdw: VP++ keeps VSSIP/VSTIP/VSEIP pending sources in raw mip/hvip positions, but lower-level routing and cause selection must see them in the supervisor-style bit positions.
	constexpr uxlen_t vs_legacy_raw_mask = csr::HVIP_MASK;
	return (pending & ~vs_legacy_raw_mask) | ((pending & vs_legacy_raw_mask) >> 1);
}

static inline uxlen_t expand_vs_legacy_interrupt_bits(uxlen_t pending) {
	// H Extension fdw: hideleg is stored in internal VS-view positions, so interrupt-routing decisions against raw mip/hvip state need the architected shifted positions.
	return (pending << 1) & csr::HVIP_MASK;
}

static inline uxlen_t effective_mideleg_bits(uxlen_t mideleg) {
	// H Extension fdw: VSSIP/VSTIP/VSEIP/SGEIP are architecturally read-only one in mideleg when H is implemented, so interrupt routing must observe them even if software never writes mideleg.
	return mideleg | uxlen_t(0x1444);
}
#endif

uxlen_t ISS_CT::compute_hs_interrupt_enable_mask() {
	// H Extension fdw: without AIA/mvien support, HS-visible sie bits track only S interrupts delegated from M.
	return effective_mideleg_bits(csrs.mideleg.reg.val) & csr::SIE_MASK;
}

uxlen_t ISS_CT::compute_hs_interrupt_pending_read_mask() {
	// H Extension fdw: sip exposes the delegated HS interrupt-pending view, but non-delegated bits stay read-only zero.
	return effective_mideleg_bits(csrs.mideleg.reg.val) & csr::SIE_MASK;
}

uxlen_t ISS_CT::compute_hs_interrupt_pending_write_mask() {
	// H Extension fdw: without mvip/mvien, only delegated HS software-pending bits are writable through sip.
	return effective_mideleg_bits(csrs.mideleg.reg.val) & csr::SIP_MASK;
}

uxlen_t ISS_CT::compute_vs_interrupt_enable_mask() {
	// H Extension fdw: VS-visible sie bits exist only for interrupts delegated to VS or explicitly injected through HVIEN.
	return (csrs.hideleg.reg.val | csrs.hvien.reg.val) & csr::SIE_MASK;
}

uxlen_t ISS_CT::compute_vs_visible_interrupt_enables() {
	// H Extension fdw: delegated VS interrupt enables alias out of mie, while any non-delegated injected-visible enables stay in the explicit vsie storage.
	return (((csrs.mie.reg.val >> 1) & csrs.hideleg.reg.val & csr::SIE_MASK) |
	        (csrs.vsie.reg.val & (((csrs.hvien.reg.val >> 1) & ~csrs.hideleg.reg.val) & csr::SIE_MASK)));
}

uxlen_t ISS_CT::compute_vs_interrupt_pending_read_mask() {
	// H Extension fdw: vsip exposes the delegated-or-injected VS interrupt-pending view, but invisible bits stay read-only zero.
	return (csrs.hideleg.reg.val | csrs.hvien.reg.val) & csr::SIE_MASK;
}

uxlen_t ISS_CT::compute_vs_interrupt_pending_write_mask() {
	// H Extension fdw: without full AIA support, direct VS pending writes only model delegated software-pending bits.
	return csrs.hideleg.reg.val & csr::SIP_MASK;
}

uxlen_t ISS_CT::compute_vs_visible_interrupts() {
	// H Extension fdw: combine delegated VS interrupts and hypervisor-injected VS interrupts after normalizing the legacy VS bit positions into the internal supervisor-style view used by VS CSRs and trap causes.
	const uxlen_t vs_mask = compute_vs_interrupt_pending_read_mask();
	const uxlen_t mideleg = effective_mideleg_bits(csrs.mideleg.reg.val);
	const uxlen_t raw_hideleg = expand_vs_legacy_interrupt_bits(csrs.hideleg.reg.val);
	const uxlen_t delegated_vs_pending =
	    normalize_vs_legacy_interrupt_bits(csrs.mip.reg.val & csrs.mie.reg.val & mideleg & raw_hideleg) &
	    vs_mask;
	const uxlen_t raw_hvip = (csrs.hvip.reg.val & (csr::HVIP_MASK & ~uxlen_t(0x4))) | (csrs.mip.reg.val & 0x4);
	const uxlen_t injected_vs_pending = normalize_vs_legacy_interrupt_bits(raw_hvip) & vs_mask;
	return delegated_vs_pending | injected_vs_pending | (csrs.vsip.reg.val & vs_mask);
}

PendingInterrupts ISS_CT::compute_pending_interrupts() {
	uxlen_t pending = csrs.mie.reg.val & csrs.mip.reg.val;
	const uxlen_t raw_vs_legacy_mask = csr::HVIP_MASK;
	const uxlen_t mideleg = effective_mideleg_bits(csrs.mideleg.reg.val);

	if (!pending)
		return {NoneMode, 0, 0};

	auto m_pending = pending & ~mideleg & ~raw_vs_legacy_mask;
	if (m_pending && (prv < MachineMode || (prv == MachineMode && csrs.mstatus.reg.fields.mie))) {
		return {MachineMode, m_pending, m_pending};
	}

	const uxlen_t delegated_pending = pending & mideleg;
	pending = normalize_vs_legacy_interrupt_bits(delegated_pending);

	if (virt) {
		auto vs_pending = compute_vs_visible_interrupts() & compute_vs_visible_interrupt_enables();
		const bool vs_enabled = (prv < SupervisorMode) || (prv == SupervisorMode && ((csrs.vsstatus.reg.val >> 1) & 0x1));
		if (vs_pending && vs_enabled) {
			return {SupervisorMode, vs_pending, delegated_pending & expand_vs_legacy_interrupt_bits(csrs.hideleg.reg.val)};
		}

		auto hs_pending = pending & ~csrs.hideleg.reg.val;
		const bool hs_enabled = virt || (prv < SupervisorMode) ||
		                        (prv == SupervisorMode && csrs.mstatus.reg.fields.sie);  // H Extension fdw: while executing in VS/VU, HS interrupts remain globally enabled regardless of sstatus.SIE
		if (hs_pending && hs_enabled) {
			return {SupervisorMode, hs_pending, delegated_pending & ~expand_vs_legacy_interrupt_bits(csrs.hideleg.reg.val)};
		}

		return {NoneMode, 0, 0};
	}

	auto s_pending = pending & ~csrs.sideleg.reg.val;
	if (s_pending && (prv < SupervisorMode || (prv == SupervisorMode && csrs.mstatus.reg.fields.sie))) {
		return {SupervisorMode, s_pending, delegated_pending & ~csrs.sideleg.reg.val};
	}

	auto u_pending = pending & csrs.sideleg.reg.val;
	if (u_pending && (prv == UserMode && csrs.mstatus.reg.fields.uie)) {
		return {UserMode, u_pending, delegated_pending & csrs.sideleg.reg.val};
	}

	return {NoneMode, 0, 0};
}

void ISS_CT::switch_to_trap_handler(PrivilegeLevel target_mode, const SimulationTrap *trap) {
	if (trace) {
		printf("[vp::iss] switch to trap handler, time %s, before pc %8x, irq %u, t-prv %1x\n",
		       quantum_keeper.get_current_time().to_string().c_str(), pc, csrs.mcause.reg.fields.interrupt,
		       target_mode);
	}

	constexpr uxlen_t STATUS_SIE = uxlen_t(1) << 1;   // H Extension fdw: shared S/VS interrupt-enable bit position during trap entry
	constexpr uxlen_t STATUS_SPIE = uxlen_t(1) << 5;  // H Extension fdw: shared S/VS previous interrupt-enable bit position during trap entry
	constexpr uxlen_t STATUS_SPP = uxlen_t(1) << 8;   // H Extension fdw: shared S/VS previous privilege bit position during trap entry

	// free any potential LR/SC bus lock before processing a trap/interrupt
	release_lr_sc_reservation();

	auto pp = prv;
	auto prev_virt = virt;
	prv = target_mode;

	switch (target_mode) {
		case MachineMode: {
			csrs.mepc.reg.val = pc;

			// H Extension fdw: M-mode trap entry snapshots virtualization state for MRET.
			uint64_t full_mstatus = csrs.get_full_mstatus();
			full_mstatus &= ~csr::MSTATUS_GVA;
			if (prev_virt && pp != MachineMode)
				full_mstatus |= csr::MSTATUS_MPV;
			else
				full_mstatus &= ~csr::MSTATUS_MPV;
			if (prev_virt && !csrs.mcause.reg.fields.interrupt && csrs.mtval.reg.val)
				full_mstatus |= csr::MSTATUS_GVA;
			csrs.set_full_mstatus(full_mstatus);
			csrs.mtval2.reg.val = 0;
			csrs.mtinst.reg.val = 0;

			csrs.mstatus.reg.fields.mpie = csrs.mstatus.reg.fields.mie;
			csrs.mstatus.reg.fields.mie = 0;
			csrs.mstatus.reg.fields.mpp = pp;
			virt = false;

			pc = csrs.mtvec.get_base_address();

			if (unlikely(pc == 0)) {
				if (error_on_zero_traphandler) {
					throw std::runtime_error("[ISS] Took null trap handler in machine mode");
				} else {
					static bool once = true;
					if (once)
						std::cout
						    << "[ISS] Warn: Taking trap handler in machine mode to 0x0, this is probably an error."
						    << std::endl;
					once = false;
				}
			}

			if (csrs.mcause.reg.fields.interrupt && csrs.mtvec.reg.fields.mode == csr_mtvec::Mode::Vectored)
				pc += 4 * csrs.mcause.reg.fields.exception_code;
			break;
		}

		case SupervisorMode:
			if (trap_to_virtual_supervisor) {
				// H Extension fdw: VS trap entry updates VS-owned status bits and keeps V=1.
				csrs.vsepc.reg.val = pc;
				csrs.vsstatus.reg.val = (csrs.vsstatus.reg.val & ~STATUS_SPIE) |
				                        ((csrs.vsstatus.reg.val & STATUS_SIE) ? STATUS_SPIE : 0);
				csrs.vsstatus.reg.val &= ~STATUS_SIE;
				if (pp == SupervisorMode)
					csrs.vsstatus.reg.val |= STATUS_SPP;
				else
					csrs.vsstatus.reg.val &= ~STATUS_SPP;
				pc = csrs.vstvec.get_base_address();
				if (csrs.vscause.reg.fields.interrupt && csrs.vstvec.reg.fields.mode == csr_mtvec::Mode::Vectored)
					pc += 4 * csrs.vscause.reg.fields.exception_code;
				virt = true;
				trap_to_virtual_supervisor = false;
			} else {
				csrs.sepc.reg.val = pc;
				// H Extension fdw: HS trap entry snapshots whether the trap came from VS or VU.
				if (csrs.misa.has_hypervisor_extension()) {
					if (prev_virt) {
						csrs.hstatus.reg.val |= csr::HSTATUS_SPV;
						if (pp == SupervisorMode)
							csrs.hstatus.reg.val |= csr::HSTATUS_SPVP;
						else
							csrs.hstatus.reg.val &= ~csr::HSTATUS_SPVP;
					} else {
						csrs.hstatus.reg.val &= ~(csr::HSTATUS_SPV | csr::HSTATUS_SPVP);
					}
					if (prev_virt && !csrs.scause.reg.fields.interrupt && csrs.stval.reg.val)
						csrs.hstatus.reg.val |= csr::HSTATUS_GVA;
					else
						csrs.hstatus.reg.val &= ~csr::HSTATUS_GVA;
					csrs.htval.reg.val = 0;
					csrs.htinst.reg.val = 0;
				}
				csrs.mstatus.reg.fields.spie = csrs.mstatus.reg.fields.sie;
				csrs.mstatus.reg.fields.sie = 0;
				csrs.mstatus.reg.fields.spp = pp;
				pc = csrs.stvec.get_base_address();

				if (csrs.scause.reg.fields.interrupt && csrs.stvec.reg.fields.mode == csr_mtvec::Mode::Vectored)
					pc += 4 * csrs.scause.reg.fields.exception_code;
				virt = false;
			}
			break;

		case UserMode:
			assert(pp == UserMode);

			csrs.uepc.reg.val = pc;

			csrs.mstatus.reg.fields.upie = csrs.mstatus.reg.fields.uie;
			csrs.mstatus.reg.fields.uie = 0;

			pc = csrs.utvec.get_base_address();

			if (csrs.ucause.reg.fields.interrupt && csrs.utvec.reg.fields.mode == csr_mtvec::Mode::Vectored)
				pc += 4 * csrs.ucause.reg.fields.exception_code;
			break;

		default:
			throw std::runtime_error("unknown privilege level " + std::to_string(target_mode));
	}

	dbbcache.enter_trap(pc);
	force_slow_path();  // H Extension fdw: serialize trap-entry execution so handler CSR reads and pass/fail marker writes observe fresh architectural state immediately after trap redirection.
}

void ISS_CT::handle_interrupt() {
	auto x = compute_pending_interrupts();
	if (x.target_mode != NoneMode) {
		prepare_interrupt(x);
		switch_to_trap_handler(x.target_mode);
	}
}

void ISS_CT::handle_trap(SimulationTrap &e, uxlen_t last_pc) {
	auto target_mode = prepare_trap(e, last_pc);
	switch_to_trap_handler(target_mode);
}

void ISS_CT::run_step() {
	if (!debug_mode) {
		throw std::runtime_error("call of run_step with disabled debug-mode not permitted");
	}
	exec_steps(true);
}

void ISS_CT::run() {
	exec_steps(false);
}

void ISS_CT::show() {
	boost::io::ios_flags_saver ifs(std::cout);
	print_stats();
	std::cout << "=[ core : " << csrs.mhartid.reg.val << " ]===========================" << std::endl;
	std::cout << "simulation time: " << sc_core::sc_time_stamp() << std::endl;
	regs.show();
	std::cout << "pc = " << std::hex << pc << std::endl;
	std::cout << "num-instr = " << std::dec << csrs.instret.reg.val << std::endl;
	// Note: Like mcycles -> Does not contain any cycles that were executed while the CSR bit mcountinhibit.CY was set.
	std::cout << "num-cycles (mcycle) = " << _compute_and_get_current_cycles() << std::endl;
}
}  // namespace rv32
