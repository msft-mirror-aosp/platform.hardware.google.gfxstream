#ifndef VIRTGPU_GFXSTREAM_RENDERER_UNSTABLE_H
#define VIRTGPU_GFXSTREAM_RENDERER_UNSTABLE_H

#include "render-utils/virtio-gpu-gfxstream-renderer.h"

#ifdef __cplusplus
extern "C" {
#endif

// Enables the host to control which memory types the guest will be allowed to map. For types not
// in the mask, the bits HOST_VISIBLE and HOST_COHERENT will be removed.
#define STREAM_RENDERER_PARAM_HOST_VISIBLE_MEMORY_MASK 8

// Information about one device's memory mask.
struct stream_renderer_param_host_visible_memory_mask_entry {
    // Which device the mask applies to.
    struct stream_renderer_device_id device_id;
    // Memory types allowed to be host visible are 1, otherwise 0.
    uint32_t memory_type_mask;
};

// Information about the devices in the system with host visible memory type constraints.
struct stream_renderer_param_host_visible_memory_mask {
    // Points to a stream_renderer_param_host_visible_memory_mask_entry array.
    uint64_t entries;
    // Length of the entries array.
    uint64_t num_entries;
};

// Enables the host to control which GPU is used for rendering.
#define STREAM_RENDERER_PARAM_RENDERING_GPU 9

// External callbacks for tracking metrics.
// Separating each function to a parameter allows new functions to be added later.
#define STREAM_RENDERER_PARAM_METRICS_CALLBACK_ADD_INSTANT_EVENT 1024
typedef void (*stream_renderer_param_metrics_callback_add_instant_event)(int64_t event_code);

#define STREAM_RENDERER_PARAM_METRICS_CALLBACK_ADD_INSTANT_EVENT_WITH_DESCRIPTOR 1025
typedef void (*stream_renderer_param_metrics_callback_add_instant_event_with_descriptor)(
    int64_t event_code, int64_t descriptor);

#define STREAM_RENDERER_PARAM_METRICS_CALLBACK_ADD_INSTANT_EVENT_WITH_METRIC 1026
typedef void (*stream_renderer_param_metrics_callback_add_instant_event_with_metric)(
    int64_t event_code, int64_t metric_value);

#define STREAM_RENDERER_PARAM_METRICS_CALLBACK_ADD_VULKAN_OUT_OF_MEMORY_EVENT 1027
typedef void (*stream_renderer_param_metrics_callback_add_vulkan_out_of_memory_event)(
    int64_t result_code, uint32_t op_code, const char* function, uint32_t line,
    uint64_t allocation_size, bool is_host_side_result, bool is_allocation);

#define STREAM_RENDERER_PARAM_METRICS_CALLBACK_SET_ANNOTATION 1028
typedef void (*stream_renderer_param_metrics_callback_set_annotation)(const char* key,
                                                                      const char* value);

#define STREAM_RENDERER_PARAM_METRICS_CALLBACK_ABORT 1029
typedef void (*stream_renderer_param_metrics_callback_abort)();

VG_EXPORT void gfxstream_backend_setup_window(void* native_window_handle, int32_t window_x,
                                              int32_t window_y, int32_t window_width,
                                              int32_t window_height, int32_t fb_width,
                                              int32_t fb_height);

VG_EXPORT void stream_renderer_flush(uint32_t res_handle);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif
