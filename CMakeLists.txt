cmake_minimum_required(VERSION 3.16)
project(hotpath CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)  # -O2 -g: we profile optimized code, with symbols
endif()

# Frame pointers are load-bearing for this whole project: the profiler unwinds by
# walking the AArch64 frame record, and the test binaries profile THEMSELVES, so
# they too must keep frame pointers. This is a correctness flag here, not a
# debugging nicety.
add_compile_options(-Wall -Wextra -Wpedantic -fno-omit-frame-pointer)

include_directories(${CMAKE_SOURCE_DIR}/include)

# --- core library ---------------------------------------------------------
add_library(hotpath STATIC src/sampler.cpp)
target_link_libraries(hotpath PUBLIC pthread)

# --- tests ----------------------------------------------------------------
enable_testing()

# Header-only-dependency suites: just compile the test against the headers.
add_executable(test_ring_buffer tests/test_ring_buffer.cpp)
target_link_libraries(test_ring_buffer PRIVATE pthread)

add_executable(test_unwind tests/test_unwind.cpp)

# The integration test profiles a known call chain in its own process. Two extra
# flags matter ONLY here:
#   -fno-optimize-sibling-calls : keep the nested chain as real frames (no tail-call
#                                 collapse), so multi-frame unwinding is observable.
#   ENABLE_EXPORTS (-rdynamic)  : put extern symbols in .dynsym so the test's dladdr
#                                 oracle can name frames. (The real tool parses
#                                 .symtab and needs neither -- see SPEC 3.4.)
add_executable(test_sampler tests/test_sampler.cpp)
target_compile_options(test_sampler PRIVATE -fno-optimize-sibling-calls)
set_target_properties(test_sampler PROPERTIES ENABLE_EXPORTS ON)
target_link_libraries(test_sampler PRIVATE hotpath dl)

add_test(NAME ring_buffer COMMAND test_ring_buffer)
add_test(NAME unwind      COMMAND test_unwind)
add_test(NAME sampler     COMMAND test_sampler)
