#pragma once

#include <cstddef>

namespace vp::extensions::ace_l2_memory {

const char *extension_name();
std::size_t compiled_components();
bool remoteport_sync_ready();

}  // namespace vp::extensions::ace_l2_memory
