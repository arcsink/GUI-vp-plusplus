#include <cstdlib>
#include <iostream>

#include <systemc>

#include "platform/common/bus.h"

using sc_core::SC_NS;
using sc_core::SC_ZERO_TIME;
using sc_core::sc_event;
using sc_core::sc_module;
using sc_core::sc_module_name;
using sc_core::sc_time;
using sc_core::sc_time_stamp;
using sc_core::sc_start;
using sc_core::sc_stop;
using sc_core::wait;

namespace {

struct LrScBusLockTestbench : public sc_module {
	SC_HAS_PROCESS(LrScBusLockTestbench);

	BusLock bus_lock;
	sc_event a_locked;
	sc_event b_waiting;
	bool failed = false;
	sc_time a_unlock_time = SC_ZERO_TIME;
	sc_time b_request_time = SC_ZERO_TIME;
	sc_time b_resume_time = SC_ZERO_TIME;

	static constexpr uint64_t test_addr = 0xefffe080ULL;

	explicit LrScBusLockTestbench(sc_module_name name) : sc_module(name) {
		SC_THREAD(hart_a_lr_sc_owner);
		SC_THREAD(hart_b_same_addr_access);
	}

	void expect(bool condition, const char *message) {
		if (!condition) {
			failed = true;
			std::cerr << "lrsc_bus_lock_test: FAIL: " << message << std::endl;
			sc_stop();
		}
	}

	void hart_a_lr_sc_owner() {
		wait(SC_ZERO_TIME);

		bus_lock.lock(0);
		const sc_time owner_check = sc_time_stamp();
		bus_lock.wait_for_access_rights(0);
		expect(sc_time_stamp() == owner_check,
		       "owning hart should not wait on its LR/SC reservation lock");

		a_locked.notify(SC_ZERO_TIME);
		wait(b_waiting);

		// Keep the simulated LR reservation long enough for hart B to block.
		wait(100, SC_NS);
		a_unlock_time = sc_time_stamp();
		bus_lock.unlock(0);
	}

	void hart_b_same_addr_access() {
		wait(a_locked);
		wait(10, SC_NS);

		b_request_time = sc_time_stamp();
		b_waiting.notify(SC_ZERO_TIME);

		// This models the wait_for_access_rights() call made before any load/store
		// to the same PMA/PBMT/ACE-visible address while another hart owns LR.
		bus_lock.wait_for_access_rights(1);
		b_resume_time = sc_time_stamp();

		expect(b_resume_time >= a_unlock_time,
		       "contending hart resumed before LR/SC owner released the lock");
		expect((b_resume_time - b_request_time) >= sc_time(100, SC_NS),
		       "contending hart did not wait for the owner's critical section");

		std::cout << "lrsc_bus_lock_test: PASS"
		          << " addr=0x" << std::hex << test_addr << std::dec
		          << " b_wait_ns=" << (b_resume_time - b_request_time).to_seconds() * 1e9
		          << std::endl;
		sc_stop();
	}
};

}  // namespace

int sc_main(int, char **) {
	LrScBusLockTestbench tb("tb");
	sc_start();
	return tb.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
