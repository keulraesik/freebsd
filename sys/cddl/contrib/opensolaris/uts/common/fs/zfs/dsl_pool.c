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
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013 by Delphix. All rights reserved.
 * Copyright (c) 2013 Steven Hartland. All rights reserved.
 */

#include <sys/dsl_pool.h>
#include <sys/dsl_dataset.h>
#include <sys/dsl_prop.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_synctask.h>
#include <sys/dsl_scan.h>
#include <sys/dnode.h>
#include <sys/dmu_tx.h>
#include <sys/dmu_objset.h>
#include <sys/arc.h>
#include <sys/zap.h>
#include <sys/zio.h>
#include <sys/zfs_context.h>
#include <sys/fs/zfs.h>
#include <sys/zfs_znode.h>
#include <sys/spa_impl.h>
#include <sys/dsl_deadlist.h>
#include <sys/bptree.h>
#include <sys/zfeature.h>
#include <sys/zil_impl.h>
#include <sys/dsl_userhold.h>

int zfs_no_write_throttle = 0;
int zfs_write_limit_shift = 3;			/* 1/8th of physical memory */
int zfs_txg_synctime_ms = 1000;		/* target millisecs to sync a txg */

uint64_t zfs_write_limit_min = 32 << 20;	/* min write limit is 32MB */
uint64_t zfs_write_limit_max = 0;		/* max data payload per txg */
uint64_t zfs_write_limit_inflated = 0;
uint64_t zfs_write_limit_override = 0;

kmutex_t zfs_write_limit_lock;

static pgcnt_t old_physmem = 0;

SYSCTL_DECL(_vfs_zfs);
TUNABLE_INT("vfs.zfs.no_write_throttle", &zfs_no_write_throttle);
SYSCTL_INT(_vfs_zfs, OID_AUTO, no_write_throttle, CTLFLAG_RDTUN,
    &zfs_no_write_throttle, 0, "");
TUNABLE_INT("vfs.zfs.write_limit_shift", &zfs_write_limit_shift);
SYSCTL_INT(_vfs_zfs, OID_AUTO, write_limit_shift, CTLFLAG_RDTUN,
    &zfs_write_limit_shift, 0, "2^N of physical memory");
SYSCTL_DECL(_vfs_zfs_txg);
TUNABLE_INT("vfs.zfs.txg.synctime_ms", &zfs_txg_synctime_ms);
SYSCTL_INT(_vfs_zfs_txg, OID_AUTO, synctime_ms, CTLFLAG_RDTUN,
    &zfs_txg_synctime_ms, 0, "Target milliseconds to sync a txg");

TUNABLE_QUAD("vfs.zfs.write_limit_min", &zfs_write_limit_min);
SYSCTL_UQUAD(_vfs_zfs, OID_AUTO, write_limit_min, CTLFLAG_RDTUN,
    &zfs_write_limit_min, 0, "Minimum write limit");
TUNABLE_QUAD("vfs.zfs.write_limit_max", &zfs_write_limit_max);
SYSCTL_UQUAD(_vfs_zfs, OID_AUTO, write_limit_max, CTLFLAG_RDTUN,
    &zfs_write_limit_max, 0, "Maximum data payload per txg");
TUNABLE_QUAD("vfs.zfs.write_limit_inflated", &zfs_write_limit_inflated);
SYSCTL_UQUAD(_vfs_zfs, OID_AUTO, write_limit_inflated, CTLFLAG_RDTUN,
    &zfs_write_limit_inflated, 0, "Maximum size of the dynamic write limit");
TUNABLE_QUAD("vfs.zfs.write_limit_override", &zfs_write_limit_override);
SYSCTL_UQUAD(_vfs_zfs, OID_AUTO, write_limit_override, CTLFLAG_RDTUN,
    &zfs_write_limit_override, 0,
    "Force a txg if dirty buffers exceed this value (bytes)");

int
dsl_pool_open_special_dir(dsl_pool_t *dp, const char *name, dsl_dir_t **ddp)
{
	uint64_t obj;
	int err;

	err = zap_lookup(dp->dp_meta_objset,
	    dp->dp_root_dir->dd_phys->dd_child_dir_zapobj,
	    name, sizeof (obj), 1, &obj);
	if (err)
		return (err);

	return (dsl_dir_hold_obj(dp, obj, name, dp, ddp));
}

static dsl_pool_t *
dsl_pool_open_impl(spa_t *spa, uint64_t txg)
{
	dsl_pool_t *dp;
	blkptr_t *bp = spa_get_rootblkptr(spa);

	dp = kmem_zalloc(sizeof (dsl_pool_t), KM_SLEEP);
	dp->dp_spa = spa;
	dp->dp_meta_rootbp = *bp;
	rrw_init(&dp->dp_config_rwlock, B_TRUE);
	dp->dp_write_limit = zfs_write_limit_min;
	txg_init(dp, txg);

	txg_list_create(&dp->dp_dirty_datasets,
	    offsetof(dsl_dataset_t, ds_dirty_link));
	txg_list_create(&dp->dp_dirty_zilogs,
	    offsetof(zilog_t, zl_dirty_link));
	txg_list_create(&dp->dp_dirty_dirs,
	    offsetof(dsl_dir_t, dd_dirty_link));
	txg_list_create(&dp->dp_sync_tasks,
	    offsetof(dsl_sync_task_t, dst_node));

	mutex_init(&dp->dp_lock, NULL, MUTEX_DEFAULT, NULL);

	dp->dp_vnrele_taskq = taskq_create("zfs_vn_rele_taskq", 1, minclsyspri,
	    1, 4, 0);

	return (dp);
}

