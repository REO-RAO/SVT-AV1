#
# Copyright(c) 2018 Intel Corporation
# SPDX - License - Identifier: BSD - 2 - Clause - Patent
#

cmake_minimum_required(VERSION 3.5)

if("${CMAKE_CURRENT_SOURCE_DIR}" STREQUAL "${CMAKE_CURRENT_BINARY_DIR}")
    message(WARNING "Building in-source is highly not recommended\n"
                    "Please use the Build folder or create your own.")
endif()

option(YASM "Use yasm (if present in PATH)" ON)
if(YASM)
    find_program(YASM_EXE yasm)
    if(YASM_EXE AND NOT CMAKE_ASM_NASM_COMPILER MATCHES "yasm")
        set(CMAKE_ASM_NASM_COMPILER ${YASM_EXE})
        message(STATUS "Found YASM: ${YASM_EXE}")
    endif()
endif()

project(svt-av1 VERSION 0.8.3
    LANGUAGES C CXX ASM_NASM)

# Default build type to release if the generator does not has its own set of build types
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(WARNING "32-bit is not supported")
endif()

if(CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
    set(X86 TRUE)
else()
    set(X86 FALSE)
endif()

include(GNUInstallDirs)
include(CheckCCompilerFlag)
include(CheckCXXCompilerFlag)

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}/)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}/)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/Bin/${CMAKE_BUILD_TYPE}/)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)

# BUILD_SHARED_LIBS is a standard CMake variable, but we declare it here to
# make it prominent in the GUI.
option(BUILD_SHARED_LIBS "Build shared libraries (DLLs)." ON)
message(STATUS "BUILD_SHARED_LIBS: ${BUILD_SHARED_LIBS}")

option(BUILD_TESTING "Build SvtAv1UnitTests, SvtAv1ApiTests, and SvtAv1E2ETests unit tests")
option(COVERAGE "Generate coverage report")
option(BUILD_APPS "Build Enc and Dec Apps" ON)
option(BUILD_ENC "Build Encoder lib and app" ON)
option(BUILD_DEC "Build Decoder lib and app" ON)
if(NOT BUILD_ENC AND NOT BUILD_DEC)
    message(FATAL_ERROR "Not building either the encoder and decoder doesn't make sense.")
endif()

if(WIN32)
    set(CMAKE_ASM_NASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -DWIN64")
else()
    set(CMAKE_ASM_NASM_FLAGS "${CMAKE_ASM_NASM_FLAGS} -DUNIX64")
endif()

if(UNIX)
    if(APPLE)
        set(CMAKE_MACOSX_RPATH 1)
        set(CMAKE_C_ARCHIVE_CREATE   "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_CXX_ARCHIVE_CREATE "<CMAKE_AR> Scr <TARGET> <LINK_FLAGS> <OBJECTS>")
        set(CMAKE_C_ARCHIVE_FINISH   "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
        set(CMAKE_CXX_ARCHIVE_FINISH "<CMAKE_RANLIB> -no_warning_for_no_symbols -c <TARGET>")
    else()
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -pie -z noexecstack -z relro -z now")
    endif()
endif()

set(flags_to_test
    -Wextra
    -Wformat
    -Wformat-security
    -fPIE
    -fPIC)
if(MSVC)
    list(APPEND flags_to_test /MP)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2")
    set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} /Od")
else()
    list(INSERT flags_to_test 0 -Wall)
    option(NATIVE "Build for native performance (march=native)")
    if(NATIVE)
        list(APPEND flags_to_test -march=native)
    endif()
    if(COVERAGE)
        list(APPEND flags_to_test --coverage)
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    endif()
    if(MINGW)
        list(APPEND flags_to_test -mxsave)
    else()
        list(APPEND flags_to_test -fstack-protector-strong -D_FORTIFY_SOURCE=2)
    endif()
endif()

foreach(flag ${flags_to_test};${release_flags_to_test};${debug_flags_to_test})
    string(REGEX REPLACE "[^A-Za-z0-9]" "_" flag_var "${flag}")
    set(test_c_flag "C_FLAG${flag_var}")
    set(test_cxx_flag "CXX_FLAG${flag_var}")
    check_c_compiler_flag(${flag} "${test_c_flag}")
    check_cxx_compiler_flag(${flag} "${test_cxx_flag}")
    if(${flag} IN_LIST release_flags_to_test)
        if(${test_c_flag})
            set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} ${flag}")
        endif()
        if(${test_cxx_flag})
            set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} ${flag}")
        endif()
    elseif(${flag} IN_LIST debug_flags_to_test)
        if(${test_c_flag})
            set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${flag}")
        endif()
        if(${test_cxx_flag})
            set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${flag}")
        endif()
    else()
        if(${test_c_flag})
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${flag}")
        endif()
        if(${test_cxx_flag})
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flag}")
        endif()
    endif()
endforeach()

if(CMAKE_ASM_NASM_OBJECT_FORMAT MATCHES "win")
    set(CMAKE_ASM_NASM_FLAGS_DEBUG "${CMAKE_ASM_NASM_FLAGS_DEBUG} -gcv8")
