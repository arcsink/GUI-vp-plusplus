#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <memory>
#include <sstream>
#include <unordered_map>
#include <vector>

#define SC_INCLUDE_DYNAMIC_PROCESSES
#include "systemc.h"
using namespace sc_core;
using namespace sc_dt;
using namespace std;

#include "tlm.h"
#include "tlm-extensions/genattr.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "util/tlm_ext_pbmt.h"

#include "tlm-bridges/amba-ace.h"
using namespace AMBA::ACE;
#include "tlm-modules/cache-ace.h"
#include "tlm-modules/master-ace.h"
#include "tlm-modules/iconnect-ace.h"

namespace vp::extensions::ace_l2_memory {

inline unsigned ace_trace_budget_from_env() {
	const char *value = std::getenv("RVVP_ACE_TRACE_BUDGET");
	if (!value || !*value) {
		return 0;
	}

	char *end = nullptr;
	const unsigned long parsed = std::strtoul(value, &end, 10);
	if (end == value) {
		return 0;
	}

	return static_cast<unsigned>(parsed);
}

struct AceTraceFilter {
	bool enabled = false;
	uint64_t start = 0;
	uint64_t end = 0;

	bool matches(uint64_t addr) const {
		return !enabled || (addr >= start && addr <= end);
	}
};

inline AceTraceFilter ace_trace_filter_from_env() {
	AceTraceFilter filter;

	const char *start_value = std::getenv("RVVP_ACE_TRACE_START");
	const char *end_value = std::getenv("RVVP_ACE_TRACE_END");
	if (!start_value || !*start_value || !end_value || !*end_value) {
		return filter;
	}

	char *start_end = nullptr;
	char *end_end = nullptr;
	const unsigned long long parsed_start = std::strtoull(start_value, &start_end, 0);
	const unsigned long long parsed_end = std::strtoull(end_value, &end_end, 0);
	if (start_end == start_value || end_end == end_value) {
		return filter;
	}

	filter.enabled = true;
	filter.start = static_cast<uint64_t>(parsed_start);
	filter.end = static_cast<uint64_t>(parsed_end);
	if (filter.end < filter.start) {
		std::swap(filter.start, filter.end);
	}
	return filter;
}

class AceSimpleMemory : public sc_core::sc_module {
 public:
	tlm_utils::simple_target_socket<AceSimpleMemory> socket;

	AceSimpleMemory(sc_core::sc_module_name name, std::size_t size_bytes,
	                sc_core::sc_time latency = sc_core::sc_time(10, sc_core::SC_NS))
	    : sc_core::sc_module(name), socket("socket"), latency(latency), storage(size_bytes, 0) {
		socket.register_b_transport(this, &AceSimpleMemory::b_transport);
		socket.register_get_direct_mem_ptr(this, &AceSimpleMemory::get_direct_mem_ptr);
		socket.register_transport_dbg(this, &AceSimpleMemory::transport_dbg);
	}

	std::size_t size() const {
		return storage.size();
	}

 private:
	void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
		const uint64_t addr = trans.get_address();
		const auto len = static_cast<uint64_t>(trans.get_data_length());

		if ((addr + len) > storage.size()) {
			trans.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
			return;
		}

		auto *data = trans.get_data_ptr();
		if (trans.is_read()) {
			std::memcpy(data, storage.data() + addr, len);
		} else if (trans.is_write()) {
			std::memcpy(storage.data() + addr, data, len);
		} else {
			trans.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
			return;
		}

		delay += latency;
		trans.set_dmi_allowed(true);
		trans.set_response_status(tlm::TLM_OK_RESPONSE);
	}

	bool get_direct_mem_ptr(tlm::tlm_generic_payload &trans, tlm::tlm_dmi &dmi_data) {
		const uint64_t addr = trans.get_address();
		if (addr >= storage.size()) {
			return false;
		}

		dmi_data.set_start_address(0);
		dmi_data.set_end_address(storage.size() - 1);
		dmi_data.set_dmi_ptr(storage.data());
		dmi_data.set_read_latency(latency);
		dmi_data.set_write_latency(latency);
		dmi_data.allow_read_write();
		return true;
	}