int
dsl_pool_init(spa_t *spa, uint64_t txg, dsl_pool_t **dpp)
{
	int err;
	dsl_pool_t *dp = dsl_pool_open_impl(spa, txg);

	err = dmu_objset_open_impl(spa, NULL, &dp->dp_meta_rootbp,
	    &dp->dp_meta_objset);
	if (err != 0)
		dsl_pool_close(dp);
	else
		*dpp = dp;

	return (err);
}

int
dsl_pool_open(dsl_pool_t *dp)
{
	int err;
	dsl_dir_t *dd;
	dsl_dataset_t *ds;
	uint64_t obj;

	rrw_enter(&dp->dp_config_rwlock, RW_WRITER, FTAG);
	err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_ROOT_DATASET, sizeof (uint64_t), 1,
	    &dp->dp_root_dir_obj);
	if (err)
		goto out;

	err = dsl_dir_hold_obj(dp, dp->dp_root_dir_obj,
	    NULL, dp, &dp->dp_root_dir);
	if (err)
		goto out;

	err = dsl_pool_open_special_dir(dp, MOS_DIR_NAME, &dp->dp_mos_dir);
	if (err)
		goto out;

	if (spa_version(dp->dp_spa) >= SPA_VERSION_ORIGIN) {
		err = dsl_pool_open_special_dir(dp, ORIGIN_DIR_NAME, &dd);
		if (err)
			goto out;
		err = dsl_dataset_hold_obj(dp, dd->dd_phys->dd_head_dataset_obj,
		    FTAG, &ds);
		if (err == 0) {
			err = dsl_dataset_hold_obj(dp,
			    ds->ds_phys->ds_prev_snap_obj, dp,
			    &dp->dp_origin_snap);
			dsl_dataset_rele(ds, FTAG);
		}
		dsl_dir_rele(dd, dp);
		if (err)
			goto out;
	}

	if (spa_version(dp->dp_spa) >= SPA_VERSION_DEADLISTS) {
		err = dsl_pool_open_special_dir(dp, FREE_DIR_NAME,
		    &dp->dp_free_dir);
		if (err)
			goto out;

		err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_FREE_BPOBJ, sizeof (uint64_t), 1, &obj);
		if (err)
			goto out;
		VERIFY0(bpobj_open(&dp->dp_free_bpobj,
		    dp->dp_meta_objset, obj));
	}

	if (spa_feature_is_active(dp->dp_spa,
	    &spa_feature_table[SPA_FEATURE_ASYNC_DESTROY])) {
		err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_BPTREE_OBJ, sizeof (uint64_t), 1,
		    &dp->dp_bptree_obj);
		if (err != 0)
			goto out;
	}

	if (spa_feature_is_active(dp->dp_spa,
	    &spa_feature_table[SPA_FEATURE_EMPTY_BPOBJ])) {
		err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_EMPTY_BPOBJ, sizeof (uint64_t), 1,
		    &dp->dp_empty_bpobj);
		if (err != 0)
			goto out;
	}

	err = zap_lookup(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_TMP_USERREFS, sizeof (uint64_t), 1,
	    &dp->dp_tmp_userrefs_obj);
	if (err == ENOENT)
		err = 0;
	if (err)
		goto out;

	err = dsl_scan_init(dp, dp->dp_tx.tx_open_txg);

out:
	rrw_exit(&dp->dp_config_rwlock, FTAG);
	return (err);
}

void
dsl_pool_close(dsl_pool_t *dp)
{
	/* drop our references from dsl_pool_open() */

	/*
	 * Since we held the origin_snap from "syncing" context (which
	 * includes pool-opening context), it actually only got a "ref"
	 * and not a hold, so just drop that here.
	 */
	if (dp->dp_origin_snap)
		dsl_dataset_rele(dp->dp_origin_snap, dp);
	if (dp->dp_mos_dir)
		dsl_dir_rele(dp->dp_mos_dir, dp);
	if (dp->dp_free_dir)
		dsl_dir_rele(dp->dp_free_dir, dp);
	if (dp->dp_root_dir)
		dsl_dir_rele(dp->dp_root_dir, dp);

	bpobj_close(&dp->dp_free_bpobj);

	/* undo the dmu_objset_open_impl(mos) from dsl_pool_open() */
	if (dp->dp_meta_objset)
		dmu_objset_evict(dp->dp_meta_objset);

	txg_list_destroy(&dp->dp_dirty_datasets);
	txg_list_destroy(&dp->dp_dirty_zilogs);
	txg_list_destroy(&dp->dp_sync_tasks);
	txg_list_destroy(&dp->dp_dirty_dirs);

	arc_flush(dp->dp_spa);
	txg_fini(dp);
	dsl_scan_fini(dp);
	rrw_destroy(&dp->dp_config_rwlock);
	mutex_destroy(&dp->dp_lock);
	taskq_destroy(dp->dp_vnrele_taskq);
	if (dp->dp_blkstats)
		kmem_free(dp->dp_blkstats, sizeof (zfs_all_blkstats_t));
	kmem_free(dp, sizeof (dsl_pool_t));
}

