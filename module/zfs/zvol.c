/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (C) 2008-2010 Lawrence Livermore National Security, LLC.
 * Produced at Lawrence Livermore National Laboratory (cf, DISCLAIMER).
 * Rewritten for Linux by Brian Behlendorf <behlendorf1@llnl.gov>.
 * LLNL-CODE-403049.
 *
 * ZFS volume emulation driver.
 *
 * Makes a DMU object look like a volume of arbitrary size, up to 2^64 bytes.
 * Volumes are accessed through the symbolic links named:
 *
 * /dev/<pool_name>/<dataset_name>
 *
 * Volumes are persistent through reboot and module load.  No user command
 * needs to be run before opening and using a device.
 *
 * Copyright 2014 Nexenta Systems, Inc.  All rights reserved.
 * Copyright (c) 2016 Actifio, Inc. All rights reserved.
 * Copyright (c) 2012, 2019 by Delphix. All rights reserved.
 */

/*
 * Note on locking of zvol state structures.
 *
 * These structures are used to maintain internal state used to emulate block
 * devices on top of zvols. In particular, management of device minor number
 * operations - create, remove, rename, and set_snapdev - involves access to
 * these structures. The zvol_state_lock is primarily used to protect the
 * zvol_state_list. The zv->zv_state_lock is used to protect the contents
 * of the zvol_state_t structures, as well as to make sure that when the
 * time comes to remove the structure from the list, it is not in use, and
 * therefore, it can be taken off zvol_state_list and freed.
 *
 * The zv_suspend_lock was introduced to allow for suspending I/O to a zvol,
 * e.g. for the duration of receive and rollback operations. This lock can be
 * held for significant periods of time. Given that it is undesirable to hold
 * mutexes for long periods of time, the following lock ordering applies:
 * - take zvol_state_lock if necessary, to protect zvol_state_list
 * - take zv_suspend_lock if necessary, by the code path in question
 * - take zv_state_lock to protect zvol_state_t
 *
 * The minor operations are issued to spa->spa_zvol_taskq queues, that are
 * single-threaded (to preserve order of minor operations), and are executed
 * through the zvol_task_cb that dispatches the specific operations. Therefore,
 * these operations are serialized per pool. Consequently, we can be certain
 * that for a given zvol, there is only one operation at a time in progress.
 * That is why one can be sure that first, zvol_state_t for a given zvol is
 * allocated and placed on zvol_state_list, and then other minor operations
 * for this zvol are going to proceed in the order of issue.
 *
 */
#include <sys/dataset_kstats.h>
#include <sys/dbuf.h>
#include <sys/dmu_traverse.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_dir.h>
#include <sys/zap.h>
#include <sys/zfeature.h>
#include <sys/zil_impl.h>
#include <sys/dmu_tx.h>
#include <sys/zio.h>
#include <sys/zfs_rlock.h>
#include <sys/spa_impl.h>
#include <sys/zvol.h>

#include <sys/zvol_impl.h>

#ifdef ZFS_DEBUG
#define	DEBUG_REFCOUNT_ADD(b) atomic_inc_32(&(b))
#define	DEBUG_REFCOUNT_DEC(b) atomic_dec_32(&(b))

/* BEGIN CSTYLED */
static uint32_t dmu_ctx_deferred;
ZFS_MODULE_PARAM(zfs_zvol, , dmu_ctx_deferred, UINT, ZMOD_RD,
    "DMU contexts deferred in zvol_dmu_ctx_init");
static uint32_t dmu_ctx_active;
ZFS_MODULE_PARAM(zfs_zvol, , dmu_ctx_active, UINT, ZMOD_RD,
    "DMU contexts active in zvol_dmu_ctx_init / zvol_dmu_issue");
static uint32_t dmu_ctx_in_init;
ZFS_MODULE_PARAM(zfs_zvol, , dmu_ctx_in_init, UINT, ZMOD_RD,
    "DMU contexts active in zvol_dmu_ctx_init");
static uint32_t dmu_ctx_in_prefault;
ZFS_MODULE_PARAM(zfs_zvol, , dmu_ctx_in_prefault, UINT, ZMOD_RD,
    "DMU contexts active in prefault");
/* END CSTYLED */
#else
#define	DEBUG_REFCOUNT_ADD(b)
#define	DEBUG_REFCOUNT_DEC(b)
#endif


unsigned int zvol_inhibit_dev = 0;
unsigned int zvol_volmode = ZFS_VOLMODE_GEOM;

struct hlist_head *zvol_htable;
list_t zvol_state_list;
krwlock_t zvol_state_lock;
const zvol_platform_ops_t *ops;

typedef enum {
	ZVOL_ASYNC_REMOVE_MINORS,
	ZVOL_ASYNC_RENAME_MINORS,
	ZVOL_ASYNC_SET_SNAPDEV,
	ZVOL_ASYNC_SET_VOLMODE,
	ZVOL_ASYNC_MAX
} zvol_async_op_t;

typedef struct {
	zvol_async_op_t op;
	char pool[MAXNAMELEN];
	char name1[MAXNAMELEN];
	char name2[MAXNAMELEN];
	zprop_source_t source;
	uint64_t value;
} zvol_task_t;

uint64_t
zvol_name_hash(const char *name)
{
	int i;
	uint64_t crc = -1ULL;
	const uint8_t *p = (const uint8_t *)name;
	ASSERT(zfs_crc64_table[128] == ZFS_CRC64_POLY);
	for (i = 0; i < MAXNAMELEN - 1 && *p; i++, p++) {
		crc = (crc >> 8) ^ zfs_crc64_table[(crc ^ (*p)) & 0xFF];
	}
	return (crc);
}

/*
 * Find a zvol_state_t given the name and hash generated by zvol_name_hash.
 * If found, return with zv_suspend_lock and zv_state_lock taken, otherwise,
 * return (NULL) without the taking locks. The zv_suspend_lock is always taken
 * before zv_state_lock. The mode argument indicates the mode (including none)
 * for zv_suspend_lock to be taken.
 */
zvol_state_t *
zvol_find_by_name_hash(const char *name, uint64_t hash, int mode)
{
	zvol_state_t *zv;
	struct hlist_node *p = NULL;

	rw_enter(&zvol_state_lock, RW_READER);
	hlist_for_each(p, ZVOL_HT_HEAD(hash)) {
		zv = hlist_entry(p, zvol_state_t, zv_hlink);
		mutex_enter(&zv->zv_state_lock);
		if (zv->zv_hash == hash &&
		    strncmp(zv->zv_name, name, MAXNAMELEN) == 0) {
			/*
			 * this is the right zvol, take the locks in the
			 * right order
			 */
			if (mode != RW_NONE &&
			    !rw_tryenter(&zv->zv_suspend_lock, mode)) {
				mutex_exit(&zv->zv_state_lock);
				rw_enter(&zv->zv_suspend_lock, mode);
				mutex_enter(&zv->zv_state_lock);
				/*
				 * zvol cannot be renamed as we continue
				 * to hold zvol_state_lock
				 */
				ASSERT(zv->zv_hash == hash &&
				    strncmp(zv->zv_name, name, MAXNAMELEN)
				    == 0);
			}
			rw_exit(&zvol_state_lock);
			return (zv);
		}
		mutex_exit(&zv->zv_state_lock);
	}
	rw_exit(&zvol_state_lock);

	return (NULL);
}

/*
 * Find a zvol_state_t given the name.
 * If found, return with zv_suspend_lock and zv_state_lock taken, otherwise,
 * return (NULL) without the taking locks. The zv_suspend_lock is always taken
 * before zv_state_lock. The mode argument indicates the mode (including none)
 * for zv_suspend_lock to be taken.
 */
static zvol_state_t *
zvol_find_by_name(const char *name, int mode)
{
	return (zvol_find_by_name_hash(name, zvol_name_hash(name), mode));
}

/*
 * ZFS_IOC_CREATE callback handles dmu zvol and zap object creation.
 */
void
zvol_create_cb(objset_t *os, void *arg, cred_t *cr, dmu_tx_t *tx)
{
	zfs_creat_t *zct = arg;
	nvlist_t *nvprops = zct->zct_props;
	int error;
	uint64_t volblocksize, volsize;

	VERIFY(nvlist_lookup_uint64(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLSIZE), &volsize) == 0);
	if (nvlist_lookup_uint64(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE), &volblocksize) != 0)
		volblocksize = zfs_prop_default_numeric(ZFS_PROP_VOLBLOCKSIZE);

	/*
	 * These properties must be removed from the list so the generic
	 * property setting step won't apply to them.
	 */
	VERIFY(nvlist_remove_all(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLSIZE)) == 0);
	(void) nvlist_remove_all(nvprops,
	    zfs_prop_to_name(ZFS_PROP_VOLBLOCKSIZE));

	error = dmu_object_claim(os, ZVOL_OBJ, DMU_OT_ZVOL, volblocksize,
	    DMU_OT_NONE, 0, tx);
	ASSERT(error == 0);

	error = zap_create_claim(os, ZVOL_ZAP_OBJ, DMU_OT_ZVOL_PROP,
	    DMU_OT_NONE, 0, tx);
	ASSERT(error == 0);

	error = zap_update(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize, tx);
	ASSERT(error == 0);
}