	unsigned int transport_dbg(tlm::tlm_generic_payload &trans) {
		const uint64_t addr = trans.get_address();
		const auto requested = static_cast<uint64_t>(trans.get_data_length());
		if (addr >= storage.size()) {
			return 0;
		}

		const auto len = static_cast<unsigned int>(std::min<uint64_t>(requested, storage.size() - addr));
		auto *data = trans.get_data_ptr();
		if (trans.is_read()) {
			std::memcpy(data, storage.data() + addr, len);
		} else if (trans.is_write()) {
			std::memcpy(storage.data() + addr, data, len);
		}
		return len;
	}

	sc_core::sc_time latency;
	std::vector<unsigned char> storage;
};

class AceCacheSlaveAdapter : public sc_core::sc_module {
 public:
	tlm_utils::simple_target_socket<AceCacheSlaveAdapter> socket;
	tlm_utils::simple_initiator_socket<AceCacheSlaveAdapter> init_socket;

	AceCacheSlaveAdapter(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket"), init_socket("init_socket") {
		socket.register_b_transport(this, &AceCacheSlaveAdapter::b_transport);
	}

 private:
	void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
		init_socket->b_transport(trans, delay);
	}
};

class NullAceliteMaster : public sc_core::sc_module {
 public:
	tlm_utils::simple_initiator_socket<NullAceliteMaster> socket;

	NullAceliteMaster(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
};

template <int L1_CACHE_SIZE, int CACHELINE_SIZE>
class CpuAceMaster : public sc_core::sc_module {
 public:
	tlm_utils::simple_target_socket<CpuAceMaster> socket;
	tlm_utils::simple_initiator_socket<CpuAceMaster> cache_init_socket;
	typename ACEMaster<L1_CACHE_SIZE, CACHELINE_SIZE>::ACEPort_M ace_port_inst;
	cache_ace<L1_CACHE_SIZE, CACHELINE_SIZE> cache;

	CpuAceMaster(sc_core::sc_module_name name, uint64_t master_id)
	    : sc_core::sc_module(name),
	      socket("socket"),
	      cache_init_socket("cache_init_socket"),
	      ace_port_inst("ace_port_inst"),
	      cache("cache"),
	      master_id(master_id) {
		socket.register_b_transport(this, &CpuAceMaster::b_transport);
		cache_init_socket.bind(cache.target_socket);
		ace_port_inst.bind_upstream(cache);
	}

	auto &cpu_target_socket() {
		return socket;
	}

	auto &snoop_target_socket() {
		return ace_port_inst.snoop_target_socket;
	}

 private:
	void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
		genattr_extension *genattr = nullptr;
		tlm_ext_pbmt *pbmt_ext = nullptr;
		trans.get_extension(genattr);
		trans.get_extension(pbmt_ext);

		bool created_extension = false;
		if (!genattr) {
			genattr = new genattr_extension();
			trans.set_extension(genattr);
			created_extension = true;
		}

		// Provide the minimum metadata expected by the ACE cache stack.
		genattr->set_master_id(master_id);
		genattr->set_secure(false);
		genattr->set_bufferable(true);
		genattr->set_modifiable(true);
		genattr->set_read_allocate(true);
		genattr->set_write_allocate(true);
		genattr->set_domain(Domain::Inner);

		if (pbmt_ext && !pbmt_ext->ace_cacheable()) {
			genattr->set_bufferable(false);
			genattr->set_modifiable(false);
			genattr->set_read_allocate(false);
			genattr->set_write_allocate(false);
			genattr->set_domain(Domain::NonSharable);
		}

		cache_init_socket->b_transport(trans, delay);

		if (created_extension) {
			trans.clear_extension(genattr);
			delete genattr;
		}
	}

	uint64_t master_id;
};

class CoherentAceL2 : public sc_core::sc_module {
 public:
	enum class LineState {
		Invalid,
		Shared,
		UniqueClean,
		UniqueDirty,
	};

	struct DirectoryEntry {
		bool valid = false;
		uint64_t tag = 0;
		uint64_t sharers = 0;
		int owner = -1;
		bool dirty = false;
		LineState state = LineState::Invalid;
	};

	tlm_utils::simple_target_socket<CoherentAceL2> target_socket;
	tlm_utils::simple_target_socket<CoherentAceL2> snoop_target_socket;
	tlm_utils::simple_initiator_socket<CoherentAceL2> init_socket;

