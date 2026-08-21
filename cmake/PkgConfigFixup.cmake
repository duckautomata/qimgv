# ============================================================================
#  Repair .pc files that hardcode MSYS-style absolute paths (Windows only).
#
#  MSYS2 packages are relocatable by convention: a .pc file declares
#  `prefix=/ucrt64` and derives everything from it, and pkgconf rewrites that
#  prefix to the real install root (C:/msys64/ucrt64) before handing the flags
#  back. Some packages skip the variables and write the path out literally --
#  mingw-w64-ucrt-x86_64-mujs 1.3.9 emits `Cflags: -I/ucrt64/include`. pkgconf
#  has nothing to relocate there, so the MSYS path leaks through verbatim.
#
#  CMake is a native Windows binary. It resolves /ucrt64/include against the
#  current drive root, finds nothing, and fails the generate step with
#  "Imported target ... includes non-existent path".
#
#  This bites libmpv rather than the other dependencies only because mpv lists
#  mujs in Requires.private, and pkgconf folds private requirements into
#  --cflags.
#
#  qimgv_fix_pkgconfig_paths(<target>...) maps such paths back onto the real
#  MSYS2 root and drops whatever is still dangling. A no-op everywhere else.
# ============================================================================
include_guard(GLOBAL)

# Subsystem directories as they appear at the root of an MSYS path.
set(QIMGV_MSYS_PREFIXES ucrt64 mingw64 mingw32 clang64 clang32 clangarm64 usr)

# Derive the MSYS2 install root (e.g. C:/msys64) from a tool we already know
# the location of. Sets <out_var> to "" when this is not an MSYS2 toolchain.
function(_qimgv_msys_root out_var)
    set(${out_var} "" PARENT_SCOPE)
    foreach(_probe IN ITEMS "${CMAKE_CXX_COMPILER}" "${PKG_CONFIG_EXECUTABLE}")
        if(NOT _probe)
            continue()
        endif()
        file(TO_CMAKE_PATH "${_probe}" _probe)
        foreach(_prefix IN LISTS QIMGV_MSYS_PREFIXES)
            if(_probe MATCHES "^(.+)/${_prefix}/")
                set(${out_var} "${CMAKE_MATCH_1}" PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endforeach()
endfunction()

function(qimgv_fix_pkgconfig_paths)
    if(NOT CMAKE_HOST_WIN32)
        return()
    endif()

    _qimgv_msys_root(_root)

    foreach(_target IN LISTS ARGN)
        if(NOT TARGET ${_target})
            continue()
        endif()
        foreach(_property INTERFACE_INCLUDE_DIRECTORIES INTERFACE_LINK_DIRECTORIES)
            get_target_property(_dirs ${_target} ${_property})
            if(NOT _dirs)
                continue()
            endif()

            set(_kept "")
            set(_changed FALSE)
            foreach(_dir IN LISTS _dirs)
                if(EXISTS "${_dir}")
                    list(APPEND _kept "${_dir}")
                    continue()
                endif()

                set(_fixed "")
                if(_root AND _dir MATCHES "^/([^/]+)/")
                    if("${CMAKE_MATCH_1}" IN_LIST QIMGV_MSYS_PREFIXES
                       AND EXISTS "${_root}${_dir}")
                        set(_fixed "${_root}${_dir}")
                    endif()
                endif()

                set(_changed TRUE)
                if(_fixed)
                    list(APPEND _kept "${_fixed}")
                    message(STATUS "${_target}: MSYS path ${_dir} -> ${_fixed}")
                else()
                    message(STATUS "${_target}: dropped dangling path ${_dir}")
                endif()
            endforeach()

            if(_changed)
                list(REMOVE_DUPLICATES _kept)
                set_target_properties(${_target} PROPERTIES ${_property} "${_kept}")
            endif()
        endforeach()
    endforeach()
endfunction()
