/* if not defined externally fall back to TARGET_RV64 */
#if !defined(TARGET_RV32) && !defined(TARGET_RV64) && !defined(TARGET_RV64_CHERIV9)
#define TARGET_RV64
#endif

/* if not defined externally fall back to one worker core */
#if !defined(NUM_CORES)
#define NUM_CORES 1
#endif

#include <termios.h>
#include <unistd.h>

#include <boost/io/ios_state.hpp>
#include <boost/program_options.hpp>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <limits>
#include <vector>

#include "core/common/clint.h"
#include "core/common/debug.h"
#include "core/common/debug_memory.h"
#include "core/common/gdb-mc/gdb_runner.h"
#include "core/common/gdb-mc/gdb_server.h"
#include "core/common/lwrt_clint.h"

/*
 * It should be possible to remove the ifdefs here and include all files
 * without any conflicts, and indeed: If we remove the ifdefs we get no
 * compilation errors. However, when we start the resulting VP (especially
 * with CHERI) we get a lot of errors like:
 * [ISS] Error: Multiple implementations for operation 955 (AMOSWAP_C)
 * -> TODO: find/fix cause and remove ifdefs (not critical)
 */
#if defined(TARGET_RV32)
#include "core/rv32/elf_loader.h"
#include "core/rv32/iss.h"
#include "core/rv32/mem.h"
#include "core/rv32/mmu.h"
#include "core/rv32/syscall.h"
#elif defined(TARGET_RV64)
#include "core/rv64/elf_loader.h"
#include "core/rv64/iss.h"
#include "core/rv64/mem.h"
#include "core/rv64/mmu.h"
#include "core/rv64/syscall.h"
#elif defined(TARGET_RV64_CHERIV9)
#include "core/rv64_cheriv9/elf_loader.h"
#include "core/rv64_cheriv9/iss.h"
#include "core/rv64_cheriv9/mem.h"
#include "core/rv64_cheriv9/mmu.h"
#include "core/rv64_cheriv9/syscall.h"
#endif
 
#include "platform/common/channel_console.h"
#include "platform/common/channel_slip.h"
#include "platform/common/ds1307.h"
#include "platform/common/fu540_gpio.h"
#include "platform/common/fu540_i2c.h"
#include "platform/common/fu540_uart.h"
#include "platform/common/memory.h"
#include "platform/common/memory_mapped_file.h"
#include "platform/common/miscdev.h"
#include "platform/common/options.h"
#include "platform/common/sifive_plic.h"
#include "platform/common/sifive_spi.h"
#include "platform/common/sifive_test.h"
#include "platform/common/spi_sd_card.h"
#include "platform/common/tagged_memory.h"
#include "platform/common/vncsimplefb.h"
#include "platform/common/vncsimpleinputkbd.h"
#include "platform/common/vncsimpleinputptr.h"
#include "ace_l2_memory_subsystem.h"
#include "adapter.h"
#include "prci.h"
#include "util/options.h"
#include "util/propertytree.h"
#include "util/vncserver.h"

/* if not defined externally fall back to four worker cores */
#if !defined(NUM_CORES)
#define NUM_CORES (4 + 1)
#endif

#if defined(TARGET_RV32)
using namespace rv32;
#define MEM_SIZE_MB 1024  // MB ram
/*
 * on RV32 linux vmalloc size is very limited
 * -> only small memory areas (images sizes) possible
 */
#define MRAM_SIZE_MB 64  // MB mem mapped file (rootfs)
/* address to load raw (not elf) images provided via --kernel-file */
#define KERNEL_LOAD_ADDR 0x80400000
#define CACHE_ACE_RESERVED_START_ADDR KERNEL_LOAD_ADDR

#elif defined(TARGET_RV64)
using namespace rv64;
#define MEM_SIZE_MB 2048  // MB ram
#define MRAM_SIZE_MB 512  // MB mem mapped file (rootfs)
/* address to load raw (not elf) images provided via --kernel-file */
#define KERNEL_LOAD_ADDR 0x80200000
#define CACHE_ACE_RESERVED_START_ADDR 0xE0000000

#elif defined(TARGET_RV64_CHERIV9)
using namespace cheriv9::rv64;
#define MEM_SIZE_MB 2048  // MB ram
#define MRAM_SIZE_MB 512  // MB mem mapped file (rootfs)
/* address to load raw (not elf) images provided via --kernel-file */
#define KERNEL_LOAD_ADDR 0x80200000
#define CACHE_ACE_RESERVED_START_ADDR 0xE0000000

#endif /* TARGET_RVxx */

namespace po = boost::program_options;

struct LinuxOptions : public Options {
   public:
	typedef uint64_t addr_t;

