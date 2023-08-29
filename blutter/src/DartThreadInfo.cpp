#include "pch.h"
#include "DartThreadInfo.h"

static std::unordered_map<intptr_t, std::string> threadOffsetNames;

static void initThreadOffsetNames()
{
#define DEFINE_OFFSET_INIT(type_name, member_name, expr, default_init_value) \
	threadOffsetNames[dart::Thread::member_name##offset()] = #member_name;
	CACHED_CONSTANTS_LIST(DEFINE_OFFSET_INIT);
#undef DEFINE_OFFSET_INIT
	for (auto& it : threadOffsetNames) {
		it.second.pop_back(); // remove trailing '_'
	}

#define DEFINE_OFFSET_INIT(name) \
	threadOffsetNames[dart::Thread::name##_entry_point_offset()] = #name;
	RUNTIME_ENTRY_LIST(DEFINE_OFFSET_INIT);
#undef DEFINE_OFFSET_INIT

#define DEFINE_OFFSET_INIT(returntype, name, ...)  \
	threadOffsetNames[dart::Thread::name##_entry_point_offset()] = #name;
	LEAF_RUNTIME_ENTRY_LIST(DEFINE_OFFSET_INIT);
#undef DEFINE_OFFSET_INIT

#define DEFINE_OFFSET_INIT(name) \
	threadOffsetNames[dart::Thread::name##_entry_point_offset()] = #name;
	CACHED_FUNCTION_ENTRY_POINTS_LIST(DEFINE_OFFSET_INIT);
#undef DEFINE_OFFSET_INIT

	// generate from "generate_thread_offsets_cpp.py runtime/vm/thread.h"
	threadOffsetNames[dart::Thread::stack_limit_offset()] = "stack_limit";
	threadOffsetNames[dart::Thread::saved_stack_limit_offset()] = "saved_stack_limit";
	threadOffsetNames[dart::Thread::saved_shadow_call_stack_offset()] = "saved_shadow_call_stack";
	threadOffsetNames[dart::Thread::write_barrier_mask_offset()] = "write_barrier_mask";
#if defined(DART_COMPRESSED_POINTERS)
	threadOffsetNames[dart::Thread::heap_base_offset()] = "heap_base";
#endif
	threadOffsetNames[dart::Thread::stack_overflow_flags_offset()] = "stack_overflow_flags";
	threadOffsetNames[dart::Thread::safepoint_state_offset()] = "safepoint_state";
	threadOffsetNames[dart::Thread::callback_code_offset()] = "ffi_callback_code";
	threadOffsetNames[dart::Thread::callback_stack_return_offset()] = "ffi_callback_stack_return";
	threadOffsetNames[dart::Thread::exit_through_ffi_offset()] = "exit_through_ffi";
	threadOffsetNames[dart::Thread::api_top_scope_offset()] = "api_top_scope";
	threadOffsetNames[dart::Thread::double_truncate_round_supported_offset()] = "double_truncate_round_supported";
	threadOffsetNames[dart::Thread::tsan_utils_offset()] = "tsan_utils";
	threadOffsetNames[dart::Thread::isolate_offset()] = "isolate";
	threadOffsetNames[dart::Thread::isolate_group_offset()] = "isolate_group";
	threadOffsetNames[dart::Thread::field_table_values_offset()] = "field_table_values";
	threadOffsetNames[dart::Thread::dart_stream_offset()] = "dart_stream";
	threadOffsetNames[dart::Thread::service_extension_stream_offset()] = "service_extension_stream";
	threadOffsetNames[dart::Thread::store_buffer_block_offset()] = "store_buffer_block";
	threadOffsetNames[dart::Thread::marking_stack_block_offset()] = "marking_stack_block";
	threadOffsetNames[dart::Thread::top_exit_frame_info_offset()] = "top_exit_frame_info";
	threadOffsetNames[dart::Thread::heap_offset()] = "heap";
	threadOffsetNames[dart::Thread::top_offset()] = "top";
	threadOffsetNames[dart::Thread::end_offset()] = "end";
	threadOffsetNames[dart::Thread::vm_tag_offset()] = "vm_tag";
	threadOffsetNames[dart::Thread::unboxed_runtime_arg_offset()] = "unboxed_runtime_arg";
	threadOffsetNames[dart::Thread::global_object_pool_offset()] = "global_object_pool";
	threadOffsetNames[dart::Thread::dispatch_table_array_offset()] = "dispatch_table_array";
	threadOffsetNames[dart::Thread::active_exception_offset()] = "active_exception";
	threadOffsetNames[dart::Thread::active_stacktrace_offset()] = "active_stacktrace";
	threadOffsetNames[dart::Thread::resume_pc_offset()] = "resume_pc";
	threadOffsetNames[dart::Thread::execution_state_offset()] = "execution_state";
	threadOffsetNames[dart::Thread::next_task_id_offset()] = "next_task_id";
	threadOffsetNames[dart::Thread::random_offset()] = "random";
}

const std::string& GetThreadOffsetName(intptr_t offset)
{
	if (threadOffsetNames.empty())
		initThreadOffsetNames();
	return threadOffsetNames[offset];
}

intptr_t GetThreadMaxOffset()
{
	if (threadOffsetNames.empty())
		initThreadOffsetNames();
	using pair_type = decltype(threadOffsetNames)::value_type;
	auto it = std::max_element(threadOffsetNames.begin(), threadOffsetNames.end(), [](const pair_type& o1, const pair_type& o2)
		{
			return o1.first < o2.first;
		});
	return it->first;
}

const std::unordered_map<intptr_t, std::string>& GetThreadOffsetsMap()
{
	if (threadOffsetNames.empty())
		initThreadOffsetNames();
	return threadOffsetNames;
}