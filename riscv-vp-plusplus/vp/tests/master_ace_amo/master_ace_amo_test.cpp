#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

using namespace sc_core;
using namespace sc_dt;

#include "tlm-bridges/amba-ace.h"
#include "tlm-extensions/genattr.h"
#include "tlm-modules/cache-ace.h"
#include "util/tlm_ext_atomic.h"
#include "util/tlm_ext_pbmt.h"

using namespace AMBA::ACE;

namespace {

struct RecordedTxn {
	tlm::tlm_command command;
	uint64_t address;
	uint8_t snoop;
};

class RecordingMemory : public sc_module {
   public:
	tlm_utils::simple_target_socket<RecordingMemory> socket;
	std::vector<RecordedTxn> records;

	SC_HAS_PROCESS(RecordingMemory);

	explicit RecordingMemory(sc_module_name name)
	    : sc_module(name), socket("socket") {
		socket.register_b_transport(this, &RecordingMemory::b_transport);
	}

	void clear() {
		records.clear();
	}

	bool saw(uint8_t snoop) const {
		for (const auto &record : records) {
			if (record.snoop == snoop) {
				return true;
			}
		}
		return false;
	}

	void b_transport(tlm::tlm_generic_payload &trans, sc_time &delay) {
		wait(delay);
		delay = SC_ZERO_TIME;

		genattr_extension *genattr = nullptr;
		trans.get_extension(genattr);
		const uint8_t snoop = genattr ? genattr->get_snoop() : 0;

		records.push_back({trans.get_command(), trans.get_address(), snoop});

		if (trans.is_read()) {
			auto *data = trans.get_data_ptr();
			for (unsigned i = 0; i < trans.get_data_length(); ++i) {
				data[i] = static_cast<unsigned char>((trans.get_address() + i) & 0xff);
			}
		}

		if (genattr) {
			genattr->set_shared(snoop == AR::ReadShared);
			genattr->set_dirty(false);
			genattr->set_was_unique(false);
		}

		trans.set_response_status(tlm::TLM_OK_RESPONSE);
	}
};

class AceAmoTestbench : public sc_module {
   public:
	tlm_utils::simple_initiator_socket<AceAmoTestbench> cpu_socket;
	tlm_utils::simple_initiator_socket<AceAmoTestbench> snoop_socket;
	cache_ace<1024, 64> cache;
	RecordingMemory memory;
	bool failed = false;

	SC_HAS_PROCESS(AceAmoTestbench);

	explicit AceAmoTestbench(sc_module_name name)
	    : sc_module(name),
	      cpu_socket("cpu_socket"),
	      snoop_socket("snoop_socket"),
	      cache("cache"),
	      memory("memory") {
		cpu_socket.bind(cache.target_socket);
		snoop_socket.bind(cache.snoop_target_socket);
		cache.init_socket.bind(memory.socket);
		SC_THREAD(run);
	}

	void fail(const char *msg) {
		std::cerr << "master_ace_amo_test: FAIL: " << msg << std::endl;
		failed = true;
		sc_stop();
	}

	void expect(bool condition, const char *msg) {
		if (!condition) {
			fail(msg);
		}
	}

	void setup_cacheable_attr(genattr_extension &genattr, tlm_ext_pbmt &pbmt) {
		genattr.set_master_id(0);
		genattr.set_secure(false);
		genattr.set_bufferable(true);
		genattr.set_modifiable(true);
		genattr.set_read_allocate(true);
		genattr.set_write_allocate(true);
		genattr.set_domain(Domain::Inner);
		pbmt.set_pma(tlm_ext_pbmt::PMA_MEMORY_MAIN,
		             tlm_ext_pbmt::PMA_ORDER_RVWMO,
		             true,
		             true);
	}

	void send_read(uint64_t addr, bool amo, bool exclusive, uint8_t *data, unsigned len) {
		tlm::tlm_generic_payload gp;
		genattr_extension genattr;
		tlm_ext_pbmt pbmt;
		tlm_ext_atomic atomic;
		sc_time delay = SC_ZERO_TIME;

		setup_cacheable_attr(genattr, pbmt);
		genattr.set_exclusive(exclusive);

		if (amo) {
			// AMO load is the first half of the read-modify-write sequence. The
			// ACE cache must use it to acquire unique line ownership.
			atomic.set_amo(TlmAtomicPhase::Load,
			               TlmAmoOp::Or,
			               PmaAmoClass::AMOLogical,
			               true,
			               true);
			gp.set_extension(&atomic);
		}

		gp.set_command(tlm::TLM_READ_COMMAND);
		gp.set_address(addr);
		gp.set_data_ptr(data);
		gp.set_data_length(len);
		gp.set_streaming_width(len);
		gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
		gp.set_extension(&genattr);
		gp.set_extension(&pbmt);

		cpu_socket->b_transport(gp, delay);
		gp.clear_extension(&genattr);
		gp.clear_extension(&pbmt);
		if (amo) {
			gp.clear_extension(&atomic);
		}

		expect(gp.get_response_status() == tlm::TLM_OK_RESPONSE,
		       "read transaction did not complete with TLM_OK_RESPONSE");
	}

	void run() {
		std::array<uint8_t, 8> data{};

		memory.clear();
		send_read(0x100, true, false, data.data(), data.size());
		expect(memory.saw(AR::ReadUnique),
		       "AMO load miss did not issue AR::ReadUnique");

		memory.clear();
		send_read(0x200, false, true, data.data(), data.size());
		expect(memory.saw(AR::ReadShared),
		       "setup read did not create a shared cache line");

		memory.clear();
		send_read(0x200, true, false, data.data(), data.size());
		expect(memory.saw(AR::CleanUnique),
		       "AMO load shared hit did not issue AR::CleanUnique");
		expect(!memory.saw(AR::ReadUnique),
		       "AMO load shared hit should upgrade with CleanUnique, not refetch with ReadUnique");

		std::cout << "master_ace_amo_test: PASS" << std::endl;
		sc_stop();
	}
};

}  // namespace

int sc_main(int, char **) {
	AceAmoTestbench tb("tb");
	sc_start();
	return tb.failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