	addr_t mem_size = 1024ul * 1024ul * (uint64_t)(MEM_SIZE_MB);
	addr_t mem_start_addr = 0x80000000;
	addr_t mem_end_addr = mem_start_addr + mem_size - 1;
	addr_t clint_start_addr = 0x02000000;
	addr_t clint_end_addr = 0x0200ffff;
	addr_t sys_start_addr = 0x02010000;
	addr_t sys_end_addr = 0x020103ff;
	addr_t dtb_rom_start_addr = 0x00001000;
	addr_t dtb_rom_size = 0x2000;
	addr_t dtb_rom_end_addr = dtb_rom_start_addr + dtb_rom_size - 1;
	addr_t uart0_start_addr = 0x10010000;
	addr_t uart0_end_addr = 0x10010fff;
	addr_t uart1_start_addr = 0x10011000;
	addr_t uart1_end_addr = 0x10011fff;
	addr_t gpio_start_addr = 0x10060000;
	addr_t gpio_end_addr = 0x10060FFF;
	addr_t spi0_start_addr = 0x10040000;
	addr_t spi0_end_addr = 0x10040FFF;
	addr_t spi1_start_addr = 0x10041000;
	addr_t spi1_end_addr = 0x10041FFF;
	addr_t spi2_start_addr = 0x10050000;
	addr_t spi2_end_addr = 0x10050FFF;
	addr_t plic_start_addr = 0x0C000000;
	addr_t plic_end_addr = 0x10000000;
	addr_t prci_start_addr = 0x10000000;
	addr_t prci_end_addr = 0x10000FFF;
	addr_t miscdev_start_addr = 0x10001000;
	addr_t miscdev_end_addr = 0x10001FFF;
	addr_t sifive_test_start_addr = 0x100000;
	addr_t sifive_test_end_addr = 0x100fff;
	addr_t vncsimplefb_start_addr = 0x11000000;
	addr_t vncsimplefb_end_addr = 0x11ffffff; /* 16MiB */
	addr_t vncsimpleinputptr_start_addr = 0x12000000;
	addr_t vncsimpleinputptr_end_addr = 0x12000fff;
	addr_t vncsimpleinputkbd_start_addr = 0x12001000;
	addr_t vncsimpleinputkbd_end_addr = 0x12001fff;
	addr_t mram_root_start_addr = 0x40000000;
	addr_t mram_root_size = 1024u * 1024u * (unsigned int)(MRAM_SIZE_MB);
	addr_t mram_root_end_addr = mram_root_start_addr + mram_root_size - 1;
	addr_t mram_data_start_addr = 0x60000000;
	addr_t mram_data_size = 1024u * 1024u * (unsigned int)(MRAM_SIZE_MB);
	addr_t mram_data_end_addr = mram_data_start_addr + mram_data_size - 1;
	addr_t i2c_start_addr = 0x10030000;
	addr_t i2c_end_addr = 0x10031000;

	OptionValue<uint64_t> entry_point;
	std::string dtb_file;
	std::string kernel_file;
	std::string tun_device = "tun0";
	std::string mram_root_image;
	std::string mram_data_image;
	std::string sd_card_image;

	unsigned int vnc_port = 5900;

	bool cheri_purecap = false;
	/* PTE/PBMT 测试窗口：Linux 侧驱动会 mmap 这段 reserved-memory，
	 * VP 侧让它绕过 data DMI，走 master-ace/coherent L2 路径，便于观察
	 * PTE PBMT 属性和 PMA 属性对事务路径的影响。 */
	addr_t cache_ace_dram_window_start = KERNEL_LOAD_ADDR;
	addr_t cache_ace_dram_window_size = 64 * 1024;

	LinuxOptions(void) {
		// clang-format off
		add_options()
			("memory-start", po::value<uint64_t>(&mem_start_addr),"set memory start address")
			("memory-size", po::value<uint64_t>(&mem_size), "set memory size")
			("entry-point", po::value<std::string>(&entry_point.option),"set entry point address (ISS program counter)")
			("dtb-file", po::value<std::string>(&dtb_file)->required(), "dtb file for boot loading")
			("kernel-file", po::value<std::string>(&kernel_file), "optional kernel file to load (supports ELF or RAW files)")
			("tun-device", po::value<std::string>(&tun_device), "tun device used by SLIP")
			("mram-root-image", po::value<std::string>(&mram_root_image)->default_value(""),"MRAM root image file")
			("mram-root-image-size", po::value<uint64_t>(&mram_root_size), "MRAM root image size")
			("mram-data-image", po::value<std::string>(&mram_data_image)->default_value(""),"MRAM data image file for persistency")
			("mram-data-image-size", po::value<uint64_t>(&mram_data_size), "MRAM data image size")
			("sd-card-image", po::value<std::string>(&sd_card_image)->default_value(""), "SD-Card image file (size must be multiple of 512 bytes)")
			("vnc-port", po::value<unsigned int>(&vnc_port), "select port number to connect with VNC")
			("cache-ace-dram-window-start", po::value<uint64_t>(&cache_ace_dram_window_start), "DRAM window start address reserved to bypass DMI and exercise cache_ace")
			("cache-ace-dram-window-size", po::value<uint64_t>(&cache_ace_dram_window_size), "DRAM window size reserved to bypass DMI and exercise cache_ace")
#ifdef TARGET_RV64_CHERIV9
			("cheri-purecap", po::bool_switch(&cheri_purecap), "start in cheri purecap mode")
#endif
			;
		// clang-format on
	}