/*
 * ZFS_IOC_OBJSET_STATS entry point.
 */
int
zvol_get_stats(objset_t *os, nvlist_t *nv)
{
	int error;
	dmu_object_info_t *doi;
	uint64_t val;

	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &val);
	if (error)
		return (SET_ERROR(error));

	dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_VOLSIZE, val);
	doi = kmem_alloc(sizeof (dmu_object_info_t), KM_SLEEP);
	error = dmu_object_info(os, ZVOL_OBJ, doi);

	if (error == 0) {
		dsl_prop_nvlist_add_uint64(nv, ZFS_PROP_VOLBLOCKSIZE,
		    doi->doi_data_block_size);
	}

	kmem_free(doi, sizeof (dmu_object_info_t));

	return (SET_ERROR(error));
}

/*
 * Sanity check volume size.
 */
int
zvol_check_volsize(uint64_t volsize, uint64_t blocksize)
{
	if (volsize == 0)
		return (SET_ERROR(EINVAL));

	if (volsize % blocksize != 0)
		return (SET_ERROR(EINVAL));

#ifdef _ILP32
	if (volsize - 1 > SPEC_MAXOFFSET_T)
		return (SET_ERROR(EOVERFLOW));
#endif
	return (0);
}

/*
 * Ensure the zap is flushed then inform the VFS of the capacity change.
 */
static int
zvol_update_volsize(uint64_t volsize, objset_t *os)
{
	dmu_tx_t *tx;
	int error;
	uint64_t txg;

	tx = dmu_tx_create(os);
	dmu_tx_hold_zap(tx, ZVOL_ZAP_OBJ, TRUE, NULL);
	dmu_tx_mark_netfree(tx);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
		return (SET_ERROR(error));
	}
	txg = dmu_tx_get_txg(tx);

	error = zap_update(os, ZVOL_ZAP_OBJ, "size", 8, 1,
	    &volsize, tx);
	dmu_tx_commit(tx);

	txg_wait_synced(dmu_objset_pool(os), txg);

	if (error == 0)
		error = dmu_free_long_range(os,
		    ZVOL_OBJ, volsize, DMU_OBJECT_END);

	return (error);
}

/*
 * Set ZFS_PROP_VOLSIZE set entry point.  Note that modifying the volume
 * size will result in a udev "change" event being generated.
 */
int
zvol_set_volsize(const char *name, uint64_t volsize)
{
	objset_t *os = NULL;
	uint64_t readonly;
	int error;
	boolean_t owned = B_FALSE;

	error = dsl_prop_get_integer(name,
	    zfs_prop_to_name(ZFS_PROP_READONLY), &readonly, NULL);
	if (error != 0)
		return (SET_ERROR(error));
	if (readonly)
		return (SET_ERROR(EROFS));

	zvol_state_t *zv = zvol_find_by_name(name, RW_READER);

	ASSERT(zv == NULL || (MUTEX_HELD(&zv->zv_state_lock) &&
	    RW_READ_HELD(&zv->zv_suspend_lock)));

	if (zv == NULL || zv->zv_objset == NULL) {
		if (zv != NULL)
			rw_exit(&zv->zv_suspend_lock);
		if ((error = dmu_objset_own(name, DMU_OST_ZVOL, B_FALSE, B_TRUE,
		    FTAG, &os)) != 0) {
			if (zv != NULL)
				mutex_exit(&zv->zv_state_lock);
			return (SET_ERROR(error));
		}
		owned = B_TRUE;
		if (zv != NULL)
			zv->zv_objset = os;
	} else {
		os = zv->zv_objset;
	}

	dmu_object_info_t *doi = kmem_alloc(sizeof (*doi), KM_SLEEP);

	if ((error = dmu_object_info(os, ZVOL_OBJ, doi)) ||
	    (error = zvol_check_volsize(volsize, doi->doi_data_block_size)))
		goto out;

	error = zvol_update_volsize(volsize, os);
	if (error == 0 && zv != NULL) {
		zv->zv_volsize = volsize;
		zv->zv_changed = 1;
	}
out:
	kmem_free(doi, sizeof (dmu_object_info_t));

	if (owned) {
		dmu_objset_disown(os, B_TRUE, FTAG);
		if (zv != NULL)
			zv->zv_objset = NULL;
	} else {
		rw_exit(&zv->zv_suspend_lock);
	}

	if (zv != NULL)
		mutex_exit(&zv->zv_state_lock);

	if (error == 0 && zv != NULL)
		ops->zv_update_volsize(zv, volsize);

	return (SET_ERROR(error));
}

/*
 * Sanity check volume block size.
 */
int
zvol_check_volblocksize(const char *name, uint64_t volblocksize)
{
	/* Record sizes above 128k need the feature to be enabled */
	if (volblocksize > SPA_OLD_MAXBLOCKSIZE) {
		spa_t *spa;
		int error;

		if ((error = spa_open(name, &spa, FTAG)) != 0)
			return (error);

		if (!spa_feature_is_enabled(spa, SPA_FEATURE_LARGE_BLOCKS)) {
			spa_close(spa, FTAG);
			return (SET_ERROR(ENOTSUP));
		}

		/*
		 * We don't allow setting the property above 1MB,
		 * unless the tunable has been changed.
		 */
		if (volblocksize > zfs_max_recordsize)
			return (SET_ERROR(EDOM));

		spa_close(spa, FTAG);
	}

	if (volblocksize < SPA_MINBLOCKSIZE ||
	    volblocksize > SPA_MAXBLOCKSIZE ||
	    !ISP2(volblocksize))
		return (SET_ERROR(EDOM));

	return (0);
}

/*
 * Set ZFS_PROP_VOLBLOCKSIZE set entry point.
 */
int
zvol_set_volblocksize(const char *name, uint64_t volblocksize)
{
	zvol_state_t *zv;
	dmu_tx_t *tx;
	int error;

	zv = zvol_find_by_name(name, RW_READER);

	if (zv == NULL)
		return (SET_ERROR(ENXIO));

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(RW_READ_HELD(&zv->zv_suspend_lock));

	if (zv->zv_flags & ZVOL_RDONLY) {
		mutex_exit(&zv->zv_state_lock);
		rw_exit(&zv->zv_suspend_lock);
		return (SET_ERROR(EROFS));
	}

	tx = dmu_tx_create(zv->zv_objset);
	dmu_tx_hold_bonus(tx, ZVOL_OBJ);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
	} else {
		error = dmu_object_set_blocksize(zv->zv_objset, ZVOL_OBJ,
		    volblocksize, 0, tx);
		if (error == ENOTSUP)
			error = SET_ERROR(EBUSY);
		dmu_tx_commit(tx);
		if (error == 0)
			zv->zv_volblocksize = volblocksize;
	}

	mutex_exit(&zv->zv_state_lock);
	rw_exit(&zv->zv_suspend_lock);

	return (SET_ERROR(error));
}

/*
 * Replay a TX_TRUNCATE ZIL transaction if asked.  TX_TRUNCATE is how we
 * implement DKIOCFREE/free-long-range.
 */
static int
zvol_replay_truncate(void *arg1, void *arg2, boolean_t byteswap)
{
	zvol_state_t *zv = arg1;
	lr_truncate_t *lr = arg2;
	uint64_t offset, length;

	if (byteswap)
		byteswap_uint64_array(lr, sizeof (*lr));

	offset = lr->lr_offset;
	length = lr->lr_length;

	return (dmu_free_long_range(zv->zv_objset, ZVOL_OBJ, offset, length));
}

/*
 * Replay a TX_WRITE ZIL transaction that didn't get committed
 * after a system failure
 */
