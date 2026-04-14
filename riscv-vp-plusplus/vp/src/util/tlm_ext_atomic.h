#pragma once

#include <cstdint>
#include <tlm>

#include "core/common/pma.h"

enum class TlmAtomicPhase : uint8_t {
	None,
	Load,
	Store,
};

enum class TlmAmoOp : uint8_t {
	None,
	Swap,
	Add,
	Xor,
	And,
	Or,
	MinSigned,
	MaxSigned,
	MinUnsigned,
	MaxUnsigned,
};

class tlm_ext_atomic : public tlm::tlm_extension<tlm_ext_atomic> {
   public:
	bool is_amo = false;
	bool is_lr = false;
	bool is_sc = false;
	bool aq = false;
	bool rl = false;
	TlmAtomicPhase phase = TlmAtomicPhase::None;
	TlmAmoOp amo_op = TlmAmoOp::None;
	PmaAmoClass amo_class = PmaAmoClass::AMOArithmetic;

	void clear() {
		is_amo = false;
		is_lr = false;
		is_sc = false;
		aq = false;
		rl = false;
		phase = TlmAtomicPhase::None;
		amo_op = TlmAmoOp::None;
		amo_class = PmaAmoClass::AMOArithmetic;
	}

	void set_amo(TlmAtomicPhase new_phase, TlmAmoOp new_op, PmaAmoClass new_class, bool new_aq, bool new_rl) {
		is_amo = true;
		is_lr = false;
		is_sc = false;
		aq = new_aq;
		rl = new_rl;
		phase = new_phase;
		amo_op = new_op;
		amo_class = new_class;
	}

	tlm::tlm_extension_base *clone() const override {
		return new tlm_ext_atomic(*this);
	}

	void copy_from(tlm::tlm_extension_base const &ext) override {
		const auto &other = static_cast<tlm_ext_atomic const &>(ext);
		is_amo = other.is_amo;
		is_lr = other.is_lr;
		is_sc = other.is_sc;
		aq = other.aq;
		rl = other.rl;
		phase = other.phase;
		amo_op = other.amo_op;
		amo_class = other.amo_class;
	}
};