dsl_pool_t *
dsl_pool_create(spa_t *spa, nvlist_t *zplprops, uint64_t txg)
{
	int err;
	dsl_pool_t *dp = dsl_pool_open_impl(spa, txg);
	dmu_tx_t *tx = dmu_tx_create_assigned(dp, txg);
	objset_t *os;
	dsl_dataset_t *ds;
	uint64_t obj;

	rrw_enter(&dp->dp_config_rwlock, RW_WRITER, FTAG);

	/* create and open the MOS (meta-objset) */
	dp->dp_meta_objset = dmu_objset_create_impl(spa,
	    NULL, &dp->dp_meta_rootbp, DMU_OST_META, tx);

	/* create the pool directory */
	err = zap_create_claim(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_OT_OBJECT_DIRECTORY, DMU_OT_NONE, 0, tx);
	ASSERT0(err);

	/* Initialize scan structures */
	VERIFY0(dsl_scan_init(dp, txg));

	/* create and open the root dir */
	dp->dp_root_dir_obj = dsl_dir_create_sync(dp, NULL, NULL, tx);
	VERIFY0(dsl_dir_hold_obj(dp, dp->dp_root_dir_obj,
	    NULL, dp, &dp->dp_root_dir));

	/* create and open the meta-objset dir */
	(void) dsl_dir_create_sync(dp, dp->dp_root_dir, MOS_DIR_NAME, tx);
	VERIFY0(dsl_pool_open_special_dir(dp,
	    MOS_DIR_NAME, &dp->dp_mos_dir));

	if (spa_version(spa) >= SPA_VERSION_DEADLISTS) {
		/* create and open the free dir */
		(void) dsl_dir_create_sync(dp, dp->dp_root_dir,
		    FREE_DIR_NAME, tx);
		VERIFY0(dsl_pool_open_special_dir(dp,
		    FREE_DIR_NAME, &dp->dp_free_dir));

		/* create and open the free_bplist */
		obj = bpobj_alloc(dp->dp_meta_objset, SPA_MAXBLOCKSIZE, tx);
		VERIFY(zap_add(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
		    DMU_POOL_FREE_BPOBJ, sizeof (uint64_t), 1, &obj, tx) == 0);
		VERIFY0(bpobj_open(&dp->dp_free_bpobj,
		    dp->dp_meta_objset, obj));
	}

	if (spa_version(spa) >= SPA_VERSION_DSL_SCRUB)
		dsl_pool_create_origin(dp, tx);

	/* create the root dataset */
	obj = dsl_dataset_create_sync_dd(dp->dp_root_dir, NULL, 0, tx);

	/* create the root objset */
	VERIFY0(dsl_dataset_hold_obj(dp, obj, FTAG, &ds));
	os = dmu_objset_create_impl(dp->dp_spa, ds,
	    dsl_dataset_get_blkptr(ds), DMU_OST_ZFS, tx);
#ifdef _KERNEL
	zfs_create_fs(os, kcred, zplprops, tx);
#endif
	dsl_dataset_rele(ds, FTAG);

	dmu_tx_commit(tx);

	rrw_exit(&dp->dp_config_rwlock, FTAG);

	return (dp);
}

/*
 * Account for the meta-objset space in its placeholder dsl_dir.
 */
void
dsl_pool_mos_diduse_space(dsl_pool_t *dp,
    int64_t used, int64_t comp, int64_t uncomp)
{
	ASSERT3U(comp, ==, uncomp); /* it's all metadata */
	mutex_enter(&dp->dp_lock);
	dp->dp_mos_used_delta += used;
	dp->dp_mos_compressed_delta += comp;
	dp->dp_mos_uncompressed_delta += uncomp;
	mutex_exit(&dp->dp_lock);
}

static int
deadlist_enqueue_cb(void *arg, const blkptr_t *bp, dmu_tx_t *tx)
{
	dsl_deadlist_t *dl = arg;
	dsl_deadlist_insert(dl, bp, tx);
	return (0);
}