	CoherentAceL2(sc_core::sc_module_name name, std::size_t num_l1, std::size_t cache_size_bytes,
	              std::size_t cacheline_size,
	              sc_core::sc_time lookup_latency = sc_core::sc_time(5, sc_core::SC_NS))
	    : sc_core::sc_module(name),
	      target_socket("target_socket"),
	      snoop_target_socket("snoop_target_socket"),
	      init_socket("init_socket"),
	      num_l1(num_l1),
	      cacheline_size(cacheline_size),
	      max_lines(std::max<std::size_t>(1, cache_size_bytes / std::max<std::size_t>(1, cacheline_size))),
	      lookup_latency(lookup_latency),
	      trace_filter(ace_trace_filter_from_env()),
	      debug_print_budget(ace_trace_budget_from_env()),
	      debug_tx_count(0) {
		target_socket.register_b_transport(this, &CoherentAceL2::b_transport);
		snoop_target_socket.register_b_transport(this, &CoherentAceL2::b_transport_snoop);
	}

	template <typename SnoopSocketT>
	void bind_snoop_master(std::size_t port_id, SnoopSocketT &socket) {
		if (port_id >= num_l1) {
			throw std::out_of_range("L2 snoop master index out of range");
		}

		if (snoop_dispatchers.size() < num_l1) {
			snoop_dispatchers.resize(num_l1);
		}

		snoop_dispatchers[port_id] = [&socket](tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
			socket->b_transport(trans, delay);
		};
	}

	void create_nonshareable_region(uint64_t start, unsigned int len) {
		nonshareable_regions.push_back({start, len});
	}

	std::size_t get_num_l1() const {
		return num_l1;
	}

	bool has_directory_entry(uint64_t addr) const {
		return directory.find(line_addr(addr)) != directory.end();
	}

	DirectoryEntry directory_entry(uint64_t addr) const {
		const auto it = directory.find(line_addr(addr));
		if (it == directory.end()) {
			return {};
		}
		return it->second;
	}

 private:
	struct NonShareableRegion {
		uint64_t start;
		unsigned int len;

		bool contains(uint64_t addr) const {
			return addr >= start && addr < (start + len);
		}
	};

	void b_transport(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
		const uint64_t original_addr = trans.get_address();
		delay += lookup_latency;
		log_transaction(trans);
		if (is_cacheable(trans)) {
			apply_minimal_snoop_coherence(trans, delay);
		}
		update_directory_before_forward(trans);
		// The existing VP memory/peripheral targets commonly rely on the
		// initiator to pre-initialize a successful response code.
		trans.set_response_status(tlm::TLM_OK_RESPONSE);
		init_socket->b_transport(trans, delay);
		trans.set_address(original_addr);
		log_response(trans);
		if (trans.get_response_status() == tlm::TLM_OK_RESPONSE) {
			update_directory_after_forward(trans);
		}
	}

	void b_transport_snoop(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
		delay += lookup_latency;
		update_directory_for_incoming_snoop(trans);
		trans.set_response_status(tlm::TLM_OK_RESPONSE);
	}

	struct SnoopResult {
		bool ok = true;
		bool data_transfer = false;
		bool dirty = false;
		bool shared = false;
	};

	void apply_minimal_snoop_coherence(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay) {
		const auto it = directory.find(line_addr(trans.get_address()));
		if (it == directory.end() || !it->second.valid) {
			return;
		}

		const int requester = requester_id(trans);
		if (requester < 0) {
			return;
		}

		DirectoryEntry &entry = it->second;
		if (trans.is_read()) {
			handle_read_snoops(trans, delay, requester, entry);
		} else if (trans.is_write()) {
			handle_write_snoops(trans, delay, requester, entry);
		}
	}

	void handle_read_snoops(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay, int requester, DirectoryEntry &entry) {
		if (entry.owner < 0 || entry.owner == requester) {
			return;
		}

		const int previous_owner = entry.owner;
		SnoopResult snoop;
		if (!issue_snoop(previous_owner, line_addr(trans.get_address()), AC::ReadShared, delay, &snoop)) {
			trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
			return;
		}

		if (snoop.data_transfer && snoop.dirty) {
			write_back_snooped_line(line_addr(trans.get_address()), delay);
		}

		entry.owner = -1;
		entry.dirty = false;
		entry.state = LineState::Shared;
		entry.sharers |= (uint64_t{1} << previous_owner);
		entry.sharers |= (uint64_t{1} << requester);
	}

