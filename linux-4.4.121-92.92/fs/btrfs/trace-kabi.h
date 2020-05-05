
#ifndef _TRACE_KABI_H_
#define _TRACE_KABI_H_
#ifndef BREAK_LTTNG
#define trace_btrfs_sync_fs(fs_info, args...)				\
	trace_btrfs_sync_fs(args)
#define trace_btrfs_qgroup_trace_extent(fs_info, args...)		\
	trace_btrfs_qgroup_trace_extent(args)
#define trace_btrfs_qgroup_insert_dirty_extent(fs_info, args...)	\
	trace_btrfs_qgroup_insert_dirty_extent(args)
#define trace_btrfs_qgroup_account_extent(fs_info, args...)		\
	trace_btrfs_qgroup_account_extent(args)
#define trace_btrfs_qgroup_account_extents(fs_info, args...)		\
	trace_btrfs_qgroup_account_extents(args)
#define trace_qgroup_update_counters(fs_info, args...)			\
	trace_qgroup_update_counters(args)
#define trace_btrfs_qgroup_free_delayed_ref(fs_info, args...)		\
	trace_btrfs_qgroup_free_delayed_ref(args)
#define trace_add_delayed_ref_head(fs_info, args...)			\
	trace_add_delayed_ref_head(args)
#define trace_run_delayed_ref_head(fs_info, args...)			\
	trace_run_delayed_ref_head(args)
#define trace_add_delayed_tree_ref(fs_info, args...)			\
	trace_add_delayed_tree_ref(args)
#define trace_run_delayed_tree_ref(fs_info, args...)			\
	trace_run_delayed_tree_ref(args)
#define trace_add_delayed_data_ref(fs_info, args...)			\
	trace_add_delayed_data_ref(args)
#define trace_run_delayed_data_ref(fs_info, args...)			\
	trace_run_delayed_data_ref(args)
#endif
#endif
