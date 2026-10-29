cmake_minimum_required(VERSION 3.14)
project(rpc_benchmark LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../ins")

find_package(FBThrift REQUIRED)
find_package(fizz REQUIRED)
find_package(fmt REQUIRED)
find_package(folly REQUIRED)
find_package(wangle REQUIRED)
find_package(gflags REQUIRED)

file(GLOB GEN_CPP_SOURCES CONFIGURE_DEPENDS "gen-cpp2/*.cpp")

set(COMMON_LIBS
    FBThrift::thriftcpp2
    Folly::folly
    wangle::wangle
    fizz::fizz
    fmt::fmt
)

add_library(thrift_generated STATIC ${GEN_CPP_SOURCES})
target_link_libraries(thrift_generated PUBLIC ${COMMON_LIBS})
target_include_directories(thrift_generated PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(press_server
    server/PerfTestService.cpp
    server/PressServer.cpp
    server/ServerPools.cpp
    util/ResourceMonitor.cpp
)
target_link_libraries(press_server PRIVATE thrift_generated ${COMMON_LIBS} atomic)

add_executable(press_client
    client/PressTestClient.cpp
    util/ResourceMonitor.cpp
)
target_link_libraries(press_client PRIVATE thrift_generated ${COMMON_LIBS} atomic)

add_executable(latency_recorder_test
    test/LatencyRecorderTest.cpp
)
target_link_libraries(latency_recorder_test PRIVATE thrift_generated ${COMMON_LIBS})

add_executable(proc_stat_test
    test/ProcStatTest.cpp
)
target_link_libraries(proc_stat_test PRIVATE atomic)

add_executable(paced_schedule_test
    test/PacedScheduleTest.cpp
)

add_executable(server_pools_test
    test/ServerPoolsTest.cpp
    server/ServerPools.cpp
)
target_link_libraries(server_pools_test PRIVATE thrift_generated ${COMMON_LIBS} atomic)

add_executable(handler_tm_test
    test/HandlerThreadManagerTest.cpp
    server/PerfTestService.cpp
    server/ServerPools.cpp
    util/ResourceMonitor.cpp
)
target_link_libraries(handler_tm_test PRIVATE thrift_generated ${COMMON_LIBS} atomic)

# Run with `ctest` in the build directory (cluster_exp/build.sh bench_test).
enable_testing()
add_test(NAME latency_recorder_test COMMAND latency_recorder_test)
add_test(NAME proc_stat_test COMMAND proc_stat_test)
add_test(NAME paced_schedule_test COMMAND paced_schedule_test)
add_test(NAME server_pools_test COMMAND server_pools_test)
add_test(NAME handler_tm_test COMMAND handler_tm_test)