	void handle_write_snoops(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay, int requester, DirectoryEntry &entry) {
		uint64_t others = entry.sharers & ~(uint64_t{1} << requester);
		if (entry.owner >= 0 && entry.owner != requester) {
			others |= (uint64_t{1} << entry.owner);
		}

		while (others) {
			const int target = static_cast<int>(__builtin_ctzll(others));
			others &= (others - 1);

			SnoopResult snoop;
			if (!issue_snoop(target, line_addr(trans.get_address()), AC::CleanInvalid, delay, &snoop)) {
				trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
				return;
			}

			if (snoop.data_transfer && snoop.dirty) {
				write_back_snooped_line(line_addr(trans.get_address()), delay);
			}
		}

		entry.sharers = (uint64_t{1} << requester);
		entry.owner = requester;
		entry.dirty = true;
		entry.state = LineState::UniqueDirty;
	}

	bool issue_snoop(int target_id, uint64_t addr, uint8_t snoop_cmd, sc_core::sc_time &delay, SnoopResult *result) {
		if (target_id < 0 || static_cast<std::size_t>(target_id) >= num_l1) {
			return true;
		}
		if (static_cast<std::size_t>(target_id) >= snoop_dispatchers.size() || !snoop_dispatchers[target_id]) {
			return true;
		}

		std::vector<unsigned char> line(cacheline_size, 0);
		tlm::tlm_generic_payload snoop_gp;
		genattr_extension genattr;

		genattr.set_secure(false);
		genattr.set_domain(Domain::Inner);
		genattr.set_snoop(snoop_cmd);

		snoop_gp.set_command(tlm::TLM_READ_COMMAND);
		snoop_gp.set_address(addr);
		snoop_gp.set_data_ptr(line.data());
		snoop_gp.set_data_length(static_cast<unsigned int>(cacheline_size));
		snoop_gp.set_streaming_width(static_cast<unsigned int>(cacheline_size));
		snoop_gp.set_byte_enable_ptr(nullptr);
		snoop_gp.set_byte_enable_length(0);
		snoop_gp.set_dmi_allowed(false);
		snoop_gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
		snoop_gp.set_extension(&genattr);

		snoop_dispatchers[target_id](snoop_gp, delay);

		if (result) {
			result->ok = (snoop_gp.get_response_status() == tlm::TLM_OK_RESPONSE);
			result->data_transfer = genattr.get_datatransfer();
			result->dirty = genattr.get_dirty();
			result->shared = genattr.get_shared();
			if (result->data_transfer) {
				pending_snoop_writeback = std::move(line);
			}
		}

		snoop_gp.clear_extension(&genattr);
		return snoop_gp.get_response_status() == tlm::TLM_OK_RESPONSE;
	}

	void write_back_snooped_line(uint64_t addr, sc_core::sc_time &delay) {
		if (pending_snoop_writeback.empty()) {
			return;
		}

		tlm::tlm_generic_payload writeback_gp;
		genattr_extension writeback_attr;

		writeback_attr.set_secure(false);
		writeback_attr.set_domain(Domain::NonSharable);
		writeback_attr.set_snoop(AW::WriteNoSnoop);

		writeback_gp.set_command(tlm::TLM_WRITE_COMMAND);
		writeback_gp.set_address(addr);
		writeback_gp.set_data_ptr(pending_snoop_writeback.data());
		writeback_gp.set_data_length(static_cast<unsigned int>(pending_snoop_writeback.size()));
		writeback_gp.set_streaming_width(static_cast<unsigned int>(pending_snoop_writeback.size()));
		writeback_gp.set_byte_enable_ptr(nullptr);
		writeback_gp.set_byte_enable_length(0);
		writeback_gp.set_dmi_allowed(false);
		writeback_gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
		writeback_gp.set_extension(&writeback_attr);
		init_socket->b_transport(writeback_gp, delay);
		writeback_gp.clear_extension(&writeback_attr);
		pending_snoop_writeback.clear();
	}

	void update_directory_for_incoming_snoop(tlm::tlm_generic_payload &trans) {
		genattr_extension *genattr = nullptr;
		trans.get_extension(genattr);
		if (!genattr) {
			return;
		}

		auto it = directory.find(line_addr(trans.get_address()));
		if (it == directory.end()) {
			return;
		}

		DirectoryEntry &entry = it->second;
		switch (genattr->get_snoop()) {
		case AC::ReadShared:
		case AC::ReadClean:
			entry.owner = -1;
			entry.dirty = false;
			entry.state = LineState::Shared;
			break;
		case AC::CleanInvalid:
		case AC::MakeInvalid:
			entry = DirectoryEntry{};
			break;
		default:
			break;
		}
	}