elseif(CMAKE_ASM_NASM_OBJECT_FORMAT MATCHES "macho")
    if(CMAKE_ASM_NASM_COMPILER MATCHES "nasm")
        set(CMAKE_ASM_NASM_FLAGS_DEBUG "${CMAKE_ASM_NASM_FLAGS_DEBUG} -gdwarf")
    elseif(CMAKE_ASM_NASM_COMPILER MATCHES "yasm")
        set(CMAKE_ASM_NASM_FLAGS_DEBUG "${CMAKE_ASM_NASM_FLAGS_DEBUG} -gnull")
    endif()
else()
    if(CMAKE_ASM_NASM_COMPILER MATCHES "nasm")
        set(CMAKE_ASM_NASM_FLAGS_DEBUG "${CMAKE_ASM_NASM_FLAGS_DEBUG} -gdwarf")
    elseif(CMAKE_ASM_NASM_COMPILER MATCHES "yasm")
        set(CMAKE_ASM_NASM_FLAGS_DEBUG "${CMAKE_ASM_NASM_FLAGS_DEBUG} -gdwarf2")
    endif()
endif()

include(CheckSymbolExists)
check_symbol_exists("_mm512_extracti64x4_epi64" "immintrin.h" HAS_AVX512)
if(NOT HAS_AVX512)
    add_definitions(-DNON_AVX512_SUPPORT)
endif()

# ASM compiler macro
macro(ASM_COMPILE_TO_TARGET target)
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        if(CMAKE_BUILD_TYPE MATCHES Release)
            set(ASM_FLAGS "${CMAKE_ASM_NASM_FLAGS_RELEASE}")
        elseif(CMAKE_BUILD_TYPE MATCHES Debug)
            set(ASM_FLAGS "${CMAKE_ASM_NASM_FLAGS_DEBUG}")
        elseif(CMAKE_BUILD_TYPE MATCHES MinSizeRel)
            set(ASM_FLAGS "${CMAKE_ASM_NASM_FLAGS_MINSIZEREL}")
        elseif(CMAKE_BUILD_TYPE MATCHES RelWithDebInfo)
            set(ASM_FLAGS "${CMAKE_ASM_NASM_FLAGS_RELWITHDEBINFO}")
        endif()

        get_property(inc_dirs DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY INCLUDE_DIRECTORIES)
        foreach(inc_dir ${inc_dirs})
            set(ASM_FLAGS "${ASM_FLAGS} -I${inc_dir}")
        endforeach()

        string(REGEX REPLACE " " ";" ASM_FLAGS "${ASM_FLAGS} ${CMAKE_ASM_NASM_FLAGS}")

        foreach(asm_file ${ARGN})
            get_filename_component(filename "${asm_file}" NAME_WE)
            set(SIMD_OBJ "${CMAKE_CURRENT_BINARY_DIR}/${filename}${CMAKE_C_OUTPUT_EXTENSION}")
            message(STATUS "Pregenerating ASM_NASM file ${filename}${CMAKE_C_OUTPUT_EXTENSION}")
            execute_process(COMMAND ${CMAKE_ASM_NASM_COMPILER}
                    -f${CMAKE_ASM_NASM_OBJECT_FORMAT}
                    -I${CMAKE_CURRENT_SOURCE_DIR}
                    -o${SIMD_OBJ}
                    ${ASM_FLAGS}
                    ${CMAKE_CURRENT_SOURCE_DIR}/${asm_file}
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})
            list(APPEND SIMD_OBJS "${SIMD_OBJ}")
        endforeach()
        #target_sources(${target} PUBLIC ${SIMD_OBJS})
        target_sources(common_lib INTERFACE ${SIMD_OBJS})
    else()
        # For every other generator
        foreach(asm_file ${ARGN})
            get_filename_component(filename "${asm_file}" ABSOLUTE)
            target_sources(${target} PUBLIC ${filename})
        endforeach()
    endif()
endmacro()

# Add Subdirectories
add_subdirectory(Source/Lib/Common)
if(BUILD_ENC)
    add_subdirectory(Source/Lib/Encoder)
endif()
if(BUILD_DEC)
    add_subdirectory(Source/Lib/Decoder)
endif()
if(BUILD_APPS AND BUILD_ENC)
    add_subdirectory(Source/App/EncApp)
endif()
if(BUILD_APPS AND BUILD_DEC)
    add_subdirectory(Source/App/DecApp)
endif()
if(BUILD_TESTING)
    include(CTest)
    message(STATUS "Building UnitTests")
    add_subdirectory(test)
    add_subdirectory(third_party/googletest)
endif()

add_subdirectory(third_party/fastfeat)

install(DIRECTORY ${PROJECT_SOURCE_DIR}/Source/API/ DESTINATION "${CMAKE_INSTALL_FULL_INCLUDEDIR}/svt-av1" FILES_MATCHING PATTERN "*.h")
