/*
 * Copyright (c) 2019 Xilinx Inc.
 * Written by Francisco Iglesias.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef __MASTER_ACE_H__
#define __MASTER_ACE_H__

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include "systemc"
using namespace sc_core;
using namespace sc_dt;
using namespace std;

#include <cstdlib>
#include <iostream>

#include "traffic-generators/tg-tlm.h"
#include "tlm-modules/cache-ace.h"
#include "tlm-modules/bp-ace.h"
#include "util/tlm_ext_atomic.h"
#include "util/tlm_ext_pbmt.h"

template<int SZ_CACHE, int SZ_CACHELINE>
class ACEMaster :
	public sc_core::sc_module
{
public:
	struct AceTraceFilter
	{
		bool enabled = false;
		uint64_t start = 0;
		uint64_t end = 0;

		bool matches(uint64_t addr) const
		{
			return !enabled || (addr >= start && addr <= end);
		}
	};

	static const char *pbmt_to_string(uint8_t pbmt)
	{
		switch (pbmt) {
		case tlm_ext_pbmt::PMA:
			return "PMA";
		case tlm_ext_pbmt::NC:
			return "NC";
		case tlm_ext_pbmt::IO:
			return "IO";
		case tlm_ext_pbmt::RESERVED:
			return "RESERVED";
		default:
			return "UNKNOWN";
		}
	}

	static const char *pma_memory_type_to_string(uint8_t memory_type)
	{
		switch (memory_type) {
		case tlm_ext_pbmt::PMA_MEMORY_MAIN:
			return "main";
		case tlm_ext_pbmt::PMA_MEMORY_IO:
			return "io";
		default:
			return "unknown";
		}
	}

	static const char *amo_op_to_string(TlmAmoOp op)
	{
		switch (op) {
		case TlmAmoOp::Swap:
			return "swap";
		case TlmAmoOp::Add:
			return "add";
		case TlmAmoOp::Xor:
			return "xor";
		case TlmAmoOp::And:
			return "and";
		case TlmAmoOp::Or:
			return "or";
		case TlmAmoOp::MinSigned:
			return "mins";
		case TlmAmoOp::MaxSigned:
			return "maxs";
		case TlmAmoOp::MinUnsigned:
			return "minu";
		case TlmAmoOp::MaxUnsigned:
			return "maxu";
		default:
			return "none";
		}
	}

	static unsigned ace_debug_budget_from_env()
	{
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

	static AceTraceFilter ace_trace_filter_from_env()
	{
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

	tlm_utils::simple_target_socket<ACEMaster> cpu_socket;
	tlm_utils::simple_initiator_socket<ACEMaster> cpu_init_socket;
	tlm_utils::simple_target_socket<ACEMaster> unused_gen_target_socket;

	class ACEPort_M :
		public sc_core::sc_module
	{
	public:
		tlm_utils::simple_initiator_socket<ACEPort_M> init_socket;
		tlm_utils::simple_target_socket<ACEPort_M> snoop_target_socket;

		ACEPort_M(sc_core::sc_module_name name) :
			sc_module(name),
			init_socket("init_socket"),
			snoop_target_socket("snoop_target_socket"),
			m_target_socket("m_target_socket"),
			m_snoop_init_socket("m_snoop_init_socket"),
			m_trace_filter(ACEMaster::ace_trace_filter_from_env()),
			m_debug_print_budget(ACEMaster::ace_debug_budget_from_env()),
			m_debug_tx_count(0)
		{
			// Receive snoop transactions
			snoop_target_socket.register_b_transport(this,
						&ACEPort_M::b_transport_snoop);

			// Cache interface, receive ace transactions
			m_target_socket.register_b_transport(this,
							&ACEPort_M::b_transport_ace);

		}

		template<typename T>
		void bind_upstream(T& dev)
		{
			dev.init_socket.bind(m_target_socket);
			m_snoop_init_socket.bind(dev.snoop_target_socket);
		}

	private:
		// Receive and forward ace transactions (downstream from cache)
		virtual void b_transport_ace(tlm::tlm_generic_payload& trans,
						sc_time& delay)
		{
			if (m_debug_tx_count < m_debug_print_budget &&
			    m_trace_filter.matches(trans.get_address())) {
				genattr_extension *genattr = nullptr;
				trans.get_extension(genattr);
				std::cout << "[MasterACE " << name() << "] "
				          << (trans.is_read() ? "READ" : (trans.is_write() ? "WRITE" : "OTHER"))
				          << " addr=0x" << std::hex << trans.get_address() << std::dec
				          << " len=" << trans.get_data_length();
				if (genattr) {
					std::cout << " snoop=0x" << std::hex
					          << static_cast<unsigned>(genattr->get_snoop()) << std::dec
					          << " domain=" << static_cast<unsigned>(genattr->get_domain())
					          << " barrier=" << genattr->get_barrier();
				}
				std::cout << std::endl;
				m_debug_tx_count++;
			}
			init_socket->b_transport(trans, delay);
		}

		// Receive and forward snoop transactions (upstream to cache)
		virtual void b_transport_snoop(tlm::tlm_generic_payload& trans,
						sc_time& delay)
		{
			m_snoop_init_socket->b_transport(trans, delay);
		}

		// Upstream interface (to the cache)
		tlm_utils::simple_target_socket<ACEPort_M> m_target_socket;
		tlm_utils::simple_initiator_socket<ACEPort_M> m_snoop_init_socket;
		AceTraceFilter m_trace_filter;
		unsigned m_debug_print_budget;
		unsigned m_debug_tx_count;
	};

	ACEPort_M m_ace_port;

	ACEMaster(sc_core::sc_module_name name,
				WritePolicy write_policy = WriteBack,
				uint64_t master_id = 0) :
		sc_module(name),
		cpu_socket("cpu_socket"),
		cpu_init_socket("cpu_init_socket"),
		unused_gen_target_socket("unused_gen_target_socket"),
		m_ace_port("m_ace_port"),
		m_gen("gen", 1),
		m_barrier_processer("barrier_processer"),
		m_cache("ace_cache", write_policy),
		m_trace_filter(ace_trace_filter_from_env()),
		m_debug_print_budget(ace_debug_budget_from_env()),
		m_debug_cpu_tx_count(0),
		m_master_id(master_id),
		m_use_internal_generator(false)
	{
		cpu_socket.register_b_transport(this, &ACEMaster::b_transport_cpu);
		unused_gen_target_socket.register_b_transport(this, &ACEMaster::b_transport_unused_gen);
		ConnectSockets();
	}

	template<typename T>
	ACEMaster(sc_core::sc_module_name name, T& transfers,
				WritePolicy write_policy = WriteBack,
				uint64_t master_id = 0) :
		sc_module(name),
		cpu_socket("cpu_socket"),
		cpu_init_socket("cpu_init_socket"),
		unused_gen_target_socket("unused_gen_target_socket"),
		m_ace_port("m_ace_port"),
		m_gen("gen", 1),
		m_barrier_processer("barrier_processer"),
		m_cache("ace_cache", write_policy),
		m_trace_filter(ace_trace_filter_from_env()),
		m_debug_print_budget(ace_debug_budget_from_env()),
		m_debug_cpu_tx_count(0),
		m_master_id(master_id),
		m_use_internal_generator(true)
	{
		// Configure generator
		m_gen.addTransfers(transfers, 0);

		cpu_socket.register_b_transport(this, &ACEMaster::b_transport_cpu);
		unused_gen_target_socket.register_b_transport(this, &ACEMaster::b_transport_unused_gen);
		ConnectSockets();
	}

	template<typename T>
	void connect(T& bridge)
	{
		m_ace_port.init_socket.bind(bridge.tgt_socket);
		bridge.snoop_init_socket.bind(m_ace_port.snoop_target_socket);
	}

	void enableDebug() { m_gen.enableDebug(); }

	auto& cpu_target_socket() { return cpu_socket; }
	auto& ace_port() { return m_ace_port; }

	void CreateNonShareableRegion(uint64_t start, unsigned int len)
	{
		m_cache.create_nonshareable_region(start, len);
	}

	TLMTrafficGenerator& GetTrafficGenerator() { return m_gen; }
private:
	void b_transport_cpu(tlm::tlm_generic_payload& trans, sc_time& delay)
	{
		genattr_extension *genattr = nullptr;
		tlm_ext_pbmt *pbmt_ext = nullptr;
		tlm_ext_atomic *atomic_ext = nullptr;
		trans.get_extension(genattr);
		trans.get_extension(pbmt_ext);
		trans.get_extension(atomic_ext);
		const tlm_ext_pbmt default_pbmt;
		const tlm_ext_pbmt& addr_attr = pbmt_ext ? *pbmt_ext : default_pbmt;
		const bool ace_cacheable = addr_attr.ace_cacheable();

		bool created_extension = false;
		if (!genattr) {
			genattr = new genattr_extension();
			trans.set_extension(genattr);
			created_extension = true;
		}

		genattr->set_master_id(m_master_id);
		genattr->set_secure(false);
		genattr->set_bufferable(true);
		genattr->set_modifiable(true);
		genattr->set_read_allocate(true);
		genattr->set_write_allocate(true);
		genattr->set_domain(Domain::Inner);

		if (!ace_cacheable) {
			genattr->set_bufferable(false);
			genattr->set_modifiable(false);
			genattr->set_read_allocate(false);
			genattr->set_write_allocate(false);
			genattr->set_domain(Domain::NonSharable);
		}

		if (m_debug_cpu_tx_count < m_debug_print_budget &&
		    m_trace_filter.matches(trans.get_address())) {
			std::cout << "[MasterACE " << name() << " cpu-entry] "
			          << (trans.is_read() ? "READ" : (trans.is_write() ? "WRITE" : "OTHER"))
			          << " addr=0x" << std::hex << trans.get_address() << std::dec
			          << " len=" << trans.get_data_length()
			          << " pbmt=" << pbmt_to_string(addr_attr.pbmt)
			          << " pma_mem=" << pma_memory_type_to_string(addr_attr.pma_memory_type)
			          << " pma_cacheable=" << addr_attr.pma_cacheable
			          << " pma_coherent=" << addr_attr.pma_coherent
			          << " cacheable=" << addr_attr.derived_cacheable()
			          << " ace_cacheable=" << ace_cacheable
			          << " domain=" << static_cast<unsigned>(genattr->get_domain())
			          << " bufferable=" << genattr->get_bufferable()
			          << " modifiable=" << genattr->get_modifiable()
			          << " rd_alloc=" << genattr->get_read_allocate()
			          << " wr_alloc=" << genattr->get_write_allocate();
			if (atomic_ext && atomic_ext->is_amo) {
				std::cout << " amo=1"
				          << " amo_op=" << amo_op_to_string(atomic_ext->amo_op)
				          << " amo_phase=" << (atomic_ext->phase == TlmAtomicPhase::Load ? "load" : "store")
				          << " aq=" << atomic_ext->aq
				          << " rl=" << atomic_ext->rl;
			} else if (atomic_ext && atomic_ext->is_lr) {
				std::cout << " lr=1"
				          << " aq=" << atomic_ext->aq
				          << " rl=" << atomic_ext->rl;
			} else if (atomic_ext && atomic_ext->is_sc) {
				std::cout << " sc=1"
				          << " aq=" << atomic_ext->aq
				          << " rl=" << atomic_ext->rl;
			}
			std::cout << std::endl;
			m_debug_cpu_tx_count++;
		}

		cpu_init_socket->b_transport(trans, delay);

		if (created_extension) {
			trans.clear_extension(genattr);
			delete genattr;
		}
	}

	void b_transport_unused_gen(tlm::tlm_generic_payload& trans, sc_time&)
	{
		trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
	}

	void ConnectSockets()
	{
		cpu_init_socket.bind(m_barrier_processer.target_socket);

		// TLMTrafficGenerator can optionally drive the same path in generator mode.
		if (m_use_internal_generator) {
			m_gen.socket.bind(m_barrier_processer.target_socket);
		} else {
			m_gen.socket.bind(unused_gen_target_socket);
		}

		// barrier processer -> cache
		m_barrier_processer.init_socket.bind(m_cache.target_socket);

		// Connect the cache with the port
		m_ace_port.bind_upstream(m_cache);
	}

	TLMTrafficGenerator m_gen;
	BarrierProcesser m_barrier_processer;
	cache_ace<SZ_CACHE, SZ_CACHELINE> m_cache;
	AceTraceFilter m_trace_filter;
	unsigned m_debug_print_budget;
	unsigned m_debug_cpu_tx_count;
	uint64_t m_master_id;
	bool m_use_internal_generator;
};

class ACELiteMaster :
	public sc_core::sc_module
{
public:
	class ACELitePort_M :
		public sc_core::sc_module
	{
	public:
		// Initiator against the interconnect
		tlm_utils::simple_initiator_socket<ACELitePort_M> init_socket;

		// Upstream interface (where traffic generator connects)
		tlm_utils::simple_target_socket<ACELitePort_M> target_socket;

		ACELitePort_M(sc_core::sc_module_name name) :
			sc_module(name),
			init_socket("init_socket"),
			target_socket("target_socket")
		{
			target_socket.register_b_transport(this,
							&ACELitePort_M::b_transport);

		}

	private:
		// Receive and forward transactions (from the traffic
		// generator to the interconnect)
		virtual void b_transport(tlm::tlm_generic_payload& trans,
						sc_time& delay)
		{
			if (trans.is_write()) {
				write_unique(trans);
			} else if (trans.is_read()) {
				read_once(trans);
			}
		}

		void write_unique(tlm::tlm_generic_payload& gp)
		{
			genattr_extension genattr;
			genattr_extension *attr;

			genattr.set_bufferable(true);
			genattr.set_modifiable(true);
			genattr.set_read_allocate(true);
			genattr.set_write_allocate(true);

			genattr.set_secure(false);

			gp.get_extension(attr);
			if (attr) {
				// Leave as normal access (Sec 3.1.5 [1])

				genattr.set_qos(attr->get_qos());
				genattr.set_secure(attr->get_secure());
				genattr.set_region(attr->get_region());
			}

			genattr.set_domain(Domain::Inner);
			genattr.set_snoop(AW::WriteUnique);

			if (do_tlm(gp, genattr)) {
				gp.set_response_status(tlm::TLM_OK_RESPONSE);
			} else {
				gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
			}
		}

		void read_once(tlm::tlm_generic_payload& gp)
		{
			genattr_extension genattr;
			genattr_extension *attr;

			genattr.set_bufferable(true);
			genattr.set_modifiable(true);
			genattr.set_read_allocate(true);
			genattr.set_write_allocate(true);

			gp.get_extension(attr);
			if (attr) {
				// Leave as normal access (Sec 3.1.5 [1])

				genattr.set_qos(attr->get_qos());
				genattr.set_secure(attr->get_secure());
				genattr.set_region(attr->get_region());
			}

			genattr.set_domain(Domain::Inner);
			genattr.set_snoop(AR::ReadOnce);

			if (do_tlm(gp, genattr)) {
				gp.set_response_status(tlm::TLM_OK_RESPONSE);
			} else {
				gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
			}
		}

		bool do_tlm(tlm::tlm_generic_payload& gp_org,
				genattr_extension& attr)
		{
			genattr_extension *genattr;
			sc_time delay(SC_ZERO_TIME);
			tlm::tlm_generic_payload gp;

			genattr = new genattr_extension();
			genattr->copy_from(attr);

			gp.set_command(gp_org.get_command());

			gp.set_address(gp_org.get_address());

			gp.set_data_length(gp_org.get_data_length());
			gp.set_data_ptr(gp_org.get_data_ptr());

			gp.set_byte_enable_ptr(gp_org.get_byte_enable_ptr());
			gp.set_byte_enable_length(gp_org.get_byte_enable_length());

			gp.set_streaming_width(gp_org.get_streaming_width());

			gp.set_dmi_allowed(false);
			gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

			gp.set_extension(genattr);

			init_socket->b_transport(gp, delay);

			return gp.get_response_status() == tlm::TLM_OK_RESPONSE;
		}
	};

	ACELitePort_M m_acelite_port;

	template<typename T>
	ACELiteMaster(sc_core::sc_module_name name, T& transfers) :
		sc_module(name),
		m_acelite_port("m_acelite_port"),
		m_gen("gen", 1)
	{
		// Configure generator
		m_gen.addTransfers(transfers, 0);

		// TLMTrafficGenerator -> cache
		m_gen.socket.bind(m_acelite_port.target_socket);
	}

	template<typename T>
	void connect(T& bridge)
	{
		m_acelite_port.init_socket.bind(bridge.tgt_socket);
	}

	void enableDebug() { m_gen.enableDebug(); }

private:
	TLMTrafficGenerator m_gen;
};

#endif
