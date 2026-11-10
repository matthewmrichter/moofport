# MoofPort host build: the portable core (core/) and its tests (tests/).
# The Mac build (mac/) is separate: scripts/build-mac.sh (Retro68 in Docker).
cmake_minimum_required(VERSION 3.16)
project(moofport C)

option(MP_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" ON)
set(MP_MEM_CAP 17578 CACHE STRING "Resident memory cap in bytes (PLAN 2.4: DaynaPORT's 16,554 + 1,024, decision 4)")
set(MP_RX_SLOTS 3 CACHE STRING "RX slots used by test_budget")
set(MP_TX_SLOTS 2 CACHE STRING "TX deferral slots used by test_budget")

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-std=c99 -Wall -Wextra -Werror -g)
if(MP_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

file(GLOB MP_CORE_SRC CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/core/*.c)
add_library(mpcore STATIC ${MP_CORE_SRC})
target_include_directories(mpcore PUBLIC ${CMAKE_SOURCE_DIR}/core)

# Test support (fake device etc.): every tests/*.c that is not tests/test_*.c.
file(GLOB MP_TEST_ALL CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/*.c)
file(GLOB MP_TEST_MAINS CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tests/test_*.c)
set(MP_TEST_SUPPORT ${MP_TEST_ALL})
if(MP_TEST_MAINS)
  list(REMOVE_ITEM MP_TEST_SUPPORT ${MP_TEST_MAINS})
endif()
add_library(mptestsupport STATIC ${MP_TEST_SUPPORT})
target_include_directories(mptestsupport PUBLIC ${CMAKE_SOURCE_DIR}/tests)
target_link_libraries(mptestsupport PUBLIC mpcore)

enable_testing()
foreach(src ${MP_TEST_MAINS})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE mptestsupport mpcore)
  target_compile_definitions(${name} PRIVATE
    MP_MEM_CAP=${MP_MEM_CAP} MP_RX_SLOTS=${MP_RX_SLOTS} MP_TX_SLOTS=${MP_TX_SLOTS})
  add_test(NAME ${name} COMMAND ${name})
endforeach()