void
dsl_pool_sync(dsl_pool_t *dp, uint64_t txg)
{
	zio_t *zio;
	dmu_tx_t *tx;
	dsl_dir_t *dd;
	dsl_dataset_t *ds;
	objset_t *mos = dp->dp_meta_objset;
	hrtime_t start, write_time;
	uint64_t data_written;
	int err;
	list_t synced_datasets;

	list_create(&synced_datasets, sizeof (dsl_dataset_t),
	    offsetof(dsl_dataset_t, ds_synced_link));

	/*
	 * We need to copy dp_space_towrite() before doing
	 * dsl_sync_task_sync(), because
	 * dsl_dataset_snapshot_reserve_space() will increase
	 * dp_space_towrite but not actually write anything.
	 */
	data_written = dp->dp_space_towrite[txg & TXG_MASK];

	tx = dmu_tx_create_assigned(dp, txg);

	dp->dp_read_overhead = 0;
	start = gethrtime();

	zio = zio_root(dp->dp_spa, NULL, NULL, ZIO_FLAG_MUSTSUCCEED);
	while (ds = txg_list_remove(&dp->dp_dirty_datasets, txg)) {
		/*
		 * We must not sync any non-MOS datasets twice, because
		 * we may have taken a snapshot of them.  However, we
		 * may sync newly-created datasets on pass 2.
		 */
		ASSERT(!list_link_active(&ds->ds_synced_link));
		list_insert_tail(&synced_datasets, ds);
		dsl_dataset_sync(ds, zio, tx);
	}
	DTRACE_PROBE(pool_sync__1setup);
	err = zio_wait(zio);

	write_time = gethrtime() - start;
	ASSERT(err == 0);
	DTRACE_PROBE(pool_sync__2rootzio);

	/*
	 * After the data blocks have been written (ensured by the zio_wait()
	 * above), update the user/group space accounting.
	 */
	for (ds = list_head(&synced_datasets); ds;
	    ds = list_next(&synced_datasets, ds))
		dmu_objset_do_userquota_updates(ds->ds_objset, tx);

	/*
	 * Sync the datasets again to push out the changes due to
	 * userspace updates.  This must be done before we process the
	 * sync tasks, so that any snapshots will have the correct
	 * user accounting information (and we won't get confused
	 * about which blocks are part of the snapshot).
	 */
	zio = zio_root(dp->dp_spa, NULL, NULL, ZIO_FLAG_MUSTSUCCEED);
	while (ds = txg_list_remove(&dp->dp_dirty_datasets, txg)) {
		ASSERT(list_link_active(&ds->ds_synced_link));
		dmu_buf_rele(ds->ds_dbuf, ds);
		dsl_dataset_sync(ds, zio, tx);
	}
	err = zio_wait(zio);

	/*
	 * Now that the datasets have been completely synced, we can
	 * clean up our in-memory structures accumulated while syncing:
	 *
	 *  - move dead blocks from the pending deadlist to the on-disk deadlist
	 *  - release hold from dsl_dataset_dirty()
	 */
	while (ds = list_remove_head(&synced_datasets)) {
		objset_t *os = ds->ds_objset;
		bplist_iterate(&ds->ds_pending_deadlist,
		    deadlist_enqueue_cb, &ds->ds_deadlist, tx);
		ASSERT(!dmu_objset_is_dirty(os, txg));
		dmu_buf_rele(ds->ds_dbuf, ds);
	}

	start = gethrtime();
	while (dd = txg_list_remove(&dp->dp_dirty_dirs, txg))
		dsl_dir_sync(dd, tx);
	write_time += gethrtime() - start;

	/*
	 * The MOS's space is accounted for in the pool/$MOS
	 * (dp_mos_dir).  We can't modify the mos while we're syncing
	 * it, so we remember the deltas and apply them here.
	 */
	if (dp->dp_mos_used_delta != 0 || dp->dp_mos_compressed_delta != 0 ||
	    dp->dp_mos_uncompressed_delta != 0) {
		dsl_dir_diduse_space(dp->dp_mos_dir, DD_USED_HEAD,
		    dp->dp_mos_used_delta,
		    dp->dp_mos_compressed_delta,
		    dp->dp_mos_uncompressed_delta, tx);
		dp->dp_mos_used_delta = 0;
		dp->dp_mos_compressed_delta = 0;
		dp->dp_mos_uncompressed_delta = 0;
	}

	start = gethrtime();
	if (list_head(&mos->os_dirty_dnodes[txg & TXG_MASK]) != NULL ||
	    list_head(&mos->os_free_dnodes[txg & TXG_MASK]) != NULL) {
		zio = zio_root(dp->dp_spa, NULL, NULL, ZIO_FLAG_MUSTSUCCEED);
		dmu_objset_sync(mos, zio, tx);
		err = zio_wait(zio);
		ASSERT(err == 0);
		dprintf_bp(&dp->dp_meta_rootbp, "meta objset rootbp is %s", "");
		spa_set_rootblkptr(dp->dp_spa, &dp->dp_meta_rootbp);
	}
	write_time += gethrtime() - start;
	DTRACE_PROBE2(pool_sync__4io, hrtime_t, write_time,
	    hrtime_t, dp->dp_read_overhead);
	write_time -= dp->dp_read_overhead;

	/*
	 * If we modify a dataset in the same txg that we want to destroy it,
	 * its dsl_dir's dd_dbuf will be dirty, and thus have a hold on it.
	 * dsl_dir_destroy_check() will fail if there are unexpected holds.
	 * Therefore, we want to sync the MOS (thus syncing the dd_dbuf
	 * and clearing the hold on it) before we process the sync_tasks.
	 * The MOS data dirtied by the sync_tasks will be synced on the next
	 * pass.
	 */
	DTRACE_PROBE(pool_sync__3task);
	if (!txg_list_empty(&dp->dp_sync_tasks, txg)) {
		dsl_sync_task_t *dst;
		/*
		 * No more sync tasks should have been added while we
		 * were syncing.
		 */
		ASSERT(spa_sync_pass(dp->dp_spa) == 1);
		while (dst = txg_list_remove(&dp->dp_sync_tasks, txg))
			dsl_sync_task_sync(dst, tx);
	}

	dmu_tx_commit(tx);

	dp->dp_space_towrite[txg & TXG_MASK] = 0;
	ASSERT(dp->dp_tempreserved[txg & TXG_MASK] == 0);

	/*
	 * If the write limit max has not been explicitly set, set it
	 * to a fraction of available physical memory (default 1/8th).
	 * Note that we must inflate the limit because the spa
	 * inflates write sizes to account for data replication.
	 * Check this each sync phase to catch changing memory size.
	 */
	if (physmem != old_physmem && zfs_write_limit_shift) {
		mutex_enter(&zfs_write_limit_lock);
		old_physmem = physmem;
		zfs_write_limit_max = ptob(physmem) >> zfs_write_limit_shift;
		zfs_write_limit_inflated = MAX(zfs_write_limit_min,
		    spa_get_asize(dp->dp_spa, zfs_write_limit_max));
		mutex_exit(&zfs_write_limit_lock);
	}

	/*
	 * Attempt to keep the sync time consistent by adjusting the
	 * amount of write traffic allowed into each transaction group.
	 * Weight the throughput calculation towards the current value:
	 * 	thru = 3/4 old_thru + 1/4 new_thru
	 *
	 * Note: write_time is in nanosecs, so write_time/MICROSEC
	 * yields millisecs
	 */
	ASSERT(zfs_write_limit_min > 0);
	if (data_written > zfs_write_limit_min / 8 && write_time > MICROSEC) {
		uint64_t throughput = data_written / (write_time / MICROSEC);

		if (dp->dp_throughput)
			dp->dp_throughput = throughput / 4 +
			    3 * dp->dp_throughput / 4;
		else
			dp->dp_throughput = throughput;
		dp->dp_write_limit = MIN(zfs_write_limit_inflated,
		    MAX(zfs_write_limit_min,
		    dp->dp_throughput * zfs_txg_synctime_ms));
	}
}

