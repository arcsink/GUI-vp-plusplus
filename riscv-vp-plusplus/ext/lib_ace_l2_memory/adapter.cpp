#include "adapter.h"
#include "ace_l2_memory_subsystem.h"

extern void *remoteport_tlm_sync_loosely_timed_ptr;
extern void *remoteport_tlm_sync_untimed_ptr;

namespace vp::extensions::ace_l2_memory {

namespace {

constexpr std::size_t kCompiledComponentCount = 13;

}  // namespace

const char *extension_name() {
	return "lib-ace-l2-memory";
}

std::size_t compiled_components() {
	return kCompiledComponentCount;
}

bool remoteport_sync_ready() {
	return remoteport_tlm_sync_loosely_timed_ptr != nullptr && remoteport_tlm_sync_untimed_ptr != nullptr;
}

}  // namespace vp::extensions::ace_l2_memory
