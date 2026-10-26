cmake_minimum_required(VERSION 3.20)

# This library is self-contained; its only third-party dependency is fmt.
# This CMake project works both standalone and as a subdirectory.
#
# Unlike libs/envelope, this is an INTERFACE library, not STATIC: it has no
# .cxx and never will - see include/parameter/parameter.hxx for why it must
# stay header-only (it's plugin ABI surface).
project(parameter LANGUAGES CXX)

set(TARGET_PARAMETER parameter)

add_library(${TARGET_PARAMETER} INTERFACE)
target_include_directories(${TARGET_PARAMETER} INTERFACE include)
target_compile_features(${TARGET_PARAMETER} INTERFACE cxx_std_23)

# fmt::fmt-header-only, not fmt::fmt: this library only uses fmt's
# header/template surface (fmt::format, fmt::format_string, ...), and
# fmt::fmt would additionally link this environment's compiled libfmt.a -
# built without -fPIC, so linking it (even transitively, as an INTERFACE
# dependency) into any SHARED consumer fails at link time ("recompile with
# -fPIC"). fmt::fmt-header-only has nothing precompiled to link at all
# (FMT_HEADER_ONLY=1, set automatically via its INTERFACE_COMPILE_DEFINITIONS),
# so every kind of consumer - executable or plugin .so alike - is safe.
find_package(fmt REQUIRED)
target_link_libraries(${TARGET_PARAMETER} INTERFACE fmt::fmt-header-only)

if (NOT DEFINED BUILD_TESTS)
    set(BUILD_TESTS ON)
endif ()
option(PARAMETER_BUILD_TESTS "Build parameter's own tests" ${BUILD_TESTS})
if (PARAMETER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

# `cmake --build . --target parameter_docs` regenerates ./docs/html/ from
# this library's own Doxyfile (include/) - deliberately separate from the
# parent project's `docs` target/Doxyfile, same reasoning as everything
# else about keeping this directory self-contained. Optional: doesn't fail
# configure if Doxygen isn't installed, and isn't part of the default
# `all` target.
find_package(Doxygen)
if (DOXYGEN_FOUND)
    add_custom_target(parameter_docs
            COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
            COMMENT "Generating HTML documentation with Doxygen into docs/html/"
            VERBATIM)
endif ()