void
dsl_pool_sync_done(dsl_pool_t *dp, uint64_t txg)
{
	zilog_t *zilog;
	dsl_dataset_t *ds;

	while (zilog = txg_list_remove(&dp->dp_dirty_zilogs, txg)) {
		ds = dmu_objset_ds(zilog->zl_os);
		zil_clean(zilog, txg);
		ASSERT(!dmu_objset_is_dirty(zilog->zl_os, txg));
		dmu_buf_rele(ds->ds_dbuf, zilog);
	}
	ASSERT(!dmu_objset_is_dirty(dp->dp_meta_objset, txg));
}

/*
 * TRUE if the current thread is the tx_sync_thread or if we
 * are being called from SPA context during pool initialization.
 */
int
dsl_pool_sync_context(dsl_pool_t *dp)
{
	return (curthread == dp->dp_tx.tx_sync_thread ||
	    spa_is_initializing(dp->dp_spa));
}

uint64_t
dsl_pool_adjustedsize(dsl_pool_t *dp, boolean_t netfree)
{
	uint64_t space, resv;

	/*
	 * Reserve about 1.6% (1/64), or at least 32MB, for allocation
	 * efficiency.
	 * XXX The intent log is not accounted for, so it must fit
	 * within this slop.
	 *
	 * If we're trying to assess whether it's OK to do a free,
	 * cut the reservation in half to allow forward progress
	 * (e.g. make it possible to rm(1) files from a full pool).
	 */
	space = spa_get_dspace(dp->dp_spa);
	resv = MAX(space >> 6, SPA_MINDEVSIZE >> 1);
	if (netfree)
		resv >>= 1;

	return (space - resv);
}

int
dsl_pool_tempreserve_space(dsl_pool_t *dp, uint64_t space, dmu_tx_t *tx)
{
	uint64_t reserved = 0;
	uint64_t write_limit = (zfs_write_limit_override ?
	    zfs_write_limit_override : dp->dp_write_limit);

	if (zfs_no_write_throttle) {
		atomic_add_64(&dp->dp_tempreserved[tx->tx_txg & TXG_MASK],
		    space);
		return (0);
	}

	/*
	 * Check to see if we have exceeded the maximum allowed IO for
	 * this transaction group.  We can do this without locks since
	 * a little slop here is ok.  Note that we do the reserved check
	 * with only half the requested reserve: this is because the
	 * reserve requests are worst-case, and we really don't want to
	 * throttle based off of worst-case estimates.
	 */
	if (write_limit > 0) {
		reserved = dp->dp_space_towrite[tx->tx_txg & TXG_MASK]
		    + dp->dp_tempreserved[tx->tx_txg & TXG_MASK] / 2;

		if (reserved && reserved > write_limit)
			return (SET_ERROR(ERESTART));
	}

	atomic_add_64(&dp->dp_tempreserved[tx->tx_txg & TXG_MASK], space);

	/*
	 * If this transaction group is over 7/8ths capacity, delay
	 * the caller 1 clock tick.  This will slow down the "fill"
	 * rate until the sync process can catch up with us.
	 */
	if (reserved && reserved > (write_limit - (write_limit >> 3)))
		txg_delay(dp, tx->tx_txg, 1);

	return (0);
}

void
dsl_pool_tempreserve_clear(dsl_pool_t *dp, int64_t space, dmu_tx_t *tx)
{
	ASSERT(dp->dp_tempreserved[tx->tx_txg & TXG_MASK] >= space);
	atomic_add_64(&dp->dp_tempreserved[tx->tx_txg & TXG_MASK], -space);
}

void
dsl_pool_memory_pressure(dsl_pool_t *dp)
{
	uint64_t space_inuse = 0;
	int i;

	if (dp->dp_write_limit == zfs_write_limit_min)
		return;

	for (i = 0; i < TXG_SIZE; i++) {
		space_inuse += dp->dp_space_towrite[i];
		space_inuse += dp->dp_tempreserved[i];
	}
	dp->dp_write_limit = MAX(zfs_write_limit_min,
	    MIN(dp->dp_write_limit, space_inuse / 4));
}

void
dsl_pool_willuse_space(dsl_pool_t *dp, int64_t space, dmu_tx_t *tx)
{
	if (space > 0) {
		mutex_enter(&dp->dp_lock);
		dp->dp_space_towrite[tx->tx_txg & TXG_MASK] += space;
		mutex_exit(&dp->dp_lock);
	}
}

/* ARGSUSED */
static int
upgrade_clones_cb(dsl_pool_t *dp, dsl_dataset_t *hds, void *arg)
{
	dmu_tx_t *tx = arg;
	dsl_dataset_t *ds, *prev = NULL;
	int err;

	err = dsl_dataset_hold_obj(dp, hds->ds_object, FTAG, &ds);
	if (err)
		return (err);

	while (ds->ds_phys->ds_prev_snap_obj != 0) {
		err = dsl_dataset_hold_obj(dp, ds->ds_phys->ds_prev_snap_obj,
		    FTAG, &prev);
		if (err) {
			dsl_dataset_rele(ds, FTAG);
			return (err);
		}

		if (prev->ds_phys->ds_next_snap_obj != ds->ds_object)
			break;
		dsl_dataset_rele(ds, FTAG);
		ds = prev;
		prev = NULL;
	}

	if (prev == NULL) {
		prev = dp->dp_origin_snap;

		/*
		 * The $ORIGIN can't have any data, or the accounting
		 * will be wrong.
		 */
		ASSERT0(prev->ds_phys->ds_bp.blk_birth);

		/* The origin doesn't get attached to itself */
		if (ds->ds_object == prev->ds_object) {
			dsl_dataset_rele(ds, FTAG);
			return (0);
		}

		dmu_buf_will_dirty(ds->ds_dbuf, tx);
		ds->ds_phys->ds_prev_snap_obj = prev->ds_object;
		ds->ds_phys->ds_prev_snap_txg = prev->ds_phys->ds_creation_txg;

		dmu_buf_will_dirty(ds->ds_dir->dd_dbuf, tx);
		ds->ds_dir->dd_phys->dd_origin_obj = prev->ds_object;

		dmu_buf_will_dirty(prev->ds_dbuf, tx);
		prev->ds_phys->ds_num_children++;

		if (ds->ds_phys->ds_next_snap_obj == 0) {
			ASSERT(ds->ds_prev == NULL);
			VERIFY0(dsl_dataset_hold_obj(dp,
			    ds->ds_phys->ds_prev_snap_obj, ds, &ds->ds_prev));
		}
	}

	ASSERT3U(ds->ds_dir->dd_phys->dd_origin_obj, ==, prev->ds_object);
	ASSERT3U(ds->ds_phys->ds_prev_snap_obj, ==, prev->ds_object);

	if (prev->ds_phys->ds_next_clones_obj == 0) {
		dmu_buf_will_dirty(prev->ds_dbuf, tx);
		prev->ds_phys->ds_next_clones_obj =
		    zap_create(dp->dp_meta_objset,
		    DMU_OT_NEXT_CLONES, DMU_OT_NONE, 0, tx);
	}
	VERIFY0(zap_add_int(dp->dp_meta_objset,
	    prev->ds_phys->ds_next_clones_obj, ds->ds_object, tx));

	dsl_dataset_rele(ds, FTAG);
	if (prev != dp->dp_origin_snap)
		dsl_dataset_rele(prev, FTAG);
	return (0);
}

void
dsl_pool_upgrade_clones(dsl_pool_t *dp, dmu_tx_t *tx)
{
	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(dp->dp_origin_snap != NULL);

	VERIFY0(dmu_objset_find_dp(dp, dp->dp_root_dir_obj, upgrade_clones_cb,
	    tx, DS_FIND_CHILDREN));
}

/* ARGSUSED */
static int
upgrade_dir_clones_cb(dsl_pool_t *dp, dsl_dataset_t *ds, void *arg)
{
	dmu_tx_t *tx = arg;
	objset_t *mos = dp->dp_meta_objset;

	if (ds->ds_dir->dd_phys->dd_origin_obj != 0) {
		dsl_dataset_t *origin;

		VERIFY0(dsl_dataset_hold_obj(dp,
		    ds->ds_dir->dd_phys->dd_origin_obj, FTAG, &origin));

		if (origin->ds_dir->dd_phys->dd_clones == 0) {
			dmu_buf_will_dirty(origin->ds_dir->dd_dbuf, tx);
			origin->ds_dir->dd_phys->dd_clones = zap_create(mos,
			    DMU_OT_DSL_CLONES, DMU_OT_NONE, 0, tx);
		}

		VERIFY0(zap_add_int(dp->dp_meta_objset,
		    origin->ds_dir->dd_phys->dd_clones, ds->ds_object, tx));

		dsl_dataset_rele(origin, FTAG);
	}
	return (0);
}

