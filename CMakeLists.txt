cmake_minimum_required(VERSION 3.20)
project(procon37_solver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

function(procon_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /EHsc /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
endfunction()

function(procon_windows_stack target)
    if(WIN32)
        if(MINGW)
            # Keep thread-local destruction in the compiler's own runtime.
            # DLLs found through PATH can crash when worker threads exit.
            target_link_options(${target} PRIVATE -static-libgcc -static-libstdc++)
        endif()
        if(MSVC)
            target_link_options(${target} PRIVATE /STACK:16777216)
        else()
            target_link_options(${target} PRIVATE -Wl,--stack,16777216)
        endif()
    endif()
endfunction()

add_executable(procon_solver main.cpp)
set_target_properties(procon_solver PROPERTIES OUTPUT_NAME main)
target_include_directories(procon_solver PRIVATE inc)
target_link_libraries(procon_solver PRIVATE Threads::Threads)
procon_warnings(procon_solver)
procon_windows_stack(procon_solver)

# Keep the GUI/CLI default path stable for single- and multi-config generators.
set_target_properties(procon_solver PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}")
foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(procon_solver PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${config} "${CMAKE_SOURCE_DIR}")
endforeach()

option(PROCON_BUILD_TESTS "Build the C++ and Python regression tests" ON)
if(PROCON_BUILD_TESTS)
    enable_testing()
    set(PROCON_TESTS
        model_test
        algorithm_test
        solver_options_test
        brand_search_test
        brand_repair_test
        udon_alns_test
        spot_route_lns_test
        quantity_search_test
        official_qa_test
        property_test)
    foreach(test_name IN LISTS PROCON_TESTS)
        add_executable(${test_name} tests/${test_name}.cpp)
        target_include_directories(${test_name} PRIVATE inc)
        target_link_libraries(${test_name} PRIVATE Threads::Threads)
        procon_warnings(${test_name})
        procon_windows_stack(${test_name})
        if(MSVC)
            target_compile_options(${test_name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_name} PRIVATE -UNDEBUG)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    add_test(NAME python_regression
        COMMAND "${CMAKE_COMMAND}" -E env
            "PYTHONPYCACHEPREFIX=${CMAKE_BINARY_DIR}/pycache"
            "PROCON_TEST_SOLVER=$<TARGET_FILE:procon_solver>"
            "${Python3_EXECUTABLE}" -m unittest -v
            tests/visualizer_test.py
            tests/recovery_test.py
            tests/visualizer_runtime_test.py
            tests/small_board_history_test.py)
    set_tests_properties(python_regression PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
