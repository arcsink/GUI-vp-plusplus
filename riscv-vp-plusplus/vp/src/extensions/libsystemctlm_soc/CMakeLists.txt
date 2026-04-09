set(LIBSYSTEMCTLM_SOC_ROOT "${CMAKE_SOURCE_DIR}/../ext/libsystemctlm-soc")
set(LIBSYSTEMCTLM_SOC_EXT_ROOT "${CMAKE_SOURCE_DIR}/../ext/lib_ace_l2_memory")

if(NOT EXISTS "${LIBSYSTEMCTLM_SOC_ROOT}")
    message(FATAL_ERROR "libsystemctlm-soc source tree not found at ${LIBSYSTEMCTLM_SOC_ROOT}")
endif()

if(NOT EXISTS "${LIBSYSTEMCTLM_SOC_EXT_ROOT}")
    message(FATAL_ERROR "libsystemctlm-soc extension tree not found at ${LIBSYSTEMCTLM_SOC_EXT_ROOT}")
endif()

add_library(libsystemctlm_soc_ext STATIC
    ${LIBSYSTEMCTLM_SOC_EXT_ROOT}/adapter.cpp
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/safeio.c
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-proto.c
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-sk.c
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-tlm.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-tlm-ats.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-tlm-memory-master.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-tlm-memory-slave.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-tlm-pci-ep.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port/remote-port-tlm-wires.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/soc/pci/core/pci-device-base.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/soc/xilinx/zynq/xilinx-zynq.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/soc/xilinx/zynqmp/xilinx-zynqmp.cc
    ${LIBSYSTEMCTLM_SOC_ROOT}/trace/trace.cc
)

target_include_directories(libsystemctlm_soc_ext
    PUBLIC
        ${LIBSYSTEMCTLM_SOC_EXT_ROOT}
        ${LIBSYSTEMCTLM_SOC_ROOT}
        ${LIBSYSTEMCTLM_SOC_ROOT}/libremote-port
)

target_link_libraries(libsystemctlm_soc_ext
    PUBLIC
        systemc
        pthread
)

target_compile_features(libsystemctlm_soc_ext PUBLIC cxx_std_17)