void
dsl_pool_upgrade_dir_clones(dsl_pool_t *dp, dmu_tx_t *tx)
{
	ASSERT(dmu_tx_is_syncing(tx));
	uint64_t obj;

	(void) dsl_dir_create_sync(dp, dp->dp_root_dir, FREE_DIR_NAME, tx);
	VERIFY0(dsl_pool_open_special_dir(dp,
	    FREE_DIR_NAME, &dp->dp_free_dir));

	/*
	 * We can't use bpobj_alloc(), because spa_version() still
	 * returns the old version, and we need a new-version bpobj with
	 * subobj support.  So call dmu_object_alloc() directly.
	 */
	obj = dmu_object_alloc(dp->dp_meta_objset, DMU_OT_BPOBJ,
	    SPA_MAXBLOCKSIZE, DMU_OT_BPOBJ_HDR, sizeof (bpobj_phys_t), tx);
	VERIFY0(zap_add(dp->dp_meta_objset, DMU_POOL_DIRECTORY_OBJECT,
	    DMU_POOL_FREE_BPOBJ, sizeof (uint64_t), 1, &obj, tx));
	VERIFY0(bpobj_open(&dp->dp_free_bpobj, dp->dp_meta_objset, obj));

	VERIFY0(dmu_objset_find_dp(dp, dp->dp_root_dir_obj,
	    upgrade_dir_clones_cb, tx, DS_FIND_CHILDREN));
}

void
dsl_pool_create_origin(dsl_pool_t *dp, dmu_tx_t *tx)
{
	uint64_t dsobj;
	dsl_dataset_t *ds;

	ASSERT(dmu_tx_is_syncing(tx));
	ASSERT(dp->dp_origin_snap == NULL);
	ASSERT(rrw_held(&dp->dp_config_rwlock, RW_WRITER));

	/* create the origin dir, ds, & snap-ds */
	dsobj = dsl_dataset_create_sync(dp->dp_root_dir, ORIGIN_DIR_NAME,
	    NULL, 0, kcred, tx);
	VERIFY0(dsl_dataset_hold_obj(dp, dsobj, FTAG, &ds));
	dsl_dataset_snapshot_sync_impl(ds, ORIGIN_DIR_NAME, tx);
	VERIFY0(dsl_dataset_hold_obj(dp, ds->ds_phys->ds_prev_snap_obj,
	    dp, &dp->dp_origin_snap));
	dsl_dataset_rele(ds, FTAG);
}

taskq_t *
dsl_pool_vnrele_taskq(dsl_pool_t *dp)
{
	return (dp->dp_vnrele_taskq);
}

/*
 * Walk through the pool-wide zap object of temporary snapshot user holds
 * and release them.
 */
void
dsl_pool_clean_tmp_userrefs(dsl_pool_t *dp)
{
	zap_attribute_t za;
	zap_cursor_t zc;
	objset_t *mos = dp->dp_meta_objset;
	uint64_t zapobj = dp->dp_tmp_userrefs_obj;
	nvlist_t *holds;

	if (zapobj == 0)
		return;
	ASSERT(spa_version(dp->dp_spa) >= SPA_VERSION_USERREFS);

	holds = fnvlist_alloc();

	for (zap_cursor_init(&zc, mos, zapobj);
	    zap_cursor_retrieve(&zc, &za) == 0;
	    zap_cursor_advance(&zc)) {
		char *htag;
		nvlist_t *tags;

		htag = strchr(za.za_name, '-');
		*htag = '\0';
		++htag;
		if (nvlist_lookup_nvlist(holds, za.za_name, &tags) != 0) {
			tags = fnvlist_alloc();
			fnvlist_add_boolean(tags, htag);
			fnvlist_add_nvlist(holds, za.za_name, tags);
			fnvlist_free(tags);
		} else {
			fnvlist_add_boolean(tags, htag);
		}
	}
	dsl_dataset_user_release_tmp(dp, holds);
	fnvlist_free(holds);
	zap_cursor_fini(&zc);
}

/*
 * Create the pool-wide zap object for storing temporary snapshot holds.
 */
void
dsl_pool_user_hold_create_obj(dsl_pool_t *dp, dmu_tx_t *tx)
{
	objset_t *mos = dp->dp_meta_objset;

	ASSERT(dp->dp_tmp_userrefs_obj == 0);
	ASSERT(dmu_tx_is_syncing(tx));

	dp->dp_tmp_userrefs_obj = zap_create_link(mos, DMU_OT_USERREFS,
	    DMU_POOL_DIRECTORY_OBJECT, DMU_POOL_TMP_USERREFS, tx);
}

static int
dsl_pool_user_hold_rele_impl(dsl_pool_t *dp, uint64_t dsobj,
    const char *tag, uint64_t now, dmu_tx_t *tx, boolean_t holding)
{
	objset_t *mos = dp->dp_meta_objset;
	uint64_t zapobj = dp->dp_tmp_userrefs_obj;
	char *name;
	int error;

	ASSERT(spa_version(dp->dp_spa) >= SPA_VERSION_USERREFS);
	ASSERT(dmu_tx_is_syncing(tx));

	/*
	 * If the pool was created prior to SPA_VERSION_USERREFS, the
	 * zap object for temporary holds might not exist yet.
	 */
	if (zapobj == 0) {
		if (holding) {
			dsl_pool_user_hold_create_obj(dp, tx);
			zapobj = dp->dp_tmp_userrefs_obj;
		} else {
			return (SET_ERROR(ENOENT));
		}
	}

	name = kmem_asprintf("%llx-%s", (u_longlong_t)dsobj, tag);
	if (holding)
		error = zap_add(mos, zapobj, name, 8, 1, &now, tx);
	else
		error = zap_remove(mos, zapobj, name, tx);
	strfree(name);

	return (error);
}

