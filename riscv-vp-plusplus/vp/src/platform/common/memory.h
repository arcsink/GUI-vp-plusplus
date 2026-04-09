#ifndef RISCV_ISA_MEMORY_H
#define RISCV_ISA_MEMORY_H

#include <stdint.h>
#include <tlm_utils/simple_target_socket.h>

#include <cstring>
#include <systemc>

#include "core/common/load_if.h"
#include "platform/common/bus.h"
#include "util/propertytree.h"

struct SimpleMemory : public sc_core::sc_module, public load_if {
	/* config properties */
	sc_core::sc_time prop_clock_cycle_period = sc_core::sc_time(10, sc_core::SC_NS);
	unsigned int prop_access_clock_cycles = 1;

	sc_core::sc_time access_delay;

	tlm_utils::simple_target_socket<SimpleMemory> tsock;

	uint8_t *data;
	uint64_t size;
	bool read_only;
	bool stop_on_tohost_write = false;
	bool tohost_stop_requested = false;
	uint64_t tohost_addr = 0;

	SimpleMemory(sc_core::sc_module_name, uint64_t size, bool read_only = false)
	    : data(new uint8_t[size]()), size(size), read_only(read_only) {
		/* get config properties from global property tree (or use default) */
		VPPP_PROPERTY_GET("SimpleMemory." + name(), "clock_cycle_period", sc_core::sc_time, prop_clock_cycle_period);
		VPPP_PROPERTY_GET("SimpleMemory." + name(), "access_clock_cycles", uint64_t, prop_access_clock_cycles);

		access_delay = prop_access_clock_cycles * prop_clock_cycle_period;

		tsock.register_b_transport(this, &SimpleMemory::transport);
		tsock.register_get_direct_mem_ptr(this, &SimpleMemory::get_direct_mem_ptr);
		tsock.register_transport_dbg(this, &SimpleMemory::transport_dbg);
	}

	~SimpleMemory(void) {
		delete[] data;
	}

	uint64_t get_size() override {
		return size;
	}

	void load_data(const char *src, uint64_t dst_addr, size_t n) override {
		assert(dst_addr + n <= size);
		memcpy(&data[dst_addr], src, n);
	}

	void load_zero(uint64_t dst_addr, size_t n) override {
		assert(dst_addr + n <= size);
		memset(&data[dst_addr], 0, n);
	}

	void write_data(uint64_t addr, const uint8_t *src, unsigned num_bytes) {
		assert(addr + num_bytes <= size);

		memcpy(data + addr, src, num_bytes);
		handle_tohost_write(addr, num_bytes);
	}

	void read_data(uint64_t addr, uint8_t *dst, unsigned num_bytes) {
		assert(addr + num_bytes <= size);

		memcpy(dst, data + addr, num_bytes);
	}

	void transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
		transport_dbg(trans);
		delay += access_delay;
	}

	unsigned transport_dbg(tlm::tlm_generic_payload &trans) {
		tlm::tlm_command cmd = trans.get_command();
		uint64_t addr = trans.get_address();
		auto *ptr = trans.get_data_ptr();
		auto len = trans.get_data_length();

		assert(addr < size);

		if (cmd == tlm::TLM_WRITE_COMMAND) {
			write_data(addr, ptr, len);
		} else if (cmd == tlm::TLM_READ_COMMAND) {
			read_data(addr, ptr, len);
		} else {
			sc_assert(false && "unsupported tlm command");
		}

		return len;
	}

	void set_tohost_watch_address(uint64_t addr) {
		stop_on_tohost_write = true;
		tohost_stop_requested = false;
		tohost_addr = addr;
	}

	bool get_direct_mem_ptr(tlm::tlm_generic_payload &trans, tlm::tlm_dmi &dmi) {
		(void)trans;
		dmi.set_start_address(0);
		dmi.set_end_address(size);
		dmi.set_dmi_ptr(data);
		if (read_only)
			dmi.allow_read();
		else
			dmi.allow_read_write();
		return true;
	}

   private:
	static bool overlaps_range(uint64_t addr, unsigned num_bytes, uint64_t start, uint64_t bytes) {
		const uint64_t end = addr + num_bytes;
		const uint64_t range_end = start + bytes;
		return addr < range_end && start < end;
	}

	uint64_t load_le_u64(uint64_t addr) const {
		uint64_t value = 0;
		for (unsigned i = 0; i < sizeof(value); ++i) {
			value |= static_cast<uint64_t>(data[addr + i]) << (8 * i);
		}
		return value;
	}

	void handle_tohost_write(uint64_t addr, unsigned num_bytes) {
		constexpr uint64_t HTIF_CMD_SHIFT = 48;
		constexpr uint64_t HTIF_DEV_SHIFT = 56;
		constexpr uint64_t HTIF_CMD_MASK = 0xffULL;
		constexpr uint64_t HTIF_DEV_MASK = 0xffULL;
		constexpr uint64_t HTIF_DEV_SYSTEM = 0;

		// H Extension fdw: Upstream riscv-hext-asm-tests finish some xtvec-overwrite
		// cases by writing a non-zero completion code to `tohost` and then executing
		// `unimp`. Stop the tiny platform only for HTIF system-device completion writes
		// so console GETC/PUTC requests do not get mistaken for terminal pass/fail codes.
		if (!stop_on_tohost_write || tohost_stop_requested || tohost_addr + sizeof(uint64_t) > size) {
			return;
		}
		if (!overlaps_range(addr, num_bytes, tohost_addr, sizeof(uint64_t))) {
			return;
		}

		uint64_t value = load_le_u64(tohost_addr);
		if (value == 0) {
			return;
		}
		const uint64_t htif_dev = (value >> HTIF_DEV_SHIFT) & HTIF_DEV_MASK;
		const uint64_t htif_cmd = (value >> HTIF_CMD_SHIFT) & HTIF_CMD_MASK;
		(void)htif_cmd;
		if (htif_dev != HTIF_DEV_SYSTEM) {
			return;
		}

		tohost_stop_requested = true;
		std::cout << "SimpleMemory: observed tohost completion 0x" << std::hex << value << std::dec << std::endl;
		sc_core::sc_stop();
	}
};

#endif  // RISCV_ISA_MEMORY_H
