/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Data "temperature" paradigm declarations
 *
 * Copyright (c) 2024-2025 Viacheslav Dubeyko <slava@dubeyko.com>
 */

#ifndef _LINUX_DATA_TEMPERATURE_H
#define _LINUX_DATA_TEMPERATURE_H

/*
 * struct data_temperature - data temperature definition
 * @temperature: current temperature of a file
 * @change_lock: modification lock
 * @state: current state of data temperature object
 * @dirty_blocks: current number of dirty blocks in page cache
 * @updated_blocks: number of updated blocks [start_timestamp, end_timestamp]
 * @start_timestamp: starting timestamp of update operations
 * @end_timestamp: finishing timestamp of update operations
 */
struct data_temperature {
	atomic_t temperature;

	spinlock_t change_lock;
	int state;
	u64 dirty_blocks;
	u64 updated_blocks;
	u64 start_timestamp;
	u64 end_timestamp;
};

enum data_temperature_state {
	DATA_TEMPERATURE_UNKNOWN_STATE,
	DATA_TEMPERATURE_CREATED,
	DATA_TEMPERATURE_UPDATE_STARTED,
	DATA_TEMPERATURE_UPDATE_FINISHED,
	DATA_TEMPERATURE_STATE_MAX
};

#ifdef CONFIG_DATA_TEMPERATURE

int __set_data_temperature_info(struct inode *inode);
void __remove_data_temperature_info(struct inode *inode);
int __get_data_temperature(const struct inode *inode);
int __increase_data_temperature_by_dirty_folio(struct folio *folio);
int __account_flushed_folio_by_data_temperature(struct folio *folio);

static inline
struct data_temperature *get_data_temperature_info(const struct inode *inode)
{
	return smp_load_acquire(&inode->i_data_temperature_info);
}

static inline
int set_data_temperature_info(struct inode *inode)
{
	return __set_data_temperature_info(inode);
}

static inline
void remove_data_temperature_info(struct inode *inode)
{
	__remove_data_temperature_info(inode);
}

static inline
int get_data_temperature(const struct inode *inode)
{
	return __get_data_temperature(inode);
}

static inline
int increase_data_temperature_by_dirty_folio(struct folio *folio)
{
	return __increase_data_temperature_by_dirty_folio(folio);
}

static inline
int account_flushed_folio_by_data_temperature(struct folio *folio)
{
	return __account_flushed_folio_by_data_temperature(folio);
}

#else  /* !CONFIG_DATA_TEMPERATURE */

static inline
int set_data_temperature_info(struct inode *inode)
{
	return 0;
}

static inline
void remove_data_temperature_info(struct inode *inode)
{
	return;
}

static inline
struct data_temperature *get_data_temperature_info(const struct inode *inode)
{
	return ERR_PTR(-EOPNOTSUPP);
}

static inline
int get_data_temperature(const struct inode *inode)
{
	return 0;
}

static inline
int increase_data_temperature_by_dirty_folio(struct folio *folio)
{
	return 0;
}

static inline
int account_flushed_folio_by_data_temperature(struct folio *folio)
{
	return 0;
}

#endif	/* CONFIG_DATA_TEMPERATURE */

#endif	/* _LINUX_DATA_TEMPERATURE_H */