static int
zvol_replay_write(void *arg1, void *arg2, boolean_t byteswap)
{
	zvol_state_t *zv = arg1;
	lr_write_t *lr = arg2;
	objset_t *os = zv->zv_objset;
	char *data = (char *)(lr + 1);  /* data follows lr_write_t */
	uint64_t offset, length;
	dmu_tx_t *tx;
	int error;

	if (byteswap)
		byteswap_uint64_array(lr, sizeof (*lr));

	offset = lr->lr_offset;
	length = lr->lr_length;

	/* If it's a dmu_sync() block, write the whole block */
	if (lr->lr_common.lrc_reclen == sizeof (lr_write_t)) {
		uint64_t blocksize = BP_GET_LSIZE(&lr->lr_blkptr);
		if (length < blocksize) {
			offset -= offset % blocksize;
			length = blocksize;
		}
	}

	tx = dmu_tx_create(os);
	dmu_tx_hold_write(tx, ZVOL_OBJ, offset, length);
	error = dmu_tx_assign(tx, TXG_WAIT);
	if (error) {
		dmu_tx_abort(tx);
	} else {
		dmu_write(os, ZVOL_OBJ, offset, length, data, tx);
		dmu_tx_commit(tx);
	}

	return (error);
}

static int
zvol_replay_err(void *arg1, void *arg2, boolean_t byteswap)
{
	return (SET_ERROR(ENOTSUP));
}

/*
 * Callback vectors for replaying records.
 * Only TX_WRITE and TX_TRUNCATE are needed for zvol.
 */
zil_replay_func_t *zvol_replay_vector[TX_MAX_TYPE] = {
	zvol_replay_err,	/* no such transaction type */
	zvol_replay_err,	/* TX_CREATE */
	zvol_replay_err,	/* TX_MKDIR */
	zvol_replay_err,	/* TX_MKXATTR */
	zvol_replay_err,	/* TX_SYMLINK */
	zvol_replay_err,	/* TX_REMOVE */
	zvol_replay_err,	/* TX_RMDIR */
	zvol_replay_err,	/* TX_LINK */
	zvol_replay_err,	/* TX_RENAME */
	zvol_replay_write,	/* TX_WRITE */
	zvol_replay_truncate,	/* TX_TRUNCATE */
	zvol_replay_err,	/* TX_SETATTR */
	zvol_replay_err,	/* TX_ACL */
	zvol_replay_err,	/* TX_CREATE_ATTR */
	zvol_replay_err,	/* TX_CREATE_ACL_ATTR */
	zvol_replay_err,	/* TX_MKDIR_ACL */
	zvol_replay_err,	/* TX_MKDIR_ATTR */
	zvol_replay_err,	/* TX_MKDIR_ACL_ATTR */
	zvol_replay_err,	/* TX_WRITE2 */
};

/*
 * zvol_log_write() handles synchronous writes using TX_WRITE ZIL transactions.
 *
 * We store data in the log buffers if it's small enough.
 * Otherwise we will later flush the data out via dmu_sync().
 */
ssize_t zvol_immediate_write_sz = 32768;

void
zvol_log_write(zvol_state_t *zv, dmu_tx_t *tx, uint64_t offset,
    uint64_t size, int sync)
{
	uint32_t blocksize = zv->zv_volblocksize;
	zilog_t *zilog = zv->zv_zilog;
	itx_wr_state_t write_state;

	if (zil_replaying(zilog, tx))
		return;

	if (zilog->zl_logbias == ZFS_LOGBIAS_THROUGHPUT)
		write_state = WR_INDIRECT;
	else if (!spa_has_slogs(zilog->zl_spa) &&
	    size >= blocksize && blocksize > zvol_immediate_write_sz)
		write_state = WR_INDIRECT;
	else if (sync)
		write_state = WR_COPIED;
	else
		write_state = WR_NEED_COPY;

	while (size) {
		itx_t *itx;
		lr_write_t *lr;
		itx_wr_state_t wr_state = write_state;
		ssize_t len = size;

		if (wr_state == WR_COPIED && size > zil_max_copied_data(zilog))
			wr_state = WR_NEED_COPY;
		else if (wr_state == WR_INDIRECT)
			len = MIN(blocksize - P2PHASE(offset, blocksize), size);

		itx = zil_itx_create(TX_WRITE, sizeof (*lr) +
		    (wr_state == WR_COPIED ? len : 0));
		lr = (lr_write_t *)&itx->itx_lr;
		if (wr_state == WR_COPIED && dmu_read_by_dnode(zv->zv_dn,
		    offset, len, lr+1, /* flags */ 0) != 0) {
			zil_itx_destroy(itx);
			itx = zil_itx_create(TX_WRITE, sizeof (*lr));
			lr = (lr_write_t *)&itx->itx_lr;
			wr_state = WR_NEED_COPY;
		}

		itx->itx_wr_state = wr_state;
		lr->lr_foid = ZVOL_OBJ;
		lr->lr_offset = offset;
		lr->lr_length = len;
		lr->lr_blkoff = 0;
		BP_ZERO(&lr->lr_blkptr);

		itx->itx_private = zv;
		itx->itx_sync = sync;

		(void) zil_itx_assign(zilog, itx, tx);

		offset += len;
		size -= len;
	}
}

/*
 * Log a DKIOCFREE/free-long-range to the ZIL with TX_TRUNCATE.
 */
void
zvol_log_truncate(zvol_state_t *zv, dmu_tx_t *tx, uint64_t off, uint64_t len,
    boolean_t sync)
{
	itx_t *itx;
	lr_truncate_t *lr;
	zilog_t *zilog = zv->zv_zilog;

	if (zil_replaying(zilog, tx))
		return;

	itx = zil_itx_create(TX_TRUNCATE, sizeof (*lr));
	lr = (lr_truncate_t *)&itx->itx_lr;
	lr->lr_foid = ZVOL_OBJ;
	lr->lr_offset = off;
	lr->lr_length = len;

	itx->itx_sync = sync;
	zil_itx_assign(zilog, itx, tx);
}


/* ARGSUSED */
static void
zvol_get_done(zgd_t *zgd, int error)
{
	if (zgd->zgd_db)
		dmu_buf_rele(zgd->zgd_db, zgd);

	zfs_rangelock_exit(zgd->zgd_lr);

	kmem_free(zgd, sizeof (zgd_t));
}

/*
 * Get data to generate a TX_WRITE intent log record.
 */
int
zvol_get_data(void *arg, lr_write_t *lr, char *buf, struct lwb *lwb, zio_t *zio)
{
	zvol_state_t *zv = arg;
	uint64_t offset = lr->lr_offset;
	uint64_t size = lr->lr_length;
	dmu_buf_t *db;
	zgd_t *zgd;
	int error;

	ASSERT3P(lwb, !=, NULL);
	ASSERT3P(zio, !=, NULL);
	ASSERT3U(size, !=, 0);

	zgd = (zgd_t *)kmem_zalloc(sizeof (zgd_t), KM_SLEEP);
	zgd->zgd_lwb = lwb;

	/*
	 * Write records come in two flavors: immediate and indirect.
	 * For small writes it's cheaper to store the data with the
	 * log record (immediate); for large writes it's cheaper to
	 * sync the data and get a pointer to it (indirect) so that
	 * we don't have to write the data twice.
	 */
	if (buf != NULL) { /* immediate write */
		zgd->zgd_lr = zfs_rangelock_enter(&zv->zv_rangelock, offset,
		    size, RL_READER);
		error = dmu_read_by_dnode(zv->zv_dn, offset, size, buf,
		    /* flags */ 0);
	} else { /* indirect write */
		/*
		 * Have to lock the whole block to ensure when it's written out
		 * and its checksum is being calculated that no one can change
		 * the data. Contrarily to zfs_get_data we need not re-check
		 * blocksize after we get the lock because it cannot be changed.
		 */
		size = zv->zv_volblocksize;
		offset = P2ALIGN_TYPED(offset, size, uint64_t);
		zgd->zgd_lr = zfs_rangelock_enter(&zv->zv_rangelock, offset,
		    size, RL_READER);
		error = dmu_buf_hold_by_dnode(zv->zv_dn, offset, zgd, &db,
		    /* flags */ 0);
		if (error == 0) {
			blkptr_t *bp = &lr->lr_blkptr;

			zgd->zgd_db = db;
			zgd->zgd_bp = bp;

			ASSERT(db != NULL);
			ASSERT(db->db_offset == offset);
			ASSERT(db->db_size == size);

			error = dmu_sync(zio, lr->lr_common.lrc_txg,
			    zvol_get_done, zgd);

			if (error == 0)
				return (0);
		}
	}

	zvol_get_done(zgd, error);

	return (SET_ERROR(error));
}

/*
 * The zvol_state_t's are inserted into zvol_state_list and zvol_htable.
 */

void
zvol_insert(zvol_state_t *zv)
{
	ASSERT(RW_WRITE_HELD(&zvol_state_lock));
	list_insert_head(&zvol_state_list, zv);
	hlist_add_head(&zv->zv_hlink, ZVOL_HT_HEAD(zv->zv_hash));
}

