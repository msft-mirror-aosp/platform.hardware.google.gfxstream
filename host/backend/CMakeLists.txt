if (NOT TARGET gfxstream-gl-host-common)
    add_library(
        gfxstream_backend_common
        INTERFACE)
    target_include_directories(
        gfxstream_backend_common
        INTERFACE
        include)
endif()
