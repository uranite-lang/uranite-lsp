
# 
# @author hxAri (hxari)
# @create 2026-08-19 03:40
# @update 2026-08-21 05:38
# @github https://github.com/uranite-lang/uranite-lsp
# 
# Uranite Copyright (c) 2025 - hxAri <hxari@proton.me>
# Uranite Licence under GNU General Public Licence v3
# 
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# any later version.
# 
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <https://www.gnu.org/licenses/>.
# 

cmake_minimum_required( VERSION 3.22 )

enable_testing()

project( UraniteLSP DESCRIPTION "Uranite Language Server Protocol" VERSION 2026.8 LANGUAGES C CXX )

find_program( LLVM_CONFIG NAMES llvm-config llvm-config-19 REQUIRED )
execute_process( COMMAND ${LLVM_CONFIG} --cxxflags OUTPUT_VARIABLE LLVM_CXXFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE )
separate_arguments( LLVM_CXXFLAGS_LIST NATIVE_COMMAND "${LLVM_CXXFLAGS}" )
list( FILTER LLVM_CXXFLAGS_LIST EXCLUDE REGEX "^-std=" )

include( FetchContent )

find_package( fmt QUIET )
if( NOT fmt_FOUND )
    FetchContent_Declare(
        fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG        11.0.2
    )
    FetchContent_MakeAvailable( fmt )
endif()

find_package( GTest QUIET )
if( NOT GTest_FOUND )
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    set( INSTALL_GTEST OFF CACHE BOOL "" FORCE )
    FetchContent_MakeAvailable( googletest )
    include( GoogleTest )
    add_library( GTest::GTest ALIAS gtest )
    add_library( GTest::Main ALIAS gtest_main )
endif()

find_package( spdlog QUIET )
if( NOT spdlog_FOUND )
    FetchContent_Declare(
        spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG        v1.14.1
    )
    FetchContent_MakeAvailable( spdlog )
endif()

include( GNUInstallDirs )

set( CMAKE_CXX_STANDARD 17 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )

find_package( UraniteCore QUIET )
if( UraniteCore_FOUND )
    if( NOT TARGET uranite-core AND TARGET uranite::uranite-core )
        add_library( uranite-core ALIAS uranite::uranite-core )
    endif()
else()
    message( STATUS "UraniteCore not found on system, falling back to FetchContent" )
    set( URANITE_COMPILER_DIR "${CMAKE_SOURCE_DIR}/../uranite" CACHE PATH "Path to Uranite compiler source (FetchContent fallback)" )
    if( EXISTS "${URANITE_COMPILER_DIR}/CMakeLists.txt" )
        FetchContent_Declare( uranite-compiler SOURCE_DIR "${URANITE_COMPILER_DIR}" )
    else()
        FetchContent_Declare( uranite-compiler
            GIT_REPOSITORY https://github.com/uranite-lang/uranite.git
            GIT_TAG        main
        )
    endif()
    set( BUILD_TESTING_SAVED ${BUILD_TESTING} )
    set( BUILD_TESTING OFF CACHE BOOL "" FORCE )
    FetchContent_MakeAvailable( uranite-compiler )
    set( BUILD_TESTING ${BUILD_TESTING_SAVED} CACHE BOOL "" FORCE )
endif()

find_library( TREE_SITTER_URANITE_LIB tree-sitter-uranite )
find_path( TREE_SITTER_URANITE_INCLUDE tree_sitter/tree-sitter-uranite.h )
if( NOT TREE_SITTER_URANITE_LIB OR NOT TREE_SITTER_URANITE_INCLUDE )
    message( STATUS "tree-sitter-uranite not found on system, compiling from source" )
    set( URANITE_TSITTER_DIR "${CMAKE_SOURCE_DIR}/../uranite-tsitter" CACHE PATH "Path to Uranite tree-sitter grammar source" )
    set( URANITE_TSITTER_SOURCES
        "${URANITE_TSITTER_DIR}/src/parser.c"
        "${URANITE_TSITTER_DIR}/src/scanner.c"
    )
    set( URANITE_TSITTER_FROM_SOURCE TRUE )