/*
 * Simply remove the zvol from to list of zvols.
 */
static void
zvol_remove(zvol_state_t *zv)
{
	ASSERT(RW_WRITE_HELD(&zvol_state_lock));
	list_remove(&zvol_state_list, zv);
	hlist_del(&zv->zv_hlink);
}

/*
 * Setup zv after we just own the zv->objset
 */
static int
zvol_setup_zv(zvol_state_t *zv)
{
	uint64_t volsize;
	int error;
	uint64_t ro;
	objset_t *os = zv->zv_objset;

	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(RW_LOCK_HELD(&zv->zv_suspend_lock));

	zv->zv_zilog = NULL;
	zv->zv_flags &= ~ZVOL_WRITTEN_TO;

	error = dsl_prop_get_integer(zv->zv_name, "readonly", &ro, NULL);
	if (error)
		return (SET_ERROR(error));

	error = zap_lookup(os, ZVOL_ZAP_OBJ, "size", 8, 1, &volsize);
	if (error)
		return (SET_ERROR(error));

	error = dnode_hold(os, ZVOL_OBJ, zv, &zv->zv_dn);
	if (error)
		return (SET_ERROR(error));

	ops->zv_set_capacity(zv, volsize >> 9);
	zv->zv_volsize = volsize;

	if (ro || dmu_objset_is_snapshot(os) ||
	    !spa_writeable(dmu_objset_spa(os))) {
		ops->zv_set_disk_ro(zv, 1);
		zv->zv_flags |= ZVOL_RDONLY;
	} else {
		ops->zv_set_disk_ro(zv, 0);
		zv->zv_flags &= ~ZVOL_RDONLY;
	}
	return (0);
}

/*
 * Shutdown every zv_objset related stuff except zv_objset itself.
 * The is the reverse of zvol_setup_zv.
 */
static void
zvol_shutdown_zv(zvol_state_t *zv)
{
	ASSERT(MUTEX_HELD(&zv->zv_state_lock) &&
	    RW_LOCK_HELD(&zv->zv_suspend_lock));

	if (zv->zv_flags & ZVOL_WRITTEN_TO) {
		ASSERT(zv->zv_zilog != NULL);
		zil_close(zv->zv_zilog);
	}

	zv->zv_zilog = NULL;

	dnode_rele(zv->zv_dn, zv);
	zv->zv_dn = NULL;

	/*
	 * Evict cached data. We must write out any dirty data before
	 * disowning the dataset.
	 */
	if (zv->zv_flags & ZVOL_WRITTEN_TO)
		txg_wait_synced(dmu_objset_pool(zv->zv_objset), 0);
	(void) dmu_objset_evict_dbufs(zv->zv_objset);
}

/*
 * return the proper tag for rollback and recv
 */
void *
zvol_tag(zvol_state_t *zv)
{
	ASSERT(RW_WRITE_HELD(&zv->zv_suspend_lock));
	return (zv->zv_open_count > 0 ? zv : NULL);
}

/*
 * Suspend the zvol for recv and rollback.
 */
zvol_state_t *
zvol_suspend(const char *name)
{
	zvol_state_t *zv;

	zv = zvol_find_by_name(name, RW_WRITER);

	if (zv == NULL)
		return (NULL);

	/* block all I/O, release in zvol_resume. */
	ASSERT(MUTEX_HELD(&zv->zv_state_lock));
	ASSERT(RW_WRITE_HELD(&zv->zv_suspend_lock));


	ASSERT(atomic_read(&zv->zv_suspend_ref) >= 0);
	atomic_inc(&zv->zv_suspend_ref);

	if (zv->zv_open_count > 0)
		zvol_shutdown_zv(zv);

	/*
	 * do not hold zv_state_lock across suspend/resume to
	 * avoid locking up zvol lookups
	 */
	mutex_exit(&zv->zv_state_lock);

	/* zv_suspend_lock is released in zvol_resume() */
	return (zv);
}

int
zvol_resume(zvol_state_t *zv)
{
	int error = 0;

	ASSERT(RW_WRITE_HELD(&zv->zv_suspend_lock));

	mutex_enter(&zv->zv_state_lock);

	if (zv->zv_open_count > 0) {
		VERIFY0(dmu_objset_hold(zv->zv_name, zv, &zv->zv_objset));
		VERIFY3P(zv->zv_objset->os_dsl_dataset->ds_owner, ==, zv);
		VERIFY(dsl_dataset_long_held(zv->zv_objset->os_dsl_dataset));
		dmu_objset_rele(zv->zv_objset, zv);

		error = zvol_setup_zv(zv);
	}

	mutex_exit(&zv->zv_state_lock);

	rw_exit(&zv->zv_suspend_lock);
	/*
	 * We need this because we don't hold zvol_state_lock while releasing
	 * zv_suspend_lock. zvol_remove_minors_impl thus cannot check
	 * zv_suspend_lock to determine it is safe to free because rwlock is
	 * not inherent atomic.
	 */
	ASSERT(atomic_read(&zv->zv_suspend_ref) > 0);
	atomic_dec(&zv->zv_suspend_ref);

	return (SET_ERROR(error));
}

int
zvol_first_open(zvol_state_t *zv, boolean_t readonly)
{
	objset_t *os;
	int error, locked = 0;
	boolean_t ro;

	ASSERT(RW_READ_HELD(&zv->zv_suspend_lock));
	ASSERT(MUTEX_HELD(&zv->zv_state_lock));

	/*
	 * In all other cases the spa_namespace_lock is taken before the
	 * bdev->bd_mutex lock.	 But in this case the Linux __blkdev_get()
	 * function calls fops->open() with the bdev->bd_mutex lock held.
	 * This deadlock can be easily observed with zvols used as vdevs.
	 *
	 * To avoid a potential lock inversion deadlock we preemptively
	 * try to take the spa_namespace_lock().  Normally it will not
	 * be contended and this is safe because spa_open_common() handles
	 * the case where the caller already holds the spa_namespace_lock.
	 *
	 * When it is contended we risk a lock inversion if we were to
	 * block waiting for the lock.	Luckily, the __blkdev_get()
	 * function allows us to return -ERESTARTSYS which will result in
	 * bdev->bd_mutex being dropped, reacquired, and fops->open() being
	 * called again.  This process can be repeated safely until both
	 * locks are acquired.
	 */
	if (!mutex_owned(&spa_namespace_lock)) {
		locked = mutex_tryenter(&spa_namespace_lock);
		if (!locked)
			return (SET_ERROR(EINTR));
	}

	ro = (readonly || (strchr(zv->zv_name, '@') != NULL));
	error = dmu_objset_own(zv->zv_name, DMU_OST_ZVOL, ro, B_TRUE, zv, &os);
	if (error)
		goto out_mutex;

	zv->zv_objset = os;

	error = zvol_setup_zv(zv);

	if (error) {
		dmu_objset_disown(os, 1, zv);
		zv->zv_objset = NULL;
	}

out_mutex:
	if (locked)
		mutex_exit(&spa_namespace_lock);
	return (SET_ERROR(error));
}

void
zvol_last_close(zvol_state_t *zv)
{
	ASSERT(RW_READ_HELD(&zv->zv_suspend_lock));
	ASSERT(MUTEX_HELD(&zv->zv_state_lock));

	zvol_shutdown_zv(zv);

	dmu_objset_disown(zv->zv_objset, 1, zv);
	zv->zv_objset = NULL;
}

typedef struct minors_job {
	list_t *list;
	list_node_t link;
	/* input */
	char *name;
	/* output */
	int error;
} minors_job_t;

/*
 * Prefetch zvol dnodes for the minors_job
 */
static void
zvol_prefetch_minors_impl(void *arg)
{
	minors_job_t *job = arg;
	char *dsname = job->name;
	objset_t *os = NULL;

	job->error = dmu_objset_own(dsname, DMU_OST_ZVOL, B_TRUE, B_TRUE,
	    FTAG, &os);
	if (job->error == 0) {
		dmu_prefetch(os, ZVOL_OBJ, 0, 0, 0, ZIO_PRIORITY_SYNC_READ);
		dmu_objset_disown(os, B_TRUE, FTAG);
	}
}

/*
 * Mask errors to continue dmu_objset_find() traversal
 */
