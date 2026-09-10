cmake_minimum_required(VERSION 3.20)

# Kept deliberately self-contained: nothing here includes or links against
# anything under src/ in the parent repo - see the note at the top of
# include/parameter/parameter.hxx. Its only third-party dependency is fmt,
# the same as any standalone consumer of this library would need.
# cmake_minimum_required() is re-asserted here (redundant when
# add_subdirectory()'d from a parent that already required a higher
# version, as this project does) so this file also works as-is if this
# directory is ever lifted out into its own top-level project.
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
            COMMENT "Generating HTML documentation with Doxygen into libs/parameter/docs/html/"
            VERBATIM)
endif ()