	void parse(int argc, char **argv) override {
		Options::parse(argc, argv);
		entry_point.finalize(parse_uint64_option);
		mem_end_addr = mem_start_addr + mem_size - 1;
		mram_root_end_addr = mram_root_start_addr + mram_root_size - 1;
		assert(mram_root_end_addr < mram_data_start_addr && "MRAM root too big, would overlap MRAM root");
		mram_data_end_addr = mram_data_start_addr + mram_data_size - 1;
		assert(mram_data_end_addr < mem_start_addr && "MRAM too big, would overlap memory");
		if (!has_option("cache-ace-dram-window-start") && cache_ace_dram_window_size > 0 &&
		    cache_ace_dram_window_size <= mem_size) {
			/*
			 * 默认 cache_ace 窗口需要避开 kernel 装载地址，并落在设备树
			 * reserved-memory 可描述的 DRAM 范围内。这样 Linux 侧 PTE/PBMT
			 * 测试驱动可以安全 mmap 该窗口，不会覆盖内核镜像。
			 */
			const addr_t reserved_start = std::max<addr_t>(mem_start_addr, CACHE_ACE_RESERVED_START_ADDR);
			const addr_t reserved_size = mem_end_addr - reserved_start + 1;
			if (cache_ace_dram_window_size <= reserved_size) {
				cache_ace_dram_window_start = reserved_start;
			} else {
				cache_ace_dram_window_start = (mem_end_addr + 1) - cache_ace_dram_window_size;
				cache_ace_dram_window_start &= ~static_cast<addr_t>(0xFFF);
				if (cache_ace_dram_window_start < mem_start_addr) {
					cache_ace_dram_window_start = mem_start_addr;
				}
			}
		}
#if defined(TARGET_RV64) || defined(TARGET_RV64_CHERIV9)
		if (!has_option("cache-ace-dram-window-size") && cache_ace_dram_window_start <= mem_end_addr) {
			/* RV64 设备树把 cache-ace reserved-memory 描述为从 0xe0000000 到
			 * DRAM 末尾。这里同步 VP 侧 DMI 挖洞/PMA region 的大小，确保
			 * PMA、NC、IO 三个分段都会经过 master-ACE cache-control 路径。 */
			cache_ace_dram_window_size = mem_end_addr - cache_ace_dram_window_start + 1;
		}
#endif
		if (cache_ace_dram_window_start < mem_start_addr || cache_ace_dram_window_start > mem_end_addr) {
			cache_ace_dram_window_size = 0;
		}
		if (cache_ace_dram_window_size > 0) {
			const addr_t max_window_size = mem_end_addr - cache_ace_dram_window_start + 1;
			cache_ace_dram_window_size = std::min(cache_ace_dram_window_size, max_window_size);
		}
	}
};

class Core {
   public:
	ISS iss;
	MMU mmu;
#ifdef TARGET_RV64_CHERIV9
	CombinedTaggedMemoryInterface memif;
#else
	CombinedMemoryInterface memif;
#endif
	InstrMemoryProxy imemif;
	std::vector<MemoryDMI> prepared_data_dmi_ranges;

	Core(RV_ISA_Config *isa_config, unsigned int id, MemoryDMI dmi, uint64_t mem_start_addr, uint64_t mem_end_addr)
	    : iss(isa_config, id),
	      mmu(iss),
#ifdef TARGET_RV64_CHERIV9
	      memif(("MemoryInterface" + std::to_string(id)).c_str(), iss, &mmu, mem_start_addr, mem_end_addr),
#else
	      memif(("MemoryInterface" + std::to_string(id)).c_str(), iss, &mmu),
#endif
	      imemif(dmi, iss) {
		return;
	}

	void init(bool use_data_dmi, bool use_instr_dmi, bool use_dbbcache, bool use_lscache, clint_if *clint,
	          uint64_t entry, uint64_t sp_base, bool cheri_purecap = false) {
		if (use_data_dmi)
			memif.dmi_ranges = prepared_data_dmi_ranges;

#ifdef TARGET_RV64_CHERIV9
		iss.init(get_instr_memory_if(use_instr_dmi), use_dbbcache, &memif, use_lscache, clint, entry, sp_base,
		         cheri_purecap);
#else
		iss.init(get_instr_memory_if(use_instr_dmi), use_dbbcache, &memif, use_lscache, clint, entry, sp_base);
#endif
	}

   private:
	instr_memory_if *get_instr_memory_if(bool use_instr_dmi) {
		if (use_instr_dmi)
			return &imemif;
		else
			return &memif;
	}
};