static int
zvol_create_snap_minor_cb(const char *dsname, void *arg)
{
	minors_job_t *j = arg;
	list_t *minors_list = j->list;
	const char *name = j->name;

	ASSERT0(MUTEX_HELD(&spa_namespace_lock));

	/* skip the designated dataset */
	if (name && strcmp(dsname, name) == 0)
		return (0);

	/* at this point, the dsname should name a snapshot */
	if (strchr(dsname, '@') == 0) {
		dprintf("zvol_create_snap_minor_cb(): "
		    "%s is not a snapshot name\n", dsname);
	} else {
		minors_job_t *job;
		char *n = kmem_strdup(dsname);
		if (n == NULL)
			return (0);

		job = kmem_alloc(sizeof (minors_job_t), KM_SLEEP);
		job->name = n;
		job->list = minors_list;
		job->error = 0;
		list_insert_tail(minors_list, job);
		/* don't care if dispatch fails, because job->error is 0 */
		taskq_dispatch(system_taskq, zvol_prefetch_minors_impl, job,
		    TQ_SLEEP);
	}

	return (0);
}

/*
 * Mask errors to continue dmu_objset_find() traversal
 */
static int
zvol_create_minors_cb(const char *dsname, void *arg)
{
	uint64_t snapdev;
	int error;
	list_t *minors_list = arg;

	ASSERT0(MUTEX_HELD(&spa_namespace_lock));

	error = dsl_prop_get_integer(dsname, "snapdev", &snapdev, NULL);
	if (error)
		return (0);

	/*
	 * Given the name and the 'snapdev' property, create device minor nodes
	 * with the linkages to zvols/snapshots as needed.
	 * If the name represents a zvol, create a minor node for the zvol, then
	 * check if its snapshots are 'visible', and if so, iterate over the
	 * snapshots and create device minor nodes for those.
	 */
	if (strchr(dsname, '@') == 0) {
		minors_job_t *job;
		char *n = kmem_strdup(dsname);
		if (n == NULL)
			return (0);

		job = kmem_alloc(sizeof (minors_job_t), KM_SLEEP);
		job->name = n;
		job->list = minors_list;
		job->error = 0;
		list_insert_tail(minors_list, job);
		/* don't care if dispatch fails, because job->error is 0 */
		taskq_dispatch(system_taskq, zvol_prefetch_minors_impl, job,
		    TQ_SLEEP);

		if (snapdev == ZFS_SNAPDEV_VISIBLE) {
			/*
			 * traverse snapshots only, do not traverse children,
			 * and skip the 'dsname'
			 */
			error = dmu_objset_find(dsname,
			    zvol_create_snap_minor_cb, (void *)job,
			    DS_FIND_SNAPSHOTS);
		}
	} else {
		dprintf("zvol_create_minors_cb(): %s is not a zvol name\n",
		    dsname);
	}

	return (0);
}

/*
 * Create minors for the specified dataset, including children and snapshots.
 * Pay attention to the 'snapdev' property and iterate over the snapshots
 * only if they are 'visible'. This approach allows one to assure that the
 * snapshot metadata is read from disk only if it is needed.
 *
 * The name can represent a dataset to be recursively scanned for zvols and
 * their snapshots, or a single zvol snapshot. If the name represents a
 * dataset, the scan is performed in two nested stages:
 * - scan the dataset for zvols, and
 * - for each zvol, create a minor node, then check if the zvol's snapshots
 *   are 'visible', and only then iterate over the snapshots if needed
 *
 * If the name represents a snapshot, a check is performed if the snapshot is
 * 'visible' (which also verifies that the parent is a zvol), and if so,
 * a minor node for that snapshot is created.
 */
void
zvol_create_minors_recursive(const char *name)
{
	list_t minors_list;
	minors_job_t *job;

	if (zvol_inhibit_dev)
		return;

	/*
	 * This is the list for prefetch jobs. Whenever we found a match
	 * during dmu_objset_find, we insert a minors_job to the list and do
	 * taskq_dispatch to parallel prefetch zvol dnodes. Note we don't need
	 * any lock because all list operation is done on the current thread.
	 *
	 * We will use this list to do zvol_create_minor_impl after prefetch
	 * so we don't have to traverse using dmu_objset_find again.
	 */
	list_create(&minors_list, sizeof (minors_job_t),
	    offsetof(minors_job_t, link));


	if (strchr(name, '@') != NULL) {
		uint64_t snapdev;

		int error = dsl_prop_get_integer(name, "snapdev",
		    &snapdev, NULL);

		if (error == 0 && snapdev == ZFS_SNAPDEV_VISIBLE)
			(void) ops->zv_create_minor(name);
	} else {
		fstrans_cookie_t cookie = spl_fstrans_mark();
		(void) dmu_objset_find(name, zvol_create_minors_cb,
		    &minors_list, DS_FIND_CHILDREN);
		spl_fstrans_unmark(cookie);
	}

	taskq_wait_outstanding(system_taskq, 0);

	/*
	 * Prefetch is completed, we can do zvol_create_minor_impl
	 * sequentially.
	 */
	while ((job = list_head(&minors_list)) != NULL) {
		list_remove(&minors_list, job);
		if (!job->error)
			(void) ops->zv_create_minor(job->name);
		kmem_strfree(job->name);
		kmem_free(job, sizeof (minors_job_t));
	}

	list_destroy(&minors_list);
}

void
zvol_create_minor(const char *name)
{
	/*
	 * Note: the dsl_pool_config_lock must not be held.
	 * Minor node creation needs to obtain the zvol_state_lock.
	 * zvol_open() obtains the zvol_state_lock and then the dsl pool
	 * config lock.  Therefore, we can't have the config lock now if
	 * we are going to wait for the zvol_state_lock, because it
	 * would be a lock order inversion which could lead to deadlock.
	 */

	if (zvol_inhibit_dev)
		return;

	if (strchr(name, '@') != NULL) {
		uint64_t snapdev;

		int error = dsl_prop_get_integer(name,
		    "snapdev", &snapdev, NULL);

		if (error == 0 && snapdev == ZFS_SNAPDEV_VISIBLE)
			(void) ops->zv_create_minor(name);
	} else {
		(void) ops->zv_create_minor(name);
	}
}

/*
 * Remove minors for specified dataset including children and snapshots.
 */

void
zvol_remove_minors_impl(const char *name)
{
	zvol_state_t *zv, *zv_next;
	int namelen = ((name) ? strlen(name) : 0);
	taskqid_t t;
	list_t free_list;

	if (zvol_inhibit_dev)
		return;

	list_create(&free_list, sizeof (zvol_state_t),
	    offsetof(zvol_state_t, zv_next));

	rw_enter(&zvol_state_lock, RW_WRITER);

	for (zv = list_head(&zvol_state_list); zv != NULL; zv = zv_next) {
		zv_next = list_next(&zvol_state_list, zv);

		mutex_enter(&zv->zv_state_lock);
		if (name == NULL || strcmp(zv->zv_name, name) == 0 ||
		    (strncmp(zv->zv_name, name, namelen) == 0 &&
		    (zv->zv_name[namelen] == '/' ||
		    zv->zv_name[namelen] == '@'))) {
			/*
			 * By holding zv_state_lock here, we guarantee that no
			 * one is currently using this zv
			 */

			/* If in use, leave alone */
			if (zv->zv_open_count > 0 ||
			    atomic_read(&zv->zv_suspend_ref)) {
				mutex_exit(&zv->zv_state_lock);
				continue;
			}

			zvol_remove(zv);

			/*
			 * Cleared while holding zvol_state_lock as a writer
			 * which will prevent zvol_open() from opening it.
			 */
			ops->zv_clear_private(zv);

			/* Drop zv_state_lock before zvol_free() */
			mutex_exit(&zv->zv_state_lock);

			/* Try parallel zv_free, if failed do it in place */
			t = taskq_dispatch(system_taskq,
			    (task_func_t *)ops->zv_free, zv, TQ_SLEEP);
			if (t == TASKQID_INVALID)
				list_insert_head(&free_list, zv);
		} else {
			mutex_exit(&zv->zv_state_lock);
		}
	}
	rw_exit(&zvol_state_lock);

	/* Drop zvol_state_lock before calling zvol_free() */
	while ((zv = list_head(&free_list)) != NULL) {
		list_remove(&free_list, zv);
		ops->zv_free(zv);
	}
}

/* Remove minor for this specific volume only */
static void
zvol_remove_minor_impl(const char *name)
{
	zvol_state_t *zv = NULL, *zv_next;

	if (zvol_inhibit_dev)
		return;

	rw_enter(&zvol_state_lock, RW_WRITER);

	for (zv = list_head(&zvol_state_list); zv != NULL; zv = zv_next) {
		zv_next = list_next(&zvol_state_list, zv);

		mutex_enter(&zv->zv_state_lock);
		if (strcmp(zv->zv_name, name) == 0) {
			/*
			 * By holding zv_state_lock here, we guarantee that no
			 * one is currently using this zv
			 */

			/* If in use, leave alone */
			if (zv->zv_open_count > 0 ||
			    atomic_read(&zv->zv_suspend_ref)) {
				mutex_exit(&zv->zv_state_lock);
				continue;
			}
			zvol_remove(zv);

			ops->zv_clear_private(zv);
			mutex_exit(&zv->zv_state_lock);
			break;
		} else {
			mutex_exit(&zv->zv_state_lock);
		}
	}

	/* Drop zvol_state_lock before calling zvol_free() */
	rw_exit(&zvol_state_lock);

	if (zv != NULL)
		ops->zv_free(zv);
}

