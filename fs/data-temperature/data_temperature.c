/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Data "temperature" paradigm implementation
 *
 * Copyright (c) 2024-2025 Viacheslav Dubeyko <slava@dubeyko.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pagemap.h>
#include <linux/data_temperature.h>
#include <linux/fs.h>

#define TIME_IS_UNKNOWN		(U64_MAX)

struct kmem_cache *data_temperature_info_cachep;

static inline
void create_data_temperature_info(struct data_temperature *dt_info)
{
	if (!dt_info)
		return;

	atomic_set(&dt_info->temperature, 0);
	dt_info->updated_blocks = 0;
	dt_info->dirty_blocks = 0;
	dt_info->start_timestamp = TIME_IS_UNKNOWN;
	dt_info->end_timestamp = TIME_IS_UNKNOWN;
	dt_info->state = DATA_TEMPERATURE_CREATED;
}

static inline
void free_data_temperature_info(struct data_temperature *dt_info)
{
	if (!dt_info)
		return;

	kmem_cache_free(data_temperature_info_cachep, dt_info);
}

int __set_data_temperature_info(struct inode *inode)
{
	struct data_temperature *dt_info;

	dt_info = kmem_cache_zalloc(data_temperature_info_cachep, GFP_KERNEL);
	if (!dt_info)
		return -ENOMEM;

	spin_lock_init(&dt_info->change_lock);
	create_data_temperature_info(dt_info);

	if (cmpxchg_release(&inode->i_data_temperature_info,
					NULL, dt_info) != NULL) {
		free_data_temperature_info(dt_info);
		get_data_temperature_info(inode);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(__set_data_temperature_info);

void __remove_data_temperature_info(struct inode *inode)
{
	free_data_temperature_info(inode->i_data_temperature_info);
	inode->i_data_temperature_info = NULL;
}
EXPORT_SYMBOL_GPL(__remove_data_temperature_info);

int __get_data_temperature(const struct inode *inode)
{
	struct data_temperature *dt_info;

	if (!S_ISREG(inode->i_mode))
		return 0;

	dt_info = get_data_temperature_info(inode);
	if (IS_ERR_OR_NULL(dt_info))
		return 0;

	return atomic_read(&dt_info->temperature);
}
EXPORT_SYMBOL_GPL(__get_data_temperature);

static inline
bool is_timestamp_invalid(struct data_temperature *dt_info)
{
	if (!dt_info)
		return false;

	if (dt_info->start_timestamp == TIME_IS_UNKNOWN ||
	    dt_info->end_timestamp == TIME_IS_UNKNOWN)
		return true;

	if (dt_info->start_timestamp > dt_info->end_timestamp)
		return true;

	return false;
}

static inline
u64 get_current_timestamp(void)
{
	return ktime_get_boottime_ns();
}

static inline
void start_account_data_temperature_info(struct data_temperature *dt_info)
{
	if (!dt_info)
		return;

	dt_info->dirty_blocks = 1;
	dt_info->start_timestamp = get_current_timestamp();
	dt_info->end_timestamp = TIME_IS_UNKNOWN;
	dt_info->state = DATA_TEMPERATURE_UPDATE_STARTED;
}

static inline
void __increase_data_temperature(struct inode *inode,
				 struct data_temperature *dt_info)
{
	u64 bytes_count;
	u64 file_blocks;
	u32 block_bytes;
	int dirty_blocks_ratio;
	int updated_blocks_ratio;
	int old_temperature;
	int calculated;

	if (!inode || !dt_info)
		return;

	block_bytes = 1 << inode->i_blkbits;
	bytes_count = i_size_read(inode) + block_bytes - 1;
	file_blocks = bytes_count >> inode->i_blkbits;

	dt_info->dirty_blocks++;

	if (file_blocks > 0) {
		old_temperature = atomic_read(&dt_info->temperature);

		dirty_blocks_ratio = div_u64(dt_info->dirty_blocks,
						file_blocks);
		updated_blocks_ratio = div_u64(dt_info->updated_blocks,
						file_blocks);
		calculated = max_t(int, dirty_blocks_ratio,
					updated_blocks_ratio);

		if (calculated > 0 && old_temperature < calculated)
			atomic_set(&dt_info->temperature, calculated);
	}
}

static inline
void __decrease_data_temperature(struct inode *inode,
				 struct data_temperature *dt_info)
{
	u64 timestamp;
	u64 time_range;
	u64 time_diff;
	u64 bytes_count;
	u64 file_blocks;
	u32 block_bytes;
	u64 blks_per_temperature_degree;
	u64 ns_per_block;
	u64 temperature_diff;

	if (!inode || !dt_info)
		return;

	if (is_timestamp_invalid(dt_info)) {
		create_data_temperature_info(dt_info);
		return;
	}

	timestamp = get_current_timestamp();

	if (dt_info->end_timestamp > timestamp) {
		create_data_temperature_info(dt_info);
		return;
	}

	time_range = dt_info->end_timestamp - dt_info->start_timestamp;
	time_diff = timestamp - dt_info->end_timestamp;

	block_bytes = 1 << inode->i_blkbits;
	bytes_count = i_size_read(inode) + block_bytes - 1;
	file_blocks = bytes_count >> inode->i_blkbits;

	blks_per_temperature_degree = file_blocks;
	if (blks_per_temperature_degree == 0) {
		start_account_data_temperature_info(dt_info);
		return;
	}

	if (dt_info->updated_blocks == 0 || time_range == 0) {
		start_account_data_temperature_info(dt_info);
		return;
	}

	ns_per_block = div_u64(time_range, dt_info->updated_blocks);
	if (ns_per_block == 0)
		ns_per_block = 1;

	if (time_diff == 0) {
		start_account_data_temperature_info(dt_info);
		return;
	}

	temperature_diff = div_u64(time_diff, ns_per_block);
	temperature_diff = div_u64(temperature_diff,
					blks_per_temperature_degree);

	if (temperature_diff == 0)
		return;

	if (temperature_diff <= atomic_read(&dt_info->temperature)) {
		atomic_sub(temperature_diff, &dt_info->temperature);
		dt_info->updated_blocks -=
			temperature_diff * blks_per_temperature_degree;
	} else {
		atomic_set(&dt_info->temperature, 0);
		dt_info->updated_blocks = 0;
	}
}

int __increase_data_temperature_by_dirty_folio(struct folio *folio)
{
	struct inode *inode;
	struct data_temperature *dt_info;

	if (!folio || !folio->mapping)
		return 0;

	inode = folio_inode(folio);

	if (!S_ISREG(inode->i_mode))
		return 0;

	dt_info = get_data_temperature_info(inode);
	if (IS_ERR_OR_NULL(dt_info))
		return 0;

	spin_lock(&dt_info->change_lock);
	switch (dt_info->state) {
	case DATA_TEMPERATURE_CREATED:
		atomic_set(&dt_info->temperature, 0);
		start_account_data_temperature_info(dt_info);
		break;

	case DATA_TEMPERATURE_UPDATE_STARTED:
		__increase_data_temperature(inode, dt_info);
		break;

	case DATA_TEMPERATURE_UPDATE_FINISHED:
		__decrease_data_temperature(inode, dt_info);
		start_account_data_temperature_info(dt_info);
		break;

	default:
		/* do nothing */
		break;
	}
	spin_unlock(&dt_info->change_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(__increase_data_temperature_by_dirty_folio);

static inline
void decrement_dirty_blocks(struct data_temperature *dt_info)
{
	if (!dt_info)
		return;

	if (dt_info->dirty_blocks > 0) {
		dt_info->dirty_blocks--;
		dt_info->updated_blocks++;
	}
}

static inline
void finish_increasing_data_temperature(struct data_temperature *dt_info)
{
	if (!dt_info)
		return;

	if (dt_info->dirty_blocks == 0) {
		dt_info->end_timestamp = get_current_timestamp();
		dt_info->state = DATA_TEMPERATURE_UPDATE_FINISHED;
	}
}

int __account_flushed_folio_by_data_temperature(struct folio *folio)
{
	struct inode *inode;
	struct data_temperature *dt_info;

	if (!folio || !folio->mapping)
		return 0;

	inode = folio_inode(folio);

	if (!S_ISREG(inode->i_mode))
		return 0;

	dt_info = get_data_temperature_info(inode);
	if (IS_ERR_OR_NULL(dt_info))
		return 0;

	spin_lock(&dt_info->change_lock);
	switch (dt_info->state) {
	case DATA_TEMPERATURE_CREATED:
		create_data_temperature_info(dt_info);
		break;

	case DATA_TEMPERATURE_UPDATE_STARTED:
		if (dt_info->dirty_blocks > 0)
			decrement_dirty_blocks(dt_info);
		if (dt_info->dirty_blocks == 0)
			finish_increasing_data_temperature(dt_info);
		break;

	case DATA_TEMPERATURE_UPDATE_FINISHED:
		/* do nothing */
		break;

	default:
		/* do nothing */
		break;
	}
	spin_unlock(&dt_info->change_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(__account_flushed_folio_by_data_temperature);

static int __init data_temperature_init(void)
{
	data_temperature_info_cachep = KMEM_CACHE(data_temperature,
						  SLAB_RECLAIM_ACCOUNT);
	if (!data_temperature_info_cachep)
		return -ENOMEM;

	return 0;
}
late_initcall(data_temperature_init)
