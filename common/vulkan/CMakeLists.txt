add_library(gfxstream_vulkan_headers INTERFACE)
target_include_directories(
    gfxstream_vulkan_headers
    INTERFACE include)
android_license(TARGET gfxstream_vulkan_headers LICENSE Apache-2.0)