/*
 * Rename minors for specified dataset including children and snapshots.
 */
static void
zvol_rename_minors_impl(const char *oldname, const char *newname)
{
	zvol_state_t *zv, *zv_next;
	int oldnamelen, newnamelen;

	if (zvol_inhibit_dev)
		return;

	oldnamelen = strlen(oldname);
	newnamelen = strlen(newname);

	rw_enter(&zvol_state_lock, RW_READER);

	for (zv = list_head(&zvol_state_list); zv != NULL; zv = zv_next) {
		zv_next = list_next(&zvol_state_list, zv);

		mutex_enter(&zv->zv_state_lock);

		if (strcmp(zv->zv_name, oldname) == 0) {
			ops->zv_rename_minor(zv, newname);
		} else if (strncmp(zv->zv_name, oldname, oldnamelen) == 0 &&
		    (zv->zv_name[oldnamelen] == '/' ||
		    zv->zv_name[oldnamelen] == '@')) {
			char *name = kmem_asprintf("%s%c%s", newname,
			    zv->zv_name[oldnamelen],
			    zv->zv_name + oldnamelen + 1);
			ops->zv_rename_minor(zv, name);
			kmem_strfree(name);
		}

		mutex_exit(&zv->zv_state_lock);
	}

	rw_exit(&zvol_state_lock);
}

typedef struct zvol_snapdev_cb_arg {
	uint64_t snapdev;
} zvol_snapdev_cb_arg_t;

static int
zvol_set_snapdev_cb(const char *dsname, void *param)
{
	zvol_snapdev_cb_arg_t *arg = param;

	if (strchr(dsname, '@') == NULL)
		return (0);

	switch (arg->snapdev) {
		case ZFS_SNAPDEV_VISIBLE:
			(void) ops->zv_create_minor(dsname);
			break;
		case ZFS_SNAPDEV_HIDDEN:
			(void) zvol_remove_minor_impl(dsname);
			break;
	}

	return (0);
}

static void
zvol_set_snapdev_impl(char *name, uint64_t snapdev)
{
	zvol_snapdev_cb_arg_t arg = {snapdev};
	fstrans_cookie_t cookie = spl_fstrans_mark();
	/*
	 * The zvol_set_snapdev_sync() sets snapdev appropriately
	 * in the dataset hierarchy. Here, we only scan snapshots.
	 */
	dmu_objset_find(name, zvol_set_snapdev_cb, &arg, DS_FIND_SNAPSHOTS);
	spl_fstrans_unmark(cookie);
}

typedef struct zvol_volmode_cb_arg {
	uint64_t volmode;
} zvol_volmode_cb_arg_t;

static void
zvol_set_volmode_impl(char *name, uint64_t volmode)
{
	fstrans_cookie_t cookie = spl_fstrans_mark();

	if (strchr(name, '@') != NULL)
		return;

	/*
	 * It's unfortunate we need to remove minors before we create new ones:
	 * this is necessary because our backing gendisk (zvol_state->zv_disk)
	 * could be different when we set, for instance, volmode from "geom"
	 * to "dev" (or vice versa).
	 * A possible optimization is to modify our consumers so we don't get
	 * called when "volmode" does not change.
	 */
	switch (volmode) {
		case ZFS_VOLMODE_NONE:
			(void) zvol_remove_minor_impl(name);
			break;
		case ZFS_VOLMODE_GEOM:
		case ZFS_VOLMODE_DEV:
			(void) zvol_remove_minor_impl(name);
			(void) ops->zv_create_minor(name);
			break;
		case ZFS_VOLMODE_DEFAULT:
			(void) zvol_remove_minor_impl(name);
			if (zvol_volmode == ZFS_VOLMODE_NONE)
				break;
			else /* if zvol_volmode is invalid defaults to "geom" */
				(void) ops->zv_create_minor(name);
			break;
	}

	spl_fstrans_unmark(cookie);
}

static zvol_task_t *
zvol_task_alloc(zvol_async_op_t op, const char *name1, const char *name2,
    uint64_t value)
{
	zvol_task_t *task;
	char *delim;

	/* Never allow tasks on hidden names. */
	if (name1[0] == '$')
		return (NULL);

	task = kmem_zalloc(sizeof (zvol_task_t), KM_SLEEP);
	task->op = op;
	task->value = value;
	delim = strchr(name1, '/');
	strlcpy(task->pool, name1, delim ? (delim - name1 + 1) : MAXNAMELEN);

	strlcpy(task->name1, name1, MAXNAMELEN);
	if (name2 != NULL)
		strlcpy(task->name2, name2, MAXNAMELEN);

	return (task);
}

static void
zvol_task_free(zvol_task_t *task)
{
	kmem_free(task, sizeof (zvol_task_t));
}

/*
 * The worker thread function performed asynchronously.
 */
static void
zvol_task_cb(void *arg)
{
	zvol_task_t *task = arg;

	switch (task->op) {
	case ZVOL_ASYNC_REMOVE_MINORS:
		zvol_remove_minors_impl(task->name1);
		break;
	case ZVOL_ASYNC_RENAME_MINORS:
		zvol_rename_minors_impl(task->name1, task->name2);
		break;
	case ZVOL_ASYNC_SET_SNAPDEV:
		zvol_set_snapdev_impl(task->name1, task->value);
		break;
	case ZVOL_ASYNC_SET_VOLMODE:
		zvol_set_volmode_impl(task->name1, task->value);
		break;
	default:
		VERIFY(0);
		break;
	}

	zvol_task_free(task);
}

typedef struct zvol_set_prop_int_arg {
	const char *zsda_name;
	uint64_t zsda_value;
	zprop_source_t zsda_source;
	dmu_tx_t *zsda_tx;
} zvol_set_prop_int_arg_t;

/*
 * Sanity check the dataset for safe use by the sync task.  No additional
 * conditions are imposed.
 */
static int
zvol_set_snapdev_check(void *arg, dmu_tx_t *tx)
{
	zvol_set_prop_int_arg_t *zsda = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dir_t *dd;
	int error;

	error = dsl_dir_hold(dp, zsda->zsda_name, FTAG, &dd, NULL);
	if (error != 0)
		return (error);

	dsl_dir_rele(dd, FTAG);

	return (error);
}

/* ARGSUSED */
static int
zvol_set_snapdev_sync_cb(dsl_pool_t *dp, dsl_dataset_t *ds, void *arg)
{
	char dsname[MAXNAMELEN];
	zvol_task_t *task;
	uint64_t snapdev;

	dsl_dataset_name(ds, dsname);
	if (dsl_prop_get_int_ds(ds, "snapdev", &snapdev) != 0)
		return (0);
	task = zvol_task_alloc(ZVOL_ASYNC_SET_SNAPDEV, dsname, NULL, snapdev);
	if (task == NULL)
		return (0);

	(void) taskq_dispatch(dp->dp_spa->spa_zvol_taskq, zvol_task_cb,
	    task, TQ_SLEEP);
	return (0);
}

/*
 * Traverse all child datasets and apply snapdev appropriately.
 * We call dsl_prop_set_sync_impl() here to set the value only on the toplevel
 * dataset and read the effective "snapdev" on every child in the callback
 * function: this is because the value is not guaranteed to be the same in the
 * whole dataset hierarchy.
 */
static void
zvol_set_snapdev_sync(void *arg, dmu_tx_t *tx)
{
	zvol_set_prop_int_arg_t *zsda = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dir_t *dd;
	dsl_dataset_t *ds;
	int error;

	VERIFY0(dsl_dir_hold(dp, zsda->zsda_name, FTAG, &dd, NULL));
	zsda->zsda_tx = tx;

	error = dsl_dataset_hold(dp, zsda->zsda_name, FTAG, &ds);
	if (error == 0) {
		dsl_prop_set_sync_impl(ds, zfs_prop_to_name(ZFS_PROP_SNAPDEV),
		    zsda->zsda_source, sizeof (zsda->zsda_value), 1,
		    &zsda->zsda_value, zsda->zsda_tx);
		dsl_dataset_rele(ds, FTAG);
	}
	dmu_objset_find_dp(dp, dd->dd_object, zvol_set_snapdev_sync_cb,
	    zsda, DS_FIND_CHILDREN);

	dsl_dir_rele(dd, FTAG);
}