/*
 * Add a temporary hold for the given dataset object and tag.
 */
int
dsl_pool_user_hold(dsl_pool_t *dp, uint64_t dsobj, const char *tag,
    uint64_t now, dmu_tx_t *tx)
{
	return (dsl_pool_user_hold_rele_impl(dp, dsobj, tag, now, tx, B_TRUE));
}

/*
 * Release a temporary hold for the given dataset object and tag.
 */
int
dsl_pool_user_release(dsl_pool_t *dp, uint64_t dsobj, const char *tag,
    dmu_tx_t *tx)
{
	return (dsl_pool_user_hold_rele_impl(dp, dsobj, tag, 0,
	    tx, B_FALSE));
}

/*
 * DSL Pool Configuration Lock
 *
 * The dp_config_rwlock protects against changes to DSL state (e.g. dataset
 * creation / destruction / rename / property setting).  It must be held for
 * read to hold a dataset or dsl_dir.  I.e. you must call
 * dsl_pool_config_enter() or dsl_pool_hold() before calling
 * dsl_{dataset,dir}_hold{_obj}.  In most circumstances, the dp_config_rwlock
 * must be held continuously until all datasets and dsl_dirs are released.
 *
 * The only exception to this rule is that if a "long hold" is placed on
 * a dataset, then the dp_config_rwlock may be dropped while the dataset
 * is still held.  The long hold will prevent the dataset from being
 * destroyed -- the destroy will fail with EBUSY.  A long hold can be
 * obtained by calling dsl_dataset_long_hold(), or by "owning" a dataset
 * (by calling dsl_{dataset,objset}_{try}own{_obj}).
 *
 * Legitimate long-holders (including owners) should be long-running, cancelable
 * tasks that should cause "zfs destroy" to fail.  This includes DMU
 * consumers (i.e. a ZPL filesystem being mounted or ZVOL being open),
 * "zfs send", and "zfs diff".  There are several other long-holders whose
 * uses are suboptimal (e.g. "zfs promote", and zil_suspend()).
 *
 * The usual formula for long-holding would be:
 * dsl_pool_hold()
 * dsl_dataset_hold()
 * ... perform checks ...
 * dsl_dataset_long_hold()
 * dsl_pool_rele()
 * ... perform long-running task ...
 * dsl_dataset_long_rele()
 * dsl_dataset_rele()
 *
 * Note that when the long hold is released, the dataset is still held but
 * the pool is not held.  The dataset may change arbitrarily during this time
 * (e.g. it could be destroyed).  Therefore you shouldn't do anything to the
 * dataset except release it.
 *
 * User-initiated operations (e.g. ioctls, zfs_ioc_*()) are either read-only
 * or modifying operations.
 *
 * Modifying operations should generally use dsl_sync_task().  The synctask
 * infrastructure enforces proper locking strategy with respect to the
 * dp_config_rwlock.  See the comment above dsl_sync_task() for details.
 *
 * Read-only operations will manually hold the pool, then the dataset, obtain
 * information from the dataset, then release the pool and dataset.
 * dmu_objset_{hold,rele}() are convenience routines that also do the pool
 * hold/rele.
 */

int
dsl_pool_hold(const char *name, void *tag, dsl_pool_t **dp)
{
	spa_t *spa;
	int error;

	error = spa_open(name, &spa, tag);
	if (error == 0) {
		*dp = spa_get_dsl(spa);
		dsl_pool_config_enter(*dp, tag);
	}
	return (error);
}

void
dsl_pool_rele(dsl_pool_t *dp, void *tag)
{
	dsl_pool_config_exit(dp, tag);
	spa_close(dp->dp_spa, tag);
}

void
dsl_pool_config_enter(dsl_pool_t *dp, void *tag)
{
	/*
	 * We use a "reentrant" reader-writer lock, but not reentrantly.
	 *
	 * The rrwlock can (with the track_all flag) track all reading threads,
	 * which is very useful for debugging which code path failed to release
	 * the lock, and for verifying that the *current* thread does hold
	 * the lock.
	 *
	 * (Unlike a rwlock, which knows that N threads hold it for
	 * read, but not *which* threads, so rw_held(RW_READER) returns TRUE
	 * if any thread holds it for read, even if this thread doesn't).
	 */
	ASSERT(!rrw_held(&dp->dp_config_rwlock, RW_READER));
	rrw_enter(&dp->dp_config_rwlock, RW_READER, tag);
}

void
dsl_pool_config_exit(dsl_pool_t *dp, void *tag)
{
	rrw_exit(&dp->dp_config_rwlock, tag);
}

boolean_t
dsl_pool_config_held(dsl_pool_t *dp)
{
	return (RRW_LOCK_HELD(&dp->dp_config_rwlock));
}
