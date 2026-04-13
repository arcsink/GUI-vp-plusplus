#pragma once

#include <cstdint>
#include <tlm>

class tlm_ext_pbmt : public tlm::tlm_extension<tlm_ext_pbmt> {
   public:
	enum : uint8_t {
		PMA = 0,
		NC = 1,
		IO = 2,
		RESERVED = 3,
	};

	enum : uint8_t {
		PMA_MEMORY_MAIN = 0,
		PMA_MEMORY_IO = 1,
	};

	enum : uint8_t {
		PMA_ORDER_RVWMO = 0,
		PMA_ORDER_RVTSO = 1,
		PMA_ORDER_RELAXED_IO = 2,
		PMA_ORDER_STRONG_IO = 3,
	};

	uint8_t pbmt;
	uint8_t pma_memory_type;
	uint8_t pma_ordering;
	bool pma_cacheable;
	bool pma_coherent;

	explicit tlm_ext_pbmt(uint8_t pbmt = PMA)
	    : pbmt(pbmt),
	      pma_memory_type(PMA_MEMORY_MAIN),
	      pma_ordering(PMA_ORDER_RVWMO),
	      pma_cacheable(true),
	      pma_coherent(true) {}

	void set_pma(uint8_t memory_type, uint8_t ordering, bool cacheable, bool coherent) {
		pma_memory_type = memory_type;
		pma_ordering = ordering;
		pma_cacheable = cacheable;
		pma_coherent = coherent;
	}

	bool pte_forces_uncached() const {
		return pbmt == NC || pbmt == IO || pbmt == RESERVED;
	}

	bool pma_allows_ace_cache() const {
		return pma_memory_type == PMA_MEMORY_MAIN && pma_cacheable && pma_coherent;
	}

	bool derived_cacheable() const {
		return !pte_forces_uncached() && pma_cacheable;
	}

	bool is_coherent_addr() const {
		return derived_cacheable() &&
		       pma_memory_type == PMA_MEMORY_MAIN &&
		       pma_coherent;
	}

	bool ace_cacheable() const {
		return is_coherent_addr();
	}

	tlm::tlm_extension_base *clone() const override {
		return new tlm_ext_pbmt(*this);
	}

	void copy_from(tlm::tlm_extension_base const &ext) override {
		const auto &other = static_cast<tlm_ext_pbmt const &>(ext);
		pbmt = other.pbmt;
		pma_memory_type = other.pma_memory_type;
		pma_ordering = other.pma_ordering;
		pma_cacheable = other.pma_cacheable;
		pma_coherent = other.pma_coherent;
	}
};