int
zvol_set_snapdev(const char *ddname, zprop_source_t source, uint64_t snapdev)
{
	zvol_set_prop_int_arg_t zsda;

	zsda.zsda_name = ddname;
	zsda.zsda_source = source;
	zsda.zsda_value = snapdev;

	return (dsl_sync_task(ddname, zvol_set_snapdev_check,
	    zvol_set_snapdev_sync, &zsda, 0, ZFS_SPACE_CHECK_NONE));
}

/*
 * Sanity check the dataset for safe use by the sync task.  No additional
 * conditions are imposed.
 */
static int
zvol_set_volmode_check(void *arg, dmu_tx_t *tx)
{
	zvol_set_prop_int_arg_t *zsda = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dir_t *dd;
	int error;

	error = dsl_dir_hold(dp, zsda->zsda_name, FTAG, &dd, NULL);
	if (error != 0)
		return (error);

	dsl_dir_rele(dd, FTAG);

	return (error);
}

/* ARGSUSED */
static int
zvol_set_volmode_sync_cb(dsl_pool_t *dp, dsl_dataset_t *ds, void *arg)
{
	char dsname[MAXNAMELEN];
	zvol_task_t *task;
	uint64_t volmode;

	dsl_dataset_name(ds, dsname);
	if (dsl_prop_get_int_ds(ds, "volmode", &volmode) != 0)
		return (0);
	task = zvol_task_alloc(ZVOL_ASYNC_SET_VOLMODE, dsname, NULL, volmode);
	if (task == NULL)
		return (0);

	(void) taskq_dispatch(dp->dp_spa->spa_zvol_taskq, zvol_task_cb,
	    task, TQ_SLEEP);
	return (0);
}

/*
 * Traverse all child datasets and apply volmode appropriately.
 * We call dsl_prop_set_sync_impl() here to set the value only on the toplevel
 * dataset and read the effective "volmode" on every child in the callback
 * function: this is because the value is not guaranteed to be the same in the
 * whole dataset hierarchy.
 */
static void
zvol_set_volmode_sync(void *arg, dmu_tx_t *tx)
{
	zvol_set_prop_int_arg_t *zsda = arg;
	dsl_pool_t *dp = dmu_tx_pool(tx);
	dsl_dir_t *dd;
	dsl_dataset_t *ds;
	int error;

	VERIFY0(dsl_dir_hold(dp, zsda->zsda_name, FTAG, &dd, NULL));
	zsda->zsda_tx = tx;

	error = dsl_dataset_hold(dp, zsda->zsda_name, FTAG, &ds);
	if (error == 0) {
		dsl_prop_set_sync_impl(ds, zfs_prop_to_name(ZFS_PROP_VOLMODE),
		    zsda->zsda_source, sizeof (zsda->zsda_value), 1,
		    &zsda->zsda_value, zsda->zsda_tx);
		dsl_dataset_rele(ds, FTAG);
	}

	dmu_objset_find_dp(dp, dd->dd_object, zvol_set_volmode_sync_cb,
	    zsda, DS_FIND_CHILDREN);

	dsl_dir_rele(dd, FTAG);
}

int
zvol_set_volmode(const char *ddname, zprop_source_t source, uint64_t volmode)
{
	zvol_set_prop_int_arg_t zsda;

	zsda.zsda_name = ddname;
	zsda.zsda_source = source;
	zsda.zsda_value = volmode;

	return (dsl_sync_task(ddname, zvol_set_volmode_check,
	    zvol_set_volmode_sync, &zsda, 0, ZFS_SPACE_CHECK_NONE));
}

void
zvol_remove_minors(spa_t *spa, const char *name, boolean_t async)
{
	zvol_task_t *task;
	taskqid_t id;

	task = zvol_task_alloc(ZVOL_ASYNC_REMOVE_MINORS, name, NULL, ~0ULL);
	if (task == NULL)
		return;

	id = taskq_dispatch(spa->spa_zvol_taskq, zvol_task_cb, task, TQ_SLEEP);
	if ((async == B_FALSE) && (id != TASKQID_INVALID))
		taskq_wait_id(spa->spa_zvol_taskq, id);
}

void
zvol_rename_minors(spa_t *spa, const char *name1, const char *name2,
    boolean_t async)
{
	zvol_task_t *task;
	taskqid_t id;

	task = zvol_task_alloc(ZVOL_ASYNC_RENAME_MINORS, name1, name2, ~0ULL);
	if (task == NULL)
		return;

	id = taskq_dispatch(spa->spa_zvol_taskq, zvol_task_cb, task, TQ_SLEEP);
	if ((async == B_FALSE) && (id != TASKQID_INVALID))
		taskq_wait_id(spa->spa_zvol_taskq, id);
}

boolean_t
zvol_is_zvol(const char *name)
{

	return (ops->zv_is_zvol(name));
}

void
zvol_register_ops(const zvol_platform_ops_t *zvol_ops)
{
	ops = zvol_ops;
}

static void
zvol_dmu_buf_set_transfer_write(dmu_buf_set_t *dbs)
{
	zvol_dmu_state_t *zds = (zvol_dmu_state_t *)dbs->dbs_dc;
	zvol_state_t *zv = zds->zds_zv;
	dmu_tx_t *tx = dmu_buf_set_tx(dbs);

	dmu_buf_set_transfer(dbs);

	/* Log this write. */
	if (zds->zds_sync)
		zvol_log_write(zv, tx, dbs->dbs_dn_start, dbs->dbs_size,
		    zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS);
	dmu_tx_commit(tx);
}

static void
zvol_dmu_ctx_init_wrapper(dmu_buf_ctx_t *ctx, int err)
{
	zvol_dmu_state_t *zds = (zvol_dmu_state_t *)ctx;

	zvol_dmu_ctx_init(zds);
}

static void
zvol_dmu_ctx_init_deferred(zvol_state_t *zv)
{
	zvol_dmu_state_t *zds;

	ASSERT(tsd_get(zfs_async_io_key) != NULL);
	mutex_enter(&zv->zv_state_lock);
	if ((zds = list_remove_head(&zv->zv_deferred)) != NULL) {
		DEBUG_REFCOUNT_DEC(dmu_ctx_deferred);
		zds->zds_retry = B_TRUE;
	} else {
		DEBUG_REFCOUNT_DEC(dmu_ctx_active);
		zv->zv_active--;
	}
	ASSERT(zv->zv_active >= 0);
	mutex_exit(&zv->zv_state_lock);
	if (zds == NULL)
		return;

	ASSERT(zds->zds_dc.dc_buf_ctx.dbc_flags & DMU_CTX_FLAG_ASYNC);
	dmu_thread_context_dispatch(&zds->zds_dc.dc_buf_ctx, 0,
	    zvol_dmu_ctx_init_wrapper);
}

static void
zvol_dmu_err(zvol_dmu_state_t *zds_, dmu_ctx_cb_t err_cb)
{
	zvol_dmu_state_t *zds = zds_;
	zvol_state_t *zv = zds->zds_zv;

	err_cb(&zds->zds_dc);
	zvol_dmu_ctx_init_deferred(zv);
}

typedef struct {
	dmu_tx_buf_set_t zdps_dtbs;
	zvol_dmu_state_t *zdps_zds;
	dmu_ctx_cb_t zdps_err_cb;
	boolean_t zdps_prefault_done;
} zvol_dmu_prefault_state_t;