class DummyBusMaster : public sc_core::sc_module {
   public:
	tlm_utils::simple_initiator_socket<DummyBusMaster> socket;

	DummyBusMaster(sc_core::sc_module_name name) : sc_core::sc_module(name), socket("socket") {}
};

template <typename AddRangeFn>
void add_dmi_ranges_with_cache_ace_window(uint8_t *mem, uint64_t mem_start_addr, uint64_t mem_size,
                                          uint64_t cache_ace_window_start, uint64_t cache_ace_window_size,
                                          AddRangeFn add_range) {
	/* PTE/PBMT 测试窗口必须从 data DMI 中挖掉，否则 CPU 数据访问会直接命中
	 * host 内存指针，绕过 TLM/master-ace/coherent L2，导致看不到 PBMT/PMA
	 * 对事务属性和一致性路径的影响。 */
	const uint64_t mem_end_addr = mem_start_addr + mem_size;
	const uint64_t window_start = std::max(cache_ace_window_start, mem_start_addr);
	const uint64_t window_end = std::min(cache_ace_window_start + cache_ace_window_size, mem_end_addr);

	if (cache_ace_window_size == 0 || window_start >= window_end) {
		add_range(mem, mem_start_addr, mem_size);
		return;
	}

	if (window_start > mem_start_addr) {
		add_range(mem, mem_start_addr, window_start - mem_start_addr);
	}
	if (window_end < mem_end_addr) {
		add_range(mem + (window_end - mem_start_addr), window_end, mem_end_addr - window_end);
	}
}

void handle_kernel_file(const LinuxOptions opt, load_if &mem) {
	if (opt.kernel_file.size() == 0) {
		return;
	}

	std::cout << "Info: load kernel file \"" << opt.kernel_file << "\" ";
	ELFLoader elf(opt.kernel_file.c_str());
	if (elf.is_elf()) {
		/* load elf (use physical addresses) */
		std::cout << "as ELF file (to physical addresses defined in ELF)" << std::endl;
		elf.load_executable_image(mem, mem.get_size(), opt.mem_start_addr, false);
	} else {
		/* load raw to KERNEL_LOAD_ADDR */
		std::cout << "as RAW file (to 0x" << std::hex << KERNEL_LOAD_ADDR << std::dec << ")" << std::endl;
		mem.load_binary_file(opt.kernel_file, KERNEL_LOAD_ADDR - opt.mem_start_addr);
	}
}

