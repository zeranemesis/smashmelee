# Offline regression suite for the native HSD port.
#
# The HSD core compiles against headers only, so this target links no Aurora
# library, needs no GPU, and never touches a disc image.  It can be configured
# on its own:
#
#   cmake -S tests/hsd -B build/hsd-tests && cmake --build build/hsd-tests
#   ./build/hsd-tests/melee_hsd_tests
#
# It is also picked up by the main build when MELEE_BUILD_TESTS is ON.

cmake_minimum_required(VERSION 3.20)
project(melee_hsd_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Upstream's C is C89 with a few C99 conveniences; C11 accepts all of it.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif ()

set(MELEE_HSD_TESTS_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)

# The HSD sources under test.  Every file here must stay free of Aurora
# runtime dependencies; the renderer and bootstrap live outside this list
# because they talk to GX and the host window.
set(MELEE_HSD_SOURCES
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/animation.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/animation_player.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/aobj.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/archive.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/class.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/fobj.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/gobj.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/id.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/list.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/mtx_alloc.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/object.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/objalloc.cpp
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd/scene.cpp
)

set(MELEE_HSD_TEST_SOURCES
        ${CMAKE_CURRENT_LIST_DIR}/dat_builder.cpp
        ${CMAKE_CURRENT_LIST_DIR}/gx_texture_stub.cpp
        ${CMAKE_CURRENT_LIST_DIR}/harness.cpp
        ${CMAKE_CURRENT_LIST_DIR}/hsd_fixtures.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_animation.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_animation_player.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_aobj.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_archive.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_class.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_collections.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_fobj.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_gobj.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_objalloc.cpp
        ${CMAKE_CURRENT_LIST_DIR}/test_scene.cpp
)

add_executable(melee_hsd_tests ${MELEE_HSD_SOURCES} ${MELEE_HSD_TEST_SOURCES})
target_include_directories(melee_hsd_tests PRIVATE
        ${MELEE_HSD_TESTS_ROOT}/include
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port
        ${MELEE_HSD_TESTS_ROOT}/src/melee_port/hsd
        # Aurora headers only: the Dolphin SDK type and enum declarations the
        # HSD sources share with the runtime build.
        ${MELEE_HSD_TESTS_ROOT}/extern/aurora/include
        ${MELEE_HSD_TESTS_ROOT}/extern/aurora/include/dolphin
        ${CMAKE_CURRENT_LIST_DIR}
)

if (MSVC)
    # C4324: the allocator case over-aligns its probe object deliberately.
    target_compile_options(melee_hsd_tests PRIVATE /W4 /wd4324)
else ()
    # The HSD sources use partial aggregate initialization for their static
    # class descriptors, which is intentional.
    target_compile_options(melee_hsd_tests PRIVATE -Wall -Wextra
            -Wno-missing-field-initializers)
endif ()

option(MELEE_TESTS_SANITIZE "Build the HSD tests with address/UB sanitizers" OFF)
if (MELEE_TESTS_SANITIZE AND NOT MSVC)
    target_compile_options(melee_hsd_tests PRIVATE -fsanitize=address,undefined
            -fno-omit-frame-pointer)
    target_link_options(melee_hsd_tests PRIVATE -fsanitize=address,undefined)
endif ()

include(CTest)
add_test(NAME melee_hsd_tests COMMAND melee_hsd_tests)

# ---------------------------------------------------------------------------
# Upstream conformance target
#
# Phase 2 of docs/PLAN.md replaces the hand-written HSD with upstream's one
# unit at a time.  This second binary compiles the units that have moved over
# and drives them through the same behavioral assertions, so the swap is
# exercised continuously rather than in one step at the end.  The two binaries
# cannot be one: both implementations define HSD_ObjAlloc.
#
# Units that have moved: objalloc, class, object, list, id, and the two the
# rest depend on -- memory, which the allocator falls back on when no arena is
# set, and hash, which the class registry searches through.

include(${MELEE_HSD_TESTS_ROOT}/cmake/MeleeUpstream.cmake)
melee_upstream_available(MELEE_UPSTREAM_PRESENT)

if (NOT MELEE_UPSTREAM_PRESENT)
    message(STATUS "melee: extern/melee is not checked out; skipping the "
            "upstream conformance target "
            "(git submodule update --init --depth 1 extern/melee)")
else ()
    melee_upstream_sources(MELEE_UPSTREAM_UNITS
            sysdolphin/baselib/objalloc
            sysdolphin/baselib/class
            sysdolphin/baselib/object
            sysdolphin/baselib/list
            sysdolphin/baselib/id
            sysdolphin/baselib/memory
            sysdolphin/baselib/hash)

    add_executable(melee_hsd_upstream_tests
            ${CMAKE_CURRENT_LIST_DIR}/harness.cpp
            ${CMAKE_CURRENT_LIST_DIR}/test_upstream_core.cpp
            ${CMAKE_CURRENT_LIST_DIR}/test_upstream_objalloc.cpp
            ${CMAKE_CURRENT_LIST_DIR}/upstream_host.c
            ${MELEE_UPSTREAM_UNITS}
    )
    target_include_directories(melee_hsd_upstream_tests PRIVATE
            ${CMAKE_CURRENT_LIST_DIR})
    melee_upstream_configure(melee_hsd_upstream_tests)

    if (MSVC)
        target_compile_options(melee_hsd_upstream_tests PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:/W4>
                # HSD_ASSERT hands __FILE__ to a char* parameter, and
                # hsdInitClassInfo takes its names the same way.  C accepts
                # that; C++ needs the older string-literal conversion, which
                # is the exact counterpart of -Wno-writable-strings below.
                $<$<COMPILE_LANGUAGE:CXX>:/Zc:strictStrings->)
    else ()
        target_compile_options(melee_hsd_upstream_tests PRIVATE
                $<$<COMPILE_LANGUAGE:CXX>:-Wall>
                $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
                # HSD_ASSERT hands __FILE__ to a char* parameter, which C++
                # objects to and C does not.
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-writable-strings>
                $<$<COMPILE_LANGUAGE:CXX>:-Wno-write-strings>)
    endif ()

    if (MELEE_TESTS_SANITIZE AND NOT MSVC)
        target_compile_options(melee_hsd_upstream_tests PRIVATE
                -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(melee_hsd_upstream_tests PRIVATE
                -fsanitize=address,undefined)
    endif ()

    add_test(NAME melee_hsd_upstream_tests COMMAND melee_hsd_upstream_tests)
    if (MELEE_TESTS_SANITIZE AND NOT MSVC)
        # See lsan_suppressions.txt: upstream's allocator keeps its pools for
        # the life of the process, by design.
        set_tests_properties(melee_hsd_upstream_tests PROPERTIES ENVIRONMENT
                "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_LIST_DIR}/lsan_suppressions.txt:print_suppressions=0")
    endif ()
endif ()
