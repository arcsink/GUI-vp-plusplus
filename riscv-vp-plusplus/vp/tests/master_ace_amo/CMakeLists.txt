add_executable(master_ace_amo_test
	master_ace_amo_test.cpp
)

target_include_directories(master_ace_amo_test
	PRIVATE
		${CMAKE_SOURCE_DIR}/src
		${CMAKE_SOURCE_DIR}/../ext/libsystemctlm-soc
)

target_link_libraries(master_ace_amo_test
	PRIVATE
		systemc
		pthread
)

add_test(NAME master_ace_amo
	COMMAND master_ace_amo_test
)
