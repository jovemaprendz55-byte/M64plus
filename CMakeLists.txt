cmake_minimum_required(VERSION 3.22)
project(m64plus_host_tests LANGUAGES CXX)

enable_testing()
add_executable(m64plus_bus_tests
    tests/bus_tests.cpp
    app/src/main/cpp/n64/rdram.cpp
    app/src/main/cpp/n64/bus.cpp
)
target_include_directories(m64plus_bus_tests PRIVATE app/src/main/cpp)
target_compile_features(m64plus_bus_tests PRIVATE cxx_std_17)
target_compile_options(m64plus_bus_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m64plus_bus_tests COMMAND m64plus_bus_tests)

add_executable(m64plus_rsp_tests
    tests/rsp_cpu_tests.cpp
    app/src/main/cpp/n64/rdram.cpp
    app/src/main/cpp/n64/bus.cpp
    app/src/main/cpp/n64/rsp_cpu.cpp
)
target_include_directories(m64plus_rsp_tests PRIVATE app/src/main/cpp)
target_compile_features(m64plus_rsp_tests PRIVATE cxx_std_17)
target_compile_options(m64plus_rsp_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m64plus_rsp_tests COMMAND m64plus_rsp_tests)

add_executable(m64plus_machine_tests
    tests/machine_tests.cpp
    app/src/main/cpp/n64/rdram.cpp
    app/src/main/cpp/n64/bus.cpp
    app/src/main/cpp/n64/mips_cpu.cpp
    app/src/main/cpp/n64/cp0.cpp
    app/src/main/cpp/n64/tlb.cpp
    app/src/main/cpp/n64/rsp_cpu.cpp
    app/src/main/cpp/n64/machine.cpp
)
target_include_directories(m64plus_machine_tests PRIVATE app/src/main/cpp)
target_compile_features(m64plus_machine_tests PRIVATE cxx_std_17)
target_compile_options(m64plus_machine_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m64plus_machine_tests COMMAND m64plus_machine_tests)

add_executable(m64plus_rdp_tests
    tests/rdp_tests.cpp
    app/src/main/cpp/n64/rdp.cpp
)
target_include_directories(m64plus_rdp_tests PRIVATE app/src/main/cpp)
target_compile_features(m64plus_rdp_tests PRIVATE cxx_std_17)
target_compile_options(m64plus_rdp_tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME m64plus_rdp_tests COMMAND m64plus_rdp_tests)
