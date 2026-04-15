add_executable(lrsc_bus_lock_test
	lrsc_bus_lock_test.cpp
)

target_include_directories(lrsc_bus_lock_test
	PRIVATE
		${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(lrsc_bus_lock_test
	PRIVATE
		systemc
		pthread
)

add_test(NAME lrsc_bus_lock
	COMMAND lrsc_bus_lock_test
)