int sc_main(int argc, char **argv) {
	// PropertyTree::global()->set_debug(true);

	LinuxOptions opt;
	opt.parse(argc, argv);

	std::cout << "Info: enabled extension " << vp::extensions::ace_l2_memory::extension_name()
	          << " (" << vp::extensions::ace_l2_memory::compiled_components()
	          << " libsystemctlm-soc components, sync="
	          << (vp::extensions::ace_l2_memory::remoteport_sync_ready() ? "ready" : "missing") << ")"
	          << std::endl;

	if (!opt.property_tree_is_loaded) {
		/*
		 * property tree was not loaded by Options -> use default model properties
		 * and values
		 */

		/* set global clock explicitly to 100 MHz */
		VPPP_PROPERTY_SET("", "clock_cycle_period", sc_core::sc_time, sc_core::sc_time(10, sc_core::SC_NS));
	}

	if (opt.use_E_base_isa) {
		std::cerr << "Error: The Linux VP does not support RV32E/RV64E!" << std::endl;
		return -1;
	}
	RV_ISA_Config isa_config(false, opt.en_ext_Zfh);
#ifdef TARGET_RV64_CHERIV9
	isa_config.set_misa_extension(csr_misa::X);    // enable X extension (custom extension bit, marks CHERI is used)
	isa_config.clear_misa_extension(csr_misa::V);  // not supported with cheriv9
	isa_config.clear_misa_extension(csr_misa::N);  // not supported with cheriv9
#endif

	std::srand(std::time(nullptr));  // use current time as seed for random generator

	tlm::tlm_global_quantum::instance().set(sc_core::sc_time(opt.tlm_global_quantum, sc_core::SC_NS));

	VNCServer vncServer("RISC-V VP++ VNCServer", opt.vnc_port);

#ifdef TARGET_RV64_CHERIV9
	TaggedMemory mem("SimpleTaggedMemory", opt.mem_size);
#else
	SimpleMemory mem("SimpleMemory", opt.mem_size);
#endif
	SimpleMemory dtb_rom("DTB_ROM", opt.dtb_rom_size);
	ELFLoader loader(opt.input_program.c_str());
	NetTrace *debug_bus = nullptr;
	if (opt.use_debug_bus) {
		debug_bus = new NetTrace(opt.debug_bus_port);
	}
	SimpleBus<NUM_CORES + 1, 20> bus("SimpleBus", debug_bus, opt.break_on_transaction);
#if defined(TARGET_RV64) && (NUM_CORES > 1)
	vp::extensions::ace_l2_memory::SharedAceL2MemorySubsystem<NUM_CORES> ace_subsystem("AceSubsystem", opt.mem_size,
	                                                                                    false);
	ACEMaster<32 * 1024, 64> *core_l1_master_aces[NUM_CORES];
	DummyBusMaster *unused_bus_masters[NUM_CORES - 1];
#endif
	SyscallHandler sys("SyscallHandler");
	SIFIVE_PLIC plic("PLIC", true, NUM_CORES, 53);
	LWRT_CLINT<NUM_CORES> clint("CLINT");
	PRCI prci("PRCI");
	MiscDev miscdev("MiscDev");
	Channel_Console channel_console;
	FU540_UART uart0("UART0", &channel_console, 4);
	Channel_SLIP channel_slip(opt.tun_device);
	FU540_UART slip("UART1", &channel_slip, 5);
	FU540_I2C i2c("I2C", 50);

	/* interrupts for gpios (idx -> irqnr) */
	const int gpioInterrupts[] = {7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
	FU540_GPIO gpio("GPIO", gpioInterrupts);
	SIFIVE_SPI<8> spi0("SPI0", 1, 51);
	SIFIVE_SPI<8> spi1("SPI1", 4, 52);
	SIFIVE_SPI<8> spi2("SPI2", 1, 6);
	SIFIVE_Test sifive_test("SIFIVE_Test");
	VNCSimpleFB vncsimplefb("VNCSimpleFB", vncServer);
	VNCSimpleInputPtr vncsimpleinputptr("VNCSimpleInputPtr", vncServer, 10);
	VNCSimpleInputKbd vncsimpleinputkbd("VNCSimpleInputKbd", vncServer, 11);
	DebugMemoryInterface dbg_if("DebugMemoryInterface");
	MemoryMappedFile mramRoot("MRAM_Root", opt.mram_root_image, opt.mram_root_size);
	MemoryMappedFile mramData("MRAM_Data", opt.mram_data_image, opt.mram_data_size);

	SPI_SD_Card spi_sd_card(&spi2, 0, &gpio, 11, false);
	if (opt.sd_card_image.length()) {
		spi_sd_card.insert(opt.sd_card_image);
	}

	DS1307 *rtc_ds1307 = new DS1307();
	i2c.register_device(0x68, rtc_ds1307);

	Core *cores[NUM_CORES];
	std::shared_ptr<BusLock> bus_lock = std::make_shared<BusLock>();
	for (unsigned i = 0; i < NUM_CORES; i++) {
		MemoryDMI instr_dmi = MemoryDMI::create_start_size_mapping(mem.data, opt.mem_start_addr, mem.get_size());
		cores[i] = new Core(&isa_config, i, instr_dmi, opt.mem_start_addr, opt.mem_end_addr);

		cores[i]->memif.bus_lock = bus_lock;
		cores[i]->mmu.mem = &cores[i]->memif;

#if defined(TARGET_RV32) || defined(TARGET_RV64)
		if (opt.cache_ace_dram_window_size > 0) {
			/* PMA 由 VP 平台在 M Mode 语义下初始化，不是 OpenSBI 运行时配置。
			 * 这里把 PTE/PBMT 测试窗口显式声明为普通主存：可读、可写、可执行，
			 * cacheable 且 coherent，因此该窗口可以参与 ACE/L2 一致性路径。 */
			pma_attributes cache_ace_attr;
			/* memory_type：该物理窗口按普通主存处理，而不是 MMIO/设备内存。 */
			cache_ace_attr.memory_type = PmaMemoryType::MainMemory;
			/* read：允许 1/2/4/8 字节读访问，不允许 16 字节读访问，允许 misaligned 标量读。 */
			cache_ace_attr.read = {true, true, true, true, false, true};
			/* write：允许 1/2/4/8 字节写访问，不允许 16 字节写访问，允许 misaligned 标量写。 */
			cache_ace_attr.write = {true, true, true, true, false, true};
			/* execute：允许 16-bit RVC 和 32-bit 普通指令取指，不允许 1/8/16 字节取指。 */
			cache_ace_attr.execute = {false, true, true, false, false, true};
			/* ordering：普通内存使用 RISC-V 默认 RVWMO 内存顺序模型。 */
			cache_ace_attr.ordering = PmaOrdering::RVWMO;
			/* cacheable：允许该窗口进入 cache 层次，而不是作为 uncached 设备访问。 */
			cache_ace_attr.cacheable = true;
			/* coherent：该窗口参与一致性维护，可走 master-ace/coherent L2 路径。 */
			cache_ace_attr.coherent = true;
			/* amo_class：允许常见算术 AMO，用于普通可缓存主存的 atomic 操作。 */
			cache_ace_attr.amo_class = PmaAmoClass::AMOArithmetic;
			/* reservability：允许 LR/SC，并声明具备 eventual forward-progress 语义。 */
			cache_ace_attr.reservability = PmaReservability::RsrvEventual;

			const uint64_t pma_mag_page_offset = 0x0fffc000;
			const uint64_t pma_rsrv_none_page_offset = 0x0fffd000;
			const uint64_t pma_amo_page_offset = 0x0fffe000;
			const uint64_t pma_fault_page_offset = 0x0ffff000;
			const uint64_t pma_fault_page_size = 0x1000;
			if (opt.cache_ace_dram_window_size > pma_fault_page_offset + pma_fault_page_size) {
				const uint64_t pma_mag_page_start = opt.cache_ace_dram_window_start + pma_mag_page_offset;
				const uint64_t pma_rsrv_none_page_start =
				    opt.cache_ace_dram_window_start + pma_rsrv_none_page_offset;
				const uint64_t pma_amo_page_start = opt.cache_ace_dram_window_start + pma_amo_page_offset;
				const uint64_t pma_fault_page_start =
				    opt.cache_ace_dram_window_start + pma_fault_page_offset;
				cores[i]->memif.get_pma().set_region(opt.cache_ace_dram_window_start, pma_mag_page_offset,
				                                      cache_ace_attr, MachineMode);

				/* MAG test page：允许 misaligned 8-byte AMO，只要访问完整落在同一个 16-byte granule 内。 */
				pma_attributes mag_attr = cache_ace_attr;
				mag_attr.misaligned_atomicity_granule = 16;
				cores[i]->memif.get_pma().set_region(pma_mag_page_start, pma_fault_page_size, mag_attr,
				                                      MachineMode);

				/* LR/SC reservability test page：普通读写/AMO 允许，但 LR/SC 应触发 PMA access-fault。 */
				pma_attributes rsrv_none_attr = cache_ace_attr;
				rsrv_none_attr.reservability = PmaReservability::RsrvNone;
				cores[i]->memif.get_pma().set_region(pma_rsrv_none_page_start, pma_fault_page_size, rsrv_none_attr,
				                                      MachineMode);

				/* AMO class test page：读/写允许，但只支持 swap/logical AMO，用于 Linux 触发 arithmetic AMO fault。 */
				pma_attributes amo_logical_attr = cache_ace_attr;
				amo_logical_attr.amo_class = PmaAmoClass::AMOLogical;
				cores[i]->memif.get_pma().set_region(pma_amo_page_start, pma_fault_page_size, amo_logical_attr,
				                                      MachineMode);

				/* fault test page：用于 Linux nofault 自测，读/写都会触发 PMA precise access-fault。 */
				pma_attributes fault_attr = cache_ace_attr;
				fault_attr.read = {false, false, false, false, false, false};
				fault_attr.write = {false, false, false, false, false, false};
				fault_attr.execute = {false, false, false, false, false, false};
				fault_attr.page_table_read = false;
				fault_attr.page_table_write = false;
				fault_attr.failure_response = PmaFailureResponse::PreciseAccessFault;
				cores[i]->memif.get_pma().set_region(pma_fault_page_start, pma_fault_page_size, fault_attr,
				                                      MachineMode);

				cores[i]->memif.get_pma().set_region(
				    pma_fault_page_start + pma_fault_page_size,
				    opt.cache_ace_dram_window_size - pma_fault_page_offset - pma_fault_page_size, cache_ace_attr,
				    MachineMode);
			} else {
				cores[i]->memif.get_pma().set_region(opt.cache_ace_dram_window_start, opt.cache_ace_dram_window_size,
				                                      cache_ace_attr, MachineMode);
			}
		}
#endif

		add_dmi_ranges_with_cache_ace_window(
		    mem.data, opt.mem_start_addr, mem.get_size(), opt.cache_ace_dram_window_start, opt.cache_ace_dram_window_size,
		    [&](uint8_t *range_mem, uint64_t range_start, uint64_t range_size) {
			    cores[i]->prepared_data_dmi_ranges.emplace_back(
			        MemoryDMI::create_start_size_mapping(range_mem, range_start, range_size));
		    });

		// enable interactive debug via console
		channel_console.debug_targets_add(&cores[i]->iss);
	}

	if (opt.cache_ace_dram_window_size > 0) {
		const auto window_end = opt.cache_ace_dram_window_start + opt.cache_ace_dram_window_size - 1;
		std::cout << "Info: DRAM cache_ace window reserved at 0x" << std::hex << opt.cache_ace_dram_window_start << "-0x"
		          << window_end << std::dec << " (excluded from data DMI)" << std::endl;
		std::cout << "Info: DRAM cache_ace window PMA is main-memory, cacheable, coherent" << std::endl;
	}

#if defined(TARGET_RV64) && (NUM_CORES > 1)
	for (unsigned i = 0; i < NUM_CORES; i++) {
		core_l1_master_aces[i] = new ACEMaster<32 * 1024, 64>(("CoreMasterACE" + std::to_string(i)).c_str(), WriteBack, i);
		ace_subsystem.connect_master_ace(i, *core_l1_master_aces[i]);
	}
	if (opt.mem_start_addr > 0 && opt.mem_start_addr <= std::numeric_limits<unsigned int>::max()) {
		const unsigned int uncached_prefix_len = static_cast<unsigned int>(opt.mem_start_addr);
		for (unsigned i = 0; i < NUM_CORES; i++) {
			core_l1_master_aces[i]->CreateNonShareableRegion(0, uncached_prefix_len);
		}
		ace_subsystem.create_nonshareable_region(0, uncached_prefix_len);
	}
	ace_subsystem.bind_downstream(bus.tsocks[0]);
	std::cout << "Info: MC RV64 memory path uses mmu -> master-ace -> coherent L2 -> bus/memory" << std::endl;
#endif

	uint64_t entry_point = loader.get_entrypoint();
	if (opt.entry_point.available)
		entry_point = opt.entry_point.value;

	loader.load_executable_image(mem, mem.get_size(), opt.mem_start_addr);
	sys.init(mem.data, opt.mem_start_addr, loader.get_heap_addr(mem.get_size(), opt.mem_start_addr));
	for (size_t i = 0; i < NUM_CORES; i++) {
		bool use_data_dmi = opt.use_data_dmi;
		bool use_instr_dmi = opt.use_instr_dmi;
		if (opt.cache_ace_dram_window_size > 0 && use_instr_dmi) {
			/* 当前 instruction DMI 只支持一段连续映射。启用 cache_ace/PTE 测试窗口时，
			 * DRAM 被切成“窗口外走 DMI、窗口内走 TLM/ACE”的形态，因此关闭 instr-dmi，
			 * 保证取指也能经过 memif，从而接受 PMA execute 权限检查。 */
			std::cerr << "[Options] Info: cache_ace DRAM window disables instr-dmi because instruction DMI only supports one contiguous mapping."
			          << std::endl;
			use_instr_dmi = false;
		}
		cores[i]->init(use_data_dmi, use_instr_dmi, opt.use_dbbcache, opt.use_lscache, &clint, entry_point,
		               opt.mem_end_addr, opt.cheri_purecap);

		sys.register_core(&cores[i]->iss);
		if (opt.intercept_syscalls)
			cores[i]->iss.sys = &sys;
		cores[i]->iss.error_on_zero_traphandler = opt.error_on_zero_traphandler;
	}

	// setup port mapping
	bus.ports[0] = new PortMapping(opt.mem_start_addr, opt.mem_end_addr, mem);
	bus.ports[1] = new PortMapping(opt.clint_start_addr, opt.clint_end_addr, clint);
	bus.ports[2] = new PortMapping(opt.sys_start_addr, opt.sys_end_addr, sys);
	bus.ports[3] = new PortMapping(opt.dtb_rom_start_addr, opt.dtb_rom_end_addr, dtb_rom);
	bus.ports[4] = new PortMapping(opt.uart0_start_addr, opt.uart0_end_addr, uart0);
	bus.ports[5] = new PortMapping(opt.uart1_start_addr, opt.uart1_end_addr, slip);
	bus.ports[6] = new PortMapping(opt.gpio_start_addr, opt.gpio_end_addr, gpio);
	bus.ports[7] = new PortMapping(opt.spi0_start_addr, opt.spi0_end_addr, spi0);
	bus.ports[8] = new PortMapping(opt.spi1_start_addr, opt.spi1_end_addr, spi1);
	bus.ports[9] = new PortMapping(opt.spi2_start_addr, opt.spi2_end_addr, spi2);
	bus.ports[10] = new PortMapping(opt.plic_start_addr, opt.plic_end_addr, plic);
	bus.ports[11] = new PortMapping(opt.prci_start_addr, opt.prci_end_addr, prci);
	bus.ports[12] = new PortMapping(opt.miscdev_start_addr, opt.miscdev_end_addr, miscdev);
	bus.ports[13] = new PortMapping(opt.sifive_test_start_addr, opt.sifive_test_end_addr, sifive_test);
	bus.ports[14] = new PortMapping(opt.vncsimplefb_start_addr, opt.vncsimplefb_end_addr, vncsimplefb);
	bus.ports[15] =
	    new PortMapping(opt.vncsimpleinputptr_start_addr, opt.vncsimpleinputptr_end_addr, vncsimpleinputptr);
	bus.ports[16] =
	    new PortMapping(opt.vncsimpleinputkbd_start_addr, opt.vncsimpleinputkbd_end_addr, vncsimpleinputkbd);
	bus.ports[17] = new PortMapping(opt.mram_root_start_addr, opt.mram_root_end_addr, mramRoot);
	bus.ports[18] = new PortMapping(opt.mram_data_start_addr, opt.mram_data_end_addr, mramData);
	bus.ports[19] = new PortMapping(opt.i2c_start_addr, opt.i2c_end_addr, i2c);
	bus.mapping_complete();

	// connect TLM sockets
	for (size_t i = 0; i < NUM_CORES; i++) {
#if defined(TARGET_RV64) && (NUM_CORES > 1)
		cores[i]->memif.isock.bind(core_l1_master_aces[i]->cpu_target_socket());
#else
		cores[i]->memif.isock.bind(bus.tsocks[i]);
#endif
	}
#if defined(TARGET_RV64) && (NUM_CORES > 1)
	for (size_t i = 1; i < NUM_CORES; i++) {
		unused_bus_masters[i - 1] = new DummyBusMaster(("UnusedBusMaster" + std::to_string(i)).c_str());
		unused_bus_masters[i - 1]->socket.bind(bus.tsocks[i]);
	}
#endif
	dbg_if.isock.bind(bus.tsocks[NUM_CORES]);
	bus.isocks[0].bind(mem.tsock);
	bus.isocks[1].bind(clint.tsock);
	bus.isocks[2].bind(sys.tsock);
	bus.isocks[3].bind(dtb_rom.tsock);
	bus.isocks[4].bind(uart0.tsock);
	bus.isocks[5].bind(slip.tsock);
	bus.isocks[6].bind(gpio.tsock);
	bus.isocks[7].bind(spi0.tsock);
	bus.isocks[8].bind(spi1.tsock);
	bus.isocks[9].bind(spi2.tsock);
	bus.isocks[10].bind(plic.tsock);
	bus.isocks[11].bind(prci.tsock);
	bus.isocks[12].bind(miscdev.tsock);
	bus.isocks[13].bind(sifive_test.tsock);
	bus.isocks[14].bind(vncsimplefb.tsock);
	bus.isocks[15].bind(vncsimpleinputptr.tsock);
	bus.isocks[16].bind(vncsimpleinputkbd.tsock);
	bus.isocks[17].bind(mramRoot.tsock);
	bus.isocks[18].bind(mramData.tsock);
	bus.isocks[19].bind(i2c.tsock);

	// connect interrupt signals/communication
	for (size_t i = 0; i < NUM_CORES; i++) {
		plic.target_harts[i] = &cores[i]->iss;
		clint.target_harts[i] = &cores[i]->iss;
	}
	uart0.plic = &plic;
	slip.plic = &plic;
	gpio.plic = &plic;
	spi0.plic = &plic;
	spi1.plic = &plic;
	spi2.plic = &plic;
	vncsimpleinputptr.plic = &plic;
	vncsimpleinputkbd.plic = &plic;
	i2c.plic = &plic;

	for (size_t i = 0; i < NUM_CORES; i++) {
		// switch for printing instructions
		cores[i]->iss.enable_trace(opt.trace_mode);

		// emulate RISC-V core boot loader
		cores[i]->iss.regs[RegFile::a0] = cores[i]->iss.get_hart_id();
		cores[i]->iss.regs[RegFile::a1] = opt.dtb_rom_start_addr;
	}

	// OpenSBI boots all harts except hart 0 by default.
	//
	// To prevent this hart from being scheduled when stuck in
	// the OpenSBI `sbi_hart_hang()` function do not ignore WFI on
	// this hart.
	//
	// NOTE: ignore_wfi is not set by default -> this only has effect, if ignore_wfi is set in
	// the core initialization loop above
	//
	// See: https://github.com/riscv/opensbi/commit/d70f8aab45d1e449b3b9be26e050b20ed76e12e9
	cores[0]->iss.ignore_wfi = false;

	// load DTB (Device Tree Binary) file
	dtb_rom.load_binary_file(opt.dtb_file, 0);
	std::cout << "Info: DTB loaded" << std::endl;

	// load kernel
	handle_kernel_file(opt, mem);
	std::cout << "Info: kernel load complete" << std::endl;

	std::vector<mmu_memory_if *> mmus;
	std::vector<debug_target_if *> dharts;
	if (opt.use_debug_runner) {
		for (size_t i = 0; i < NUM_CORES; i++) {
			dharts.push_back(&cores[i]->iss);
			mmus.push_back(&cores[i]->memif);
		}

		auto server = new GDBServer("GDBServer", dharts, &dbg_if, opt.debug_port, opt.debug_cont_sim_on_wait, mmus);
		for (size_t i = 0; i < dharts.size(); i++)
			new GDBServerRunner(("GDBRunner" + std::to_string(i)).c_str(), server, dharts[i]);
	} else {
		for (size_t i = 0; i < NUM_CORES; i++) {
			new DirectCoreRunner(cores[i]->iss);
		}
	}
	std::cout << "Info: core runners initialized" << std::endl;

	/* may not return (exit) */
	opt.handle_property_export_and_exit();

	std::cout << "Info: entering sc_start" << std::endl;
	sc_core::sc_start();
	std::cout << "Info: sc_start returned" << std::endl;
	for (size_t i = 0; i < NUM_CORES; i++) {
		cores[i]->iss.show();
	}

	return 0;
}