else()
    set( URANITE_TSITTER_FROM_SOURCE FALSE )
endif()

file( GLOB_RECURSE URANITE_LSP_SOURCES "${CMAKE_SOURCE_DIR}/src/uranite-lsp/*.cpp" )
file( GLOB URANITE_LSP_SOURCES_TEST "${CMAKE_SOURCE_DIR}/src/tests/*.cpp" )

set( URANITE_LSP_MAIN src/uranite-lsp.cpp )

set( URANITE_LSP_SOURCES_LIBRARY ${URANITE_LSP_SOURCES} )
list( REMOVE_ITEM URANITE_LSP_SOURCES_LIBRARY "${CMAKE_SOURCE_DIR}/src/uranite-lsp.cpp" )

if( URANITE_TSITTER_FROM_SOURCE )
    add_executable( uranite-lsp ${URANITE_LSP_MAIN} ${URANITE_LSP_SOURCES_LIBRARY} ${URANITE_TSITTER_SOURCES} )
    add_executable( uranite-lsp-tests ${URANITE_LSP_SOURCES_LIBRARY} ${URANITE_TSITTER_SOURCES} ${URANITE_LSP_SOURCES_TEST} )
    target_include_directories( uranite-lsp PRIVATE src src/uranite-lsp ${URANITE_TSITTER_DIR}/src ${URANITE_TSITTER_DIR}/bindings/c )
    target_include_directories( uranite-lsp-tests PRIVATE src src/uranite-lsp ${URANITE_TSITTER_DIR}/src ${URANITE_TSITTER_DIR}/bindings/c )
    set_source_files_properties( ${URANITE_TSITTER_SOURCES} PROPERTIES LANGUAGE C )
    set_source_files_properties( ${URANITE_TSITTER_SOURCES} PROPERTIES COMPILE_FLAGS "-std=c11" )
else()
    add_executable( uranite-lsp ${URANITE_LSP_MAIN} ${URANITE_LSP_SOURCES_LIBRARY} )
    add_executable( uranite-lsp-tests ${URANITE_LSP_SOURCES_LIBRARY} ${URANITE_LSP_SOURCES_TEST} )
    target_include_directories( uranite-lsp PRIVATE src src/uranite-lsp ${TREE_SITTER_URANITE_INCLUDE} )
    target_include_directories( uranite-lsp-tests PRIVATE src src/uranite-lsp ${TREE_SITTER_URANITE_INCLUDE} )
endif()

target_compile_options( uranite-lsp PRIVATE ${LLVM_CXXFLAGS_LIST} -Wall -Wextra -Wno-unused-parameter -Werror=return-type -O2 -fexceptions -g -DNDEBUG )
target_compile_options( uranite-lsp-tests PRIVATE ${LLVM_CXXFLAGS_LIST} -Wall -Wextra -Wno-unused-parameter -Werror=return-type -O2 -fexceptions -g -DNDEBUG )

find_library( TREE_SITTER_LIB tree-sitter REQUIRED )

if( URANITE_TSITTER_FROM_SOURCE )
    target_link_libraries( uranite-lsp PRIVATE uranite-core ${TREE_SITTER_LIB} )
    target_link_libraries( uranite-lsp-tests PRIVATE uranite-core GTest::GTest GTest::Main ${TREE_SITTER_LIB} pthread )
else()
    target_link_libraries( uranite-lsp PRIVATE uranite-core ${TREE_SITTER_LIB} ${TREE_SITTER_URANITE_LIB} )
    target_link_libraries( uranite-lsp-tests PRIVATE uranite-core GTest::GTest GTest::Main ${TREE_SITTER_LIB} ${TREE_SITTER_URANITE_LIB} pthread )
endif()

install( TARGETS uranite-lsp RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} )

include( GoogleTest )

gtest_discover_tests( uranite-lsp-tests )