	void update_directory_before_forward(tlm::tlm_generic_payload &trans) {
		if (!is_cacheable(trans)) {
			return;
		}

		touch_entry(trans.get_address());
		DirectoryEntry &entry = directory[line_addr(trans.get_address())];
		entry.valid = true;
		entry.tag = line_tag(trans.get_address());

		const int requester = requester_id(trans);
		if (requester < 0) {
			return;
		}

		if (trans.is_read()) {
			entry.sharers |= (uint64_t{1} << requester);
			entry.owner = entry.dirty ? entry.owner : -1;
			entry.state = entry.dirty ? LineState::UniqueDirty : LineState::Shared;
		} else if (trans.is_write()) {
			entry.sharers = (uint64_t{1} << requester);
			entry.owner = requester;
			entry.dirty = true;
			entry.state = LineState::UniqueDirty;
		}
	}

	void update_directory_after_forward(tlm::tlm_generic_payload &trans) {
		if (!is_cacheable(trans)) {
			return;
		}

		DirectoryEntry &entry = directory[line_addr(trans.get_address())];
		if (trans.is_read() && entry.owner < 0 && !entry.dirty) {
			entry.state = LineState::Shared;
		}
	}

	bool is_cacheable(tlm::tlm_generic_payload &trans) const {
		if (!(trans.is_read() || trans.is_write())) {
			return false;
		}

		tlm_ext_pbmt *pbmt_ext = nullptr;
		trans.get_extension(pbmt_ext);
		if (pbmt_ext && !pbmt_ext->ace_cacheable()) {
			return false;
		}

		for (const auto &region : nonshareable_regions) {
			if (region.contains(trans.get_address())) {
				return false;
			}
		}
		return true;
	}

	void touch_entry(uint64_t addr) {
		const uint64_t base = line_addr(addr);
		if (directory.find(base) != directory.end()) {
			return;
		}

		if (directory.size() >= max_lines && !line_order.empty()) {
			directory.erase(line_order.front());
			line_order.pop_front();
		}

		line_order.push_back(base);
		directory.emplace(base, DirectoryEntry{});
	}

	int requester_id(tlm::tlm_generic_payload &trans) const {
		genattr_extension *genattr = nullptr;
		trans.get_extension(genattr);
		if (!genattr) {
			return -1;
		}

		const uint64_t master_id = genattr->get_master_id();
		if (master_id < num_l1) {
			return static_cast<int>(master_id);
		}

		return -1;
	}

	void log_transaction(tlm::tlm_generic_payload &trans) {
		if (debug_tx_count >= debug_print_budget) {
			return;
		}
		if (!trace_filter.matches(trans.get_address())) {
			return;
		}

		std::cout << "[CoherentL2 " << name() << "] "
		          << (trans.is_read() ? "READ" : (trans.is_write() ? "WRITE" : "OTHER"))
		          << " addr=0x" << std::hex << trans.get_address() << std::dec
		          << " len=" << trans.get_data_length();

		const int requester = requester_id(trans);
		if (requester >= 0) {
			std::cout << " requester=L1-" << requester;
		}

		std::cout << std::endl;
		debug_tx_count++;
	}

	void log_response(tlm::tlm_generic_payload &trans) {
		if (debug_resp_count >= debug_print_budget) {
			return;
		}

		const uint64_t addr = trans.get_address();
		if (!trace_filter.matches(addr)) {
			return;
		}
		std::cout << "[CoherentL2 " << name() << "] response"
		          << " addr=0x" << std::hex << addr << std::dec
		          << " status=" << trans.get_response_string()
		          << std::endl;
		debug_resp_count++;
	}

	uint64_t line_addr(uint64_t addr) const {
		return addr & ~(static_cast<uint64_t>(cacheline_size) - 1);
	}

	uint64_t line_tag(uint64_t addr) const {
		return line_addr(addr) / cacheline_size;
	}

	const std::size_t num_l1;
	const std::size_t cacheline_size;
	const std::size_t max_lines;
	const sc_core::sc_time lookup_latency;
	const AceTraceFilter trace_filter;
	const unsigned debug_print_budget;
	unsigned debug_tx_count;
	unsigned debug_resp_count = 0;
	std::unordered_map<uint64_t, DirectoryEntry> directory;
	std::deque<uint64_t> line_order;
	std::vector<NonShareableRegion> nonshareable_regions;
	std::vector<std::function<void(tlm::tlm_generic_payload &, sc_core::sc_time &)>> snoop_dispatchers;
	std::vector<unsigned char> pending_snoop_writeback;
};

