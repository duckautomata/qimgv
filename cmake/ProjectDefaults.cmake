# ============================================================================
#  Sane defaults so a bare `cmake -S . -B build` does the right thing.
# ============================================================================
include_guard(GLOBAL)

# --- Honour the pre-fork option names ---------------------------------------
# qimgv_compat_option(<old_name> <new_name>)
# If the user passed -D<old_name>=..., copy it onto <new_name> and warn once.
macro(qimgv_compat_option _old _new)
    if(DEFINED ${_old})
        message(WARNING "-D${_old} is deprecated; use -D${_new} instead.")
        set(${_new} "${${_old}}" CACHE BOOL "" FORCE)
    endif()
endmacro()

# --- Default build type -----------------------------------------------------
get_property(_qimgv_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _qimgv_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
    message(STATUS "No CMAKE_BUILD_TYPE specified — defaulting to Release")
endif()
set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
    Debug Release RelWithDebInfo MinSizeRel)

# --- C++ standard -----------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Tooling ----------------------------------------------------------------
# Always emit compile_commands.json so clangd / IDEs work with zero setup.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "" FORCE)

# Put binaries somewhere predictable instead of scattered through the tree.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Hide symbols by default: smaller binaries, faster dynamic linking.
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# --- Compiler cache ---------------------------------------------------------
if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(QIMGV_CCACHE NAMES ccache sccache)
    if(QIMGV_CCACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${QIMGV_CCACHE}" CACHE STRING "" FORCE)
        message(STATUS "Using compiler cache: ${QIMGV_CCACHE}")
    endif()
endif()

# ============================================================================
#  Option-dependent tuning. Must be called AFTER the option() block so that
#  QIMGV_LTO / QIMGV_SANITIZE actually have values.
# ============================================================================
macro(qimgv_apply_build_tuning)
    # --- Link-time optimization -------------------------------------------------
    if(QIMGV_LTO)
        include(CheckIPOSupported)
        check_ipo_supported(RESULT _qimgv_ipo_ok OUTPUT _qimgv_ipo_msg)
        if(_qimgv_ipo_ok)
            # Release only — LTO makes debug builds slow to link for no benefit.
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
            set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_MINSIZEREL ON)
        else()
            message(STATUS "LTO unavailable, skipping: ${_qimgv_ipo_msg}")
        endif()
    endif()

    # --- Faster linkers ---------------------------------------------------------
    # mold/lld cut link time dramatically on a target this size.
    if(UNIX AND NOT APPLE AND NOT CMAKE_CXX_LINKER_LAUNCHER)
        find_program(QIMGV_MOLD mold)
        find_program(QIMGV_LLD  ld.lld)
        if(QIMGV_MOLD)
            add_link_options("-fuse-ld=mold")
            message(STATUS "Using linker: mold")
        elseif(QIMGV_LLD)
            add_link_options("-fuse-ld=lld")
            message(STATUS "Using linker: lld")
        endif()
    endif()

    # --- Sanitizers -------------------------------------------------------------
    if(QIMGV_SANITIZE)
        if(MSVC)
            message(WARNING "QIMGV_SANITIZE is not supported with MSVC; ignoring.")
        else()
            add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
            add_link_options(-fsanitize=address,undefined)
            message(STATUS "Sanitizers enabled: address, undefined")
        endif()
    endif()
endmacro()
