# ============================================================================
#  A single INTERFACE target carrying the project's warning flags.
#  Link it PRIVATE into our own targets only — never into 3rd-party code.
# ============================================================================
include_guard(GLOBAL)

add_library(qimgv_warnings INTERFACE)
add_library(qimgv::warnings ALIAS qimgv_warnings)

if(MSVC)
    target_compile_options(qimgv_warnings INTERFACE
        /W4
        /permissive-        # standards conformance
        /Zc:__cplusplus     # report the real __cplusplus value
        /utf-8
        /wd4244             # conversion, possible loss of data — pervasive in Qt code
        /wd4267             # size_t -> int conversion
    )
    if(QIMGV_WERROR)
        target_compile_options(qimgv_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(qimgv_warnings INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wnon-virtual-dtor
        -Wnull-dereference
        -Wformat=2
        -Wimplicit-fallthrough

        # Deliberately disabled:
        #   -Wdeprecated-copy: Qt's moc-generated code and the signal/slot
        #     macros trip this constantly and we do not control that code.
        #   -Wdouble-promotion: qimgv stores geometry as float and Qt's API
        #     takes qreal (double), so ordinary widget code promotes on almost
        #     every line. ~28 hits, none of them bugs.
        -Wno-deprecated-copy
    )

    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        target_compile_options(qimgv_warnings INTERFACE
            -Wduplicated-cond
            -Wduplicated-branches
            -Wlogical-op
        )
    endif()

    if(QIMGV_WERROR)
        target_compile_options(qimgv_warnings INTERFACE -Werror)
    endif()
endif()