template <int NUM_ACE_MASTERS, int NUM_ACELITE_MASTERS = 0, int L2_CACHE_SIZE = 256 * 1024, int CACHELINE_SIZE = 64>
class SharedAceL2MemorySubsystem : public sc_core::sc_module {
 public:
	static constexpr int kPhysicalACELiteMasters = (NUM_ACELITE_MASTERS > 0) ? NUM_ACELITE_MASTERS : 1;

	using interconnect_t = iconnect_ace<NUM_ACE_MASTERS, kPhysicalACELiteMasters, CACHELINE_SIZE>;
	using l2_cache_t = CoherentAceL2;
	using ace_master_t = CpuAceMaster<32 * 1024, CACHELINE_SIZE>;

	interconnect_t interconnect;
	AceCacheSlaveAdapter l2_port;
	l2_cache_t l2_cache;
	tlm_utils::simple_initiator_socket<SharedAceL2MemorySubsystem> l2_snoop_stub;
	NullAceliteMaster acelite_stub;
	std::unique_ptr<AceSimpleMemory> memory;

	SharedAceL2MemorySubsystem(sc_core::sc_module_name name, std::size_t memory_size_bytes,
	                           bool bind_internal_memory = true,
	                           sc_core::sc_time memory_latency = sc_core::sc_time(50, sc_core::SC_NS))
	    : sc_core::sc_module(name),
	      interconnect("interconnect"),
	      l2_port("l2_port"),
	      l2_cache("l2_cache", NUM_ACE_MASTERS, L2_CACHE_SIZE, CACHELINE_SIZE),
	      l2_snoop_stub("l2_snoop_stub"),
	      acelite_stub("acelite_stub"),
	      memory(bind_internal_memory ? std::make_unique<AceSimpleMemory>("memory", memory_size_bytes, memory_latency)
	                                 : nullptr) {
		l2_port.init_socket.bind(l2_cache.target_socket);
		l2_snoop_stub.bind(l2_cache.snoop_target_socket);
		if constexpr (NUM_ACELITE_MASTERS == 0) {
			interconnect.s_acelite_port[0]->connect_master(acelite_stub);
		}
		if (memory) {
			l2_cache.init_socket.bind(memory->socket);
		}
		interconnect.ds_port.connect_slave(l2_port);
	}

	template <typename BridgeT>
	void connect_ace_master(int port_id, BridgeT &bridge) {
		check_ace_port(port_id);
		interconnect.s_ace_port[port_id]->connect_master(bridge);
	}

	template <typename BridgeT>
	void connect_acelite_master(int port_id, BridgeT &bridge) {
		check_acelite_port(port_id);
		interconnect.s_acelite_port[port_id]->connect_master(bridge);
	}

	template <typename MasterT>
	void connect_master_ace(int port_id, MasterT &master) {
		check_ace_port(port_id);
		master.ace_port().init_socket.bind(interconnect.s_ace_port[port_id]->target_socket);
		interconnect.s_ace_port[port_id]->snoop_init_socket.bind(master.ace_port().snoop_target_socket);
		l2_cache.bind_snoop_master(port_id, interconnect.s_ace_port[port_id]->snoop_init_socket);
	}

	template <typename TargetSocketT>
	void bind_downstream(TargetSocketT &target_socket) {
		l2_cache.init_socket.bind(target_socket);
	}

	void set_forward_dvm(int port_id, bool enable) {
		check_ace_port(port_id);
		interconnect.s_ace_port[port_id]->SetForwardDVM(enable);
	}

	void create_nonshareable_region(uint64_t start, unsigned int len) {
		l2_cache.create_nonshareable_region(start, len);
	}

 private:
	static void check_ace_port(int port_id) {
		if (port_id < 0 || port_id >= NUM_ACE_MASTERS) {
			throw std::out_of_range("ACE master port index out of range");
		}
	}

	static void check_acelite_port(int port_id) {
		if (port_id < 0 || port_id >= NUM_ACELITE_MASTERS) {
			throw std::out_of_range("ACE-Lite master port index out of range");
		}
	}
};

}  // namespace vp::extensions::ace_l2_memory