static void
zvol_dmu_ctx_init_write_impl(dmu_tx_buf_set_t *dtbs)
{
	zvol_dmu_prefault_state_t *zdps;
	dnode_t *dn;
	zvol_dmu_state_t *zds;
	zvol_state_t *zv;
	dmu_ctx_cb_t err_cb;
	uint64_t off, io_size;
	dmu_tx_t	*tx;
	int count, err;

	zdps = (zvol_dmu_prefault_state_t *)dtbs;
	zds = zdps->zdps_zds;
	err = dtbs->dtbs_err;
	err_cb = zdps->zdps_err_cb;
	off = zds->zds_off;
	io_size = zds->zds_io_size;
	zv = zds->zds_zv;
	dn = zv->zv_dn;

	if (!zdps->zdps_prefault_done) {
		DEBUG_REFCOUNT_ADD(dmu_ctx_in_prefault);
		zdps->zdps_prefault_done = B_TRUE;
		count = dmu_tx_prefault_setup(dtbs, dn, off, io_size,
		    FTAG, B_FALSE, zvol_dmu_ctx_init_write_impl);
		if (count == 0)
			goto done;
		dmu_tx_prefault(dtbs);
		dmu_tx_buf_set_rele(dtbs);
		goto out;
	}
done:
	DEBUG_REFCOUNT_DEC(dmu_ctx_in_prefault);
	kmem_free(zdps, sizeof (*zdps));
	if (err) {
		zds->zds_dc.dc_err = err;
		zvol_dmu_err(zds, err_cb);
		goto out;
	}
	tx = dmu_tx_create(zv->zv_objset);
	dmu_tx_hold_write_by_dnode_impl(tx, zv->zv_dn, off,
	    io_size, B_FALSE);
	/* ensure all callbocks are cleared before blocking on assign */
	dmu_thread_context_process();
	err = dmu_tx_assign(tx, TXG_WAIT);
	if (err) {
		dmu_tx_abort(tx);
		zds->zds_dc.dc_err = err;
		zvol_dmu_err(zds, err_cb);
		goto out;
	}
	dmu_ctx_set_dmu_tx(&zds->zds_dc, tx);
	dmu_ctx_set_buf_set_transfer_cb(&zds->zds_dc,
	    zvol_dmu_buf_set_transfer_write);
	/* ensure all callbocks are cleared before blocking on the rangelock */
	dmu_thread_context_process();
	err = zfs_rangelock_tryenter_async(&zv->zv_rangelock, off, io_size,
	    RL_WRITER, &zds->zds_lr, (callback_fn)zvol_dmu_issue, zds);

	if (err == EINPROGRESS)
		goto out;

	zvol_dmu_issue(zds);
out:
	dmu_thread_context_process();
}

static int
zvol_dmu_ctx_init_write(zvol_dmu_state_t *zds, dmu_ctx_cb_t err_cb)
{
	zvol_dmu_prefault_state_t *zdps;

	zdps = kmem_zalloc(sizeof (*zdps), KM_SLEEP);
	zdps->zdps_zds = zds;
	zdps->zdps_err_cb = err_cb;
	zvol_dmu_ctx_init_write_impl(&zdps->zdps_dtbs);
	return (EINPROGRESS);
}

boolean_t
zvol_dmu_max_active(zvol_state_t *zv)
{
	return (zv->zv_active >= boot_ncpus);
}

void
zvol_dmu_ctx_init_enqueue(zvol_dmu_state_t *zds)
{
	zvol_state_t *zv = zds->zds_zv;

	ASSERT(mutex_owned(&zv->zv_state_lock));
	zds->zds_dc.dc_buf_ctx.dbc_flags |= DMU_CTX_FLAG_ASYNC;
	atomic_inc(&zv->zv_suspend_ref);
	list_insert_tail(&zv->zv_deferred, zds);
	DEBUG_REFCOUNT_ADD(dmu_ctx_deferred);
}

int
zvol_dmu_ctx_init(zvol_dmu_state_t *zds)
{
	zvol_state_t *zv = zds->zds_zv;
	uint32_t dmu_flags = zds->zds_dmu_flags;
	void *data = zds->zds_data;
	boolean_t reader = (dmu_flags & DMU_CTX_FLAG_READ) != 0;
	uint64_t off = zds->zds_off;
	uint64_t io_size = zds->zds_io_size;
	dmu_ctx_cb_t done_cb = zds->zds_dmu_done;
	dmu_ctx_cb_t err_cb = zds->zds_dmu_err;
	int err = 0;

	ASSERT(zv->zv_objset != NULL);
	ASSERT(atomic_read(&zv->zv_suspend_ref) >= 0);

	if (!zds->zds_retry)
		atomic_inc(&zv->zv_suspend_ref);
	zds->zds_sync |= !reader &&
	    (zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS);
	dmu_flags |= DMU_CTX_FLAG_NO_HOLD;
	if (reader)
		dmu_flags |= DMU_CTX_FLAG_PREFETCH;
	else if (zv->zv_flags & ZVOL_RDONLY)
		err = SET_ERROR(EIO);

	/* Reject I/Os that don't fall within the volume. */
	if (io_size > 0 && off >= zv->zv_volsize)
		err = SET_ERROR(EIO);

	if (err) {
		if (zds->zds_retry) {
			zds->zds_dc.dc_err = err;
			zvol_dmu_err(zds, err_cb);
		}
		return (err);
	}

	if (!zds->zds_retry) {
		DEBUG_REFCOUNT_ADD(dmu_ctx_in_init);
		mutex_enter(&zv->zv_state_lock);
		if (zvol_dmu_max_active(zv)) {
			zds->zds_dc.dc_buf_ctx.dbc_flags |= DMU_CTX_FLAG_ASYNC;
			list_insert_tail(&zv->zv_deferred, zds);
			DEBUG_REFCOUNT_ADD(dmu_ctx_deferred);
			err = EINPROGRESS;
		} else {
			zv->zv_active++;
			DEBUG_REFCOUNT_ADD(dmu_ctx_active);
		}
		mutex_exit(&zv->zv_state_lock);
	}
	if (err == EINPROGRESS)
		return (err);
	ASSERT(err == 0);
	/* Truncate I/Os to the end of the volume, if needed. */
	zds->zds_io_size = io_size = MIN(io_size, zv->zv_volsize - off);
	err = dmu_ctx_init(&zds->zds_dc, zv->zv_dn, zv->zv_objset,
	    ZVOL_OBJ, off, io_size, data, FTAG, dmu_flags);
	if (err) {
		zds->zds_dc.dc_err = err;
		zvol_dmu_err(zds, err_cb);
		return (err);
	}
	dmu_ctx_set_complete_cb(&zds->zds_dc, done_cb);

	if (reader) {
		err = zfs_rangelock_tryenter_async(&zv->zv_rangelock,
		    off, io_size, reader ? RL_READER : RL_WRITER,
		    &zds->zds_lr, (callback_fn)zvol_dmu_issue, zds);
	} else
		err = zvol_dmu_ctx_init_write(zds, err_cb);
	return (err);
}

void
zvol_dmu_issue(zvol_dmu_state_t *zds_)
{
	zvol_dmu_state_t *zds = zds_;
	zvol_state_t *zv = zds->zds_zv;

	DEBUG_REFCOUNT_DEC(dmu_ctx_in_init);
	ASSERT(zds->zds_lr->lr_owner == curthread);
	zds->zds_dc.dc_lr = zds->zds_lr;
	zds->zds_dc.dc_lr->lr_context = &zds->zds_dc;
	/* Errors are reported to the done callback via dmu_ctx->err. */
	(void) dmu_issue(&zds->zds_dc);
	zvol_dmu_ctx_init_deferred(zv);
	dmu_ctx_rele(&zds->zds_dc);
}

int
zvol_dmu_done(dmu_ctx_t *dc, callback_fn cb, void *arg)
{
	zvol_dmu_state_t *zds = (zvol_dmu_state_t *)dc;
	zvol_state_t *zv = zds->zds_zv;
	int rc = 0;

	/*
	 * Initialization failed
	 */
	if (zds->zds_lr != NULL)
		zfs_rangelock_exit(zds->zds_lr);

	if (dc->dc_completed_size < dc->dc_size &&
	    dc->dc_dn_offset > zv->zv_volsize)
		dc->dc_err = zio_worst_error(dc->dc_err, SET_ERROR(EINVAL));
	if ((dc->dc_flags & DMU_CTX_FLAG_READ) == 0 &&
	    (zv->zv_objset->os_sync == ZFS_SYNC_ALWAYS))
		rc = zil_commit_async(zv->zv_zilog, ZVOL_OBJ,
		    cb, arg);
	return (rc);
}

int
zvol_init_impl(void)
{
	int i;

	list_create(&zvol_state_list, sizeof (zvol_state_t),
	    offsetof(zvol_state_t, zv_next));
	rw_init(&zvol_state_lock, NULL, RW_DEFAULT, NULL);

	zvol_htable = kmem_alloc(ZVOL_HT_SIZE * sizeof (struct hlist_head),
	    KM_SLEEP);
	for (i = 0; i < ZVOL_HT_SIZE; i++)
		INIT_HLIST_HEAD(&zvol_htable[i]);

	return (0);
}

void
zvol_fini_impl(void)
{
	zvol_remove_minors_impl(NULL);

	/*
	 * The call to "zvol_remove_minors_impl" may dispatch entries to
	 * the system_taskq, but it doesn't wait for those entries to
	 * complete before it returns. Thus, we must wait for all of the
	 * removals to finish, before we can continue.
	 */
	taskq_wait_outstanding(system_taskq, 0);

	kmem_free(zvol_htable, ZVOL_HT_SIZE * sizeof (struct hlist_head));
	list_destroy(&zvol_state_list);
	rw_destroy(&zvol_state_lock);
}
