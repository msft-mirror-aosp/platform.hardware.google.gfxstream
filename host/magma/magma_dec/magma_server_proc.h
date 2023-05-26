// Generated Code - DO NOT EDIT !!
// generated by 'emugen'
#ifndef __magma_server_proc_t_h
#define __magma_server_proc_t_h



#include "magma_types.h"
#ifdef _MSC_VER
#include <stdint.h>
#endif
#ifndef magma_APIENTRY
#define magma_APIENTRY 
#endif
typedef magma_status_t (magma_APIENTRY *magma_device_import_server_proc_t) (magma_handle_t, magma_device_t*);
typedef void (magma_APIENTRY *magma_device_release_server_proc_t) (magma_device_t);
typedef magma_status_t (magma_APIENTRY *magma_device_query_server_proc_t) (magma_device_t, uint64_t, magma_handle_t*, uint64_t*);
typedef magma_status_t (magma_APIENTRY *magma_device_query_fudge_server_proc_t) (magma_device_t, uint64_t, magma_bool_t, uint64_t*, uint64_t*, uint64_t*);
typedef magma_status_t (magma_APIENTRY *magma_device_create_connection_server_proc_t) (magma_device_t, magma_connection_t*);
typedef void (magma_APIENTRY *magma_connection_release_server_proc_t) (magma_connection_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_get_error_server_proc_t) (magma_connection_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_create_context_server_proc_t) (magma_connection_t, uint32_t*);
typedef void (magma_APIENTRY *magma_connection_release_context_server_proc_t) (magma_connection_t, uint32_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_create_buffer_server_proc_t) (magma_connection_t, uint64_t, uint64_t*, magma_buffer_t*, magma_buffer_id_t*);
typedef void (magma_APIENTRY *magma_connection_release_buffer_server_proc_t) (magma_connection_t, magma_buffer_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_import_buffer_server_proc_t) (magma_connection_t, magma_handle_t, uint64_t*, magma_buffer_t*, magma_buffer_id_t*);
typedef magma_status_t (magma_APIENTRY *magma_connection_create_semaphore_server_proc_t) (magma_connection_t, magma_semaphore_t*, magma_semaphore_id_t*);
typedef void (magma_APIENTRY *magma_connection_release_semaphore_server_proc_t) (magma_connection_t, magma_semaphore_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_import_semaphore_server_proc_t) (magma_connection_t, magma_handle_t, magma_semaphore_t*, magma_semaphore_id_t*);
typedef magma_status_t (magma_APIENTRY *magma_connection_perform_buffer_op_server_proc_t) (magma_connection_t, magma_buffer_t, uint32_t, uint64_t, uint64_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_map_buffer_server_proc_t) (magma_connection_t, uint64_t, magma_buffer_t, uint64_t, uint64_t, uint64_t);
typedef void (magma_APIENTRY *magma_connection_unmap_buffer_server_proc_t) (magma_connection_t, uint64_t, magma_buffer_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_execute_command_server_proc_t) (magma_connection_t, uint32_t, magma_command_descriptor_t*);
typedef magma_status_t (magma_APIENTRY *magma_connection_execute_command_fudge_server_proc_t) (magma_connection_t, uint32_t, void*, uint64_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_execute_immediate_commands_server_proc_t) (magma_connection_t, uint32_t, uint64_t, magma_inline_command_buffer_t*);
typedef magma_status_t (magma_APIENTRY *magma_connection_execute_immediate_commands_fudge_server_proc_t) (magma_connection_t, uint32_t, uint64_t, void*, uint64_t, uint64_t*);
typedef magma_status_t (magma_APIENTRY *magma_connection_flush_server_proc_t) (magma_connection_t);
typedef magma_handle_t (magma_APIENTRY *magma_connection_get_notification_channel_handle_server_proc_t) (magma_connection_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_read_notification_channel_server_proc_t) (magma_connection_t, void*, uint64_t, uint64_t*, magma_bool_t*);
typedef magma_status_t (magma_APIENTRY *magma_buffer_clean_cache_server_proc_t) (magma_buffer_t, uint64_t, uint64_t, magma_cache_operation_t);
typedef magma_status_t (magma_APIENTRY *magma_buffer_set_cache_policy_server_proc_t) (magma_buffer_t, magma_cache_policy_t);
typedef magma_status_t (magma_APIENTRY *magma_buffer_get_cache_policy_server_proc_t) (magma_buffer_t, magma_cache_policy_t*);
typedef magma_status_t (magma_APIENTRY *magma_buffer_set_name_server_proc_t) (magma_buffer_t, const char*);
typedef magma_status_t (magma_APIENTRY *magma_buffer_set_name_fudge_server_proc_t) (magma_buffer_t, void*, uint64_t);
typedef magma_status_t (magma_APIENTRY *magma_buffer_get_info_server_proc_t) (magma_buffer_t, magma_buffer_info_t*);
typedef magma_status_t (magma_APIENTRY *magma_buffer_get_handle_server_proc_t) (magma_buffer_t, magma_handle_t*);
typedef magma_status_t (magma_APIENTRY *magma_buffer_export_server_proc_t) (magma_buffer_t, magma_handle_t*);
typedef void (magma_APIENTRY *magma_semaphore_signal_server_proc_t) (magma_semaphore_t);
typedef void (magma_APIENTRY *magma_semaphore_reset_server_proc_t) (magma_semaphore_t);
typedef magma_status_t (magma_APIENTRY *magma_semaphore_export_server_proc_t) (magma_semaphore_t, magma_handle_t*);
typedef magma_status_t (magma_APIENTRY *magma_poll_server_proc_t) (magma_poll_item_t*, uint32_t, uint64_t);
typedef magma_status_t (magma_APIENTRY *magma_initialize_tracing_server_proc_t) (magma_handle_t);
typedef magma_status_t (magma_APIENTRY *magma_initialize_logging_server_proc_t) (magma_handle_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_enable_performance_counter_access_server_proc_t) (magma_connection_t, magma_handle_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_enable_performance_counters_server_proc_t) (magma_connection_t, uint64_t*, uint64_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_create_performance_counter_buffer_pool_server_proc_t) (magma_connection_t, magma_perf_count_pool_t*, magma_handle_t*);
typedef magma_status_t (magma_APIENTRY *magma_connection_release_performance_counter_buffer_pool_server_proc_t) (magma_connection_t, magma_perf_count_pool_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_add_performance_counter_buffer_offsets_to_pool_server_proc_t) (magma_connection_t, magma_perf_count_pool_t, const magma_buffer_offset_t*, uint64_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_remove_performance_counter_buffer_from_pool_server_proc_t) (magma_connection_t, magma_perf_count_pool_t, magma_buffer_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_dump_performance_counters_server_proc_t) (magma_connection_t, magma_perf_count_pool_t, uint32_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_clear_performance_counters_server_proc_t) (magma_connection_t, uint64_t*, uint64_t);
typedef magma_status_t (magma_APIENTRY *magma_connection_read_performance_counter_completion_server_proc_t) (magma_connection_t, magma_perf_count_pool_t, uint32_t*, uint64_t*, uint32_t*, uint64_t*, uint32_t*);
typedef magma_status_t (magma_APIENTRY *magma_virt_connection_create_image_server_proc_t) (magma_connection_t, magma_image_create_info_t*, uint64_t*, magma_buffer_t*, magma_buffer_id_t*);
typedef magma_status_t (magma_APIENTRY *magma_virt_connection_get_image_info_server_proc_t) (magma_connection_t, magma_buffer_t, magma_image_info_t*);


#endif
