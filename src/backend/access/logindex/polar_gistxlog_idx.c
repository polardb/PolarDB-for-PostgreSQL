/*-------------------------------------------------------------------------
 *
 * polar_gistxlog_idx.c
 *    WAL redo parse logic for gist index.
 *
 * Portions Copyright (c) 2021, Alibaba Group Holding limited
 * Portions Copyright (c) 1996-2018, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *           src/backend/access/logindex/polar_gistxlog_idx.c
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/bufmask.h"
#include "access/gist_private.h"
#include "access/gistxlog.h"
#include "access/polar_logindex_redo.h"
#include "access/xlogutils.h"
#include "access/xlog_internal.h"
#include "storage/buf_internals.h"
#include "storage/standby.h"
#include "utils/memutils.h"

static void
polar_gist_redo_page_update_record_save(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	polar_logindex_save_block(instance, record, 0);

	if (XLogRecHasBlockRef(record, 1))
		polar_logindex_save_block(instance, record, 1);
}

static void
polar_gist_redo_page_update_record_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	BufferTag	update_tag;
	polar_page_lock_t update_lock;
	Buffer		update_buf = InvalidBuffer;

	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 0, update_tag, update_lock, update_buf);

	if (XLogRecHasBlockRef(record, 1))
		polar_logindex_redo_parse(instance, record, 1);

	if (BufferIsValid(update_buf))
		UnlockReleaseBuffer(update_buf);

	polar_logindex_mini_trans_unlock(instance->mini_trans, update_lock);
}

static void
polar_gist_redo_page_delete_record_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	/*
	 * If we have any conflict processing to do, it must happen before we
	 * update the page.
	 *
	 * GiST delete records can conflict with standby queries.  You might think
	 * that vacuum records would conflict as well, but we've handled that
	 * already.  XLOG_HEAP2_PRUNE records provide the highest xid cleaned by
	 * the vacuum of the heap and so we can resolve any conflicts just once
	 * when that arrives.  After that we know that no conflicts exist from
	 * individual gist vacuum records on that index.
	 */
	if (polar_enable_resolve_conflict && reachedConsistency && InHotStandby)
	{
		RelFileNode rnode;
		gistxlogDelete *xldata = (gistxlogDelete *) XLogRecGetData(record);

		XLogRecGetBlockTag(record, 0, &rnode, NULL, NULL);

		ResolveRecoveryConflictWithSnapshot(xldata->latestRemovedXid, rnode);
	}

	polar_logindex_redo_parse(instance, record, 0);
}

static void
polar_gist_redo_page_reuse_record_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	gistxlogPageReuse *xlrec = (gistxlogPageReuse *) XLogRecGetData(record);

	/*
	 * PAGE_REUSE records exist to provide a conflict point when we reuse
	 * pages in the index via the FSM.  That's all they do though.
	 *
	 * latestRemovedXid was the page's deleteXid.  The
	 * GlobalVisCheckRemovableFullXid(deleteXid) test in gistPageRecyclable()
	 * conceptually mirrors the PGPROC->xmin > limitXmin test in
	 * GetConflictingVirtualXIDs().  Consequently, one XID value achieves the
	 * same exclusion effect on primary and standby.
	 */
	if (polar_enable_resolve_conflict && reachedConsistency && InHotStandby)
		ResolveRecoveryConflictWithSnapshotFullXid(xlrec->latestRemovedFullXid,
												   xlrec->node);
}

static void
polar_gist_redo_page_split_record_save(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	int			i;

	for (i = 0; i <= XLogRecMaxBlockId(record); i++)
	{
		if (XLogRecHasBlockRef(record, i + 1))
			polar_logindex_save_block(instance, record, i + 1);
	}

	if (XLogRecHasBlockRef(record, 0))
		polar_logindex_save_block(instance, record, 0);
}

static void
polar_gist_redo_page_split_record_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	BufferTag	first_tag;
	polar_page_lock_t first_lock;
	Buffer		first_buf = InvalidBuffer;
	int			i;

	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 1, first_tag, first_lock, first_buf);

	for (i = 1; i <= XLogRecMaxBlockId(record); i++)
	{
		if (XLogRecHasBlockRef(record, i + 1))
			polar_logindex_redo_parse(instance, record, i + 1);
	}

	if (XLogRecHasBlockRef(record, 0))
		polar_logindex_redo_parse(instance, record, 0);

	if (BufferIsValid(first_buf))
		UnlockReleaseBuffer(first_buf);

	polar_logindex_mini_trans_unlock(instance->mini_trans, first_lock);
}

static void
polar_gist_redo_page_deletep_record_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	BufferTag	parent_tag,
				leaf_tag;
	polar_page_lock_t parent_lock,
				leaf_lock;
	Buffer		parent_buffer,
				leaf_buffer;

	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 0, leaf_tag, leaf_lock, leaf_buffer);
	POLAR_MINI_TRANS_REDO_PARSE(instance, record, 1, parent_tag, parent_lock, parent_buffer);

	if (BufferIsValid(parent_buffer))
		UnlockReleaseBuffer(parent_buffer);
	polar_logindex_mini_trans_unlock(instance->mini_trans, parent_lock);

	if (BufferIsValid(leaf_buffer))
		UnlockReleaseBuffer(leaf_buffer);
	polar_logindex_mini_trans_unlock(instance->mini_trans, leaf_lock);
}

/*
 * Replay the clearing of F_FOLLOW_RIGHT flag on a child page.
 *
 * Even if the WAL record includes a full-page image, we have to update the
 * follow-right flag, because that change is not included in the full-page
 * image.  To be sure that the intermediate state with the wrong flag value is
 * not visible to concurrent Hot Standby queries, this function handles
 * restoring the full-page image as well as updating the flag.  (Note that
 * we never need to do anything else to the child page in the current WAL
 * action.)
 */
static XLogRedoAction
polar_gist_redo_clear_follow_right(XLogReaderState *record, uint8 block_id, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	XLogRedoAction action = BLK_NOTFOUND;
	Page		page;

	/*
	 * Note that we still update the page even if it was restored from a full
	 * page image, because the updated NSN is not included in the image.
	 */
	action = POLAR_READ_BUFFER_FOR_REDO(record, block_id, buffer);

	if (action == BLK_NEEDS_REDO || action == BLK_RESTORED)
	{
		page = BufferGetPage(*buffer);

		GistPageSetNSN(page, lsn);
		GistClearFollowRight(page);

		PageSetLSN(page, lsn);
	}

	return action;
}

/*
 * redo any page update (except page split)
 */
static XLogRedoAction
polar_gist_redo_page_update_record(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageUpdate *xldata = (gistxlogPageUpdate *) XLogRecGetData(record);
	XLogRedoAction action = BLK_NOTFOUND;
	Page		page;
	BufferTag	update_tag;

	POLAR_GET_LOG_TAG(record, update_tag, 0);

	if (BUFFERTAGS_EQUAL(*tag, update_tag))
	{
		action = POLAR_READ_BUFFER_FOR_REDO(record, 0, buffer);

		if (action == BLK_NEEDS_REDO)
		{
			char	   *begin;
			char	   *data;
			Size		datalen;
			int			ninserted = 0;

			data = begin = XLogRecGetBlockData(record, 0, &datalen);

			page = (Page) BufferGetPage(*buffer);

			if (xldata->ntodelete == 1 && xldata->ntoinsert == 1)
			{
				/*
				 * When replacing one tuple with one other tuple, we must use
				 * PageIndexTupleOverwrite for consistency with
				 * gistplacetopage.
				 */
				OffsetNumber offnum = *((OffsetNumber *) data);
				IndexTuple	itup;
				Size		itupsize;

				data += sizeof(OffsetNumber);
				itup = (IndexTuple) data;
				itupsize = IndexTupleSize(itup);

				if (!PageIndexTupleOverwrite(page, offnum, (Item) itup, itupsize))
				{
					POLAR_LOG_REDO_INFO(page, record);
					elog(ERROR, "failed to add item to GiST index page, size %d bytes",
						 (int) itupsize);
				}

				data += itupsize;
				/* should be nothing left after consuming 1 tuple */
				POLAR_ASSERT_PANIC(data - begin == datalen);
				/* update insertion count for POLAR_ASSERT_PANIC check below */
				ninserted++;
			}
			else if (xldata->ntodelete > 0)
			{
				/* Otherwise, delete old tuples if any */
				OffsetNumber *todelete = (OffsetNumber *) data;

				data += sizeof(OffsetNumber) * xldata->ntodelete;

				PageIndexMultiDelete(page, todelete, xldata->ntodelete);
			}

			/* Add new tuples if any */
			if (data - begin < datalen)
			{
				OffsetNumber off = (PageIsEmpty(page)) ? FirstOffsetNumber :
					OffsetNumberNext(PageGetMaxOffsetNumber(page));

				while (data - begin < datalen)
				{
					IndexTuple	itup = (IndexTuple) data;
					Size		sz = IndexTupleSize(itup);
					OffsetNumber l;

					data += sz;

					l = PageAddItem(page, (Item) itup, sz, off, false, false);

					if (l == InvalidOffsetNumber)
					{
						POLAR_LOG_REDO_INFO(page, record);
						elog(ERROR, "failed to add item to GiST index page, size %d bytes",
							 (int) sz);
					}

					off++;
					ninserted++;
				}
			}

			/* Check that XLOG record contained expected number of tuples */
			POLAR_ASSERT_PANIC(ninserted == xldata->ntoinsert);

			PageSetLSN(page, lsn);
		}
	}

	/*
	 * Fix follow-right data on left child page
	 *
	 * This must be done while still holding the lock on the target page. Note
	 * that even if the target page no longer exists, we still attempt to
	 * replay the change on the child page.
	 */
	if (XLogRecHasBlockRef(record, 1))
	{
		BufferTag	right_tag;

		POLAR_GET_LOG_TAG(record, right_tag, 1);

		if (BUFFERTAGS_EQUAL(*tag, right_tag))
			action = polar_gist_redo_clear_follow_right(record, 1, buffer);
	}

	return action;
}

static XLogRedoAction
polar_gist_redo_page_delete_record(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogDelete *xldata = (gistxlogDelete *) XLogRecGetData(record);
	Page		page;
	BufferTag	delete_tag;
	XLogRedoAction action = BLK_NOTFOUND;

	POLAR_GET_LOG_TAG(record, delete_tag, 0);
	if (BUFFERTAGS_EQUAL(*tag, delete_tag))
	{
		action = POLAR_READ_BUFFER_FOR_REDO(record, 0, buffer);

		if (action == BLK_NEEDS_REDO)
		{
			page = (Page) BufferGetPage(*buffer);

			if (XLogRecGetDataLen(record) > SizeOfGistxlogDelete)
			{
				OffsetNumber *todelete;

				todelete = (OffsetNumber *) ((char *) xldata + SizeOfGistxlogDelete);

				PageIndexMultiDelete(page, todelete, xldata->ntodelete);
			}

			GistClearPageHasGarbage(page);

			PageSetLSN(page, lsn);
		}
	}
	return action;
}

static XLogRedoAction
polar_gist_redo_page_split_record(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageSplit *xldata = (gistxlogPageSplit *) XLogRecGetData(record);
	XLogRedoAction action = BLK_NOTFOUND;
	Page		page;
	int			i;
	bool		isrootsplit = false;

	/*
	 * We must hold lock on the first-listed page throughout the action,
	 * including while updating the left child page (if any).  We can unlock
	 * remaining pages in the list as soon as they've been written, because
	 * there is no path for concurrent queries to reach those pages without
	 * first visiting the first-listed page.
	 */

	/* loop around all pages */
	for (i = 0; i < xldata->npage; i++)
	{
		int			flags;
		char	   *data;
		Size		datalen;
		int			num;
		IndexTuple *tuples;
		BufferTag	page_tag;
		int			block_id = i + 1;

		POLAR_GET_LOG_TAG(record, page_tag, block_id);

		if (page_tag.blockNum == GIST_ROOT_BLKNO)
		{
			POLAR_ASSERT_PANIC(i == 0);
			isrootsplit = true;
		}

		if (!BUFFERTAGS_EQUAL(*tag, page_tag))
			continue;

		POLAR_INIT_BUFFER_FOR_REDO(record, block_id, buffer);
		page = (Page) BufferGetPage(*buffer);
		data = XLogRecGetBlockData(record, i + 1, &datalen);

		tuples = decodePageSplitRecord(data, datalen, &num);

		/* ok, clear buffer */
		if (xldata->origleaf && page_tag.blockNum != GIST_ROOT_BLKNO)
			flags = F_LEAF;
		else
			flags = 0;

		GISTInitBuffer(*buffer, flags);

		/* and fill it */
		gistfillbuffer(page, tuples, num, FirstOffsetNumber);

		if (page_tag.blockNum == GIST_ROOT_BLKNO)
		{
			GistPageGetOpaque(page)->rightlink = InvalidBlockNumber;
			GistPageSetNSN(page, xldata->orignsn);
			GistClearFollowRight(page);
		}
		else
		{
			if (i < xldata->npage - 1)
			{
				BlockNumber nextblkno;

				XLogRecGetBlockTag(record, i + 2, NULL, NULL, &nextblkno);
				GistPageGetOpaque(page)->rightlink = nextblkno;
			}
			else
				GistPageGetOpaque(page)->rightlink = xldata->origrlink;

			GistPageSetNSN(page, xldata->orignsn);

			if (i < xldata->npage - 1 && !isrootsplit &&
				xldata->markfollowright)
				GistMarkFollowRight(page);
			else
				GistClearFollowRight(page);
		}

		PageSetLSN(page, lsn);

		return BLK_NEEDS_REDO;
	}


	/* Fix follow-right data on left child page, if any */
	if (XLogRecHasBlockRef(record, 0))
	{
		BufferTag	right_tag;

		POLAR_GET_LOG_TAG(record, right_tag, 0);

		if (BUFFERTAGS_EQUAL(*tag, right_tag))
			action = polar_gist_redo_clear_follow_right(record, 0, buffer);
	}

	return action;
}

static XLogRedoAction
polar_gist_redo_page_deletep_record(XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	XLogRecPtr	lsn = record->EndRecPtr;
	gistxlogPageDelete *xldata = (gistxlogPageDelete *) XLogRecGetData(record);
	BufferTag	delete_tag;
	XLogRedoAction action = BLK_NOTFOUND;

	POLAR_GET_LOG_TAG(record, delete_tag, 0);
	if (BUFFERTAGS_EQUAL(*tag, delete_tag))
	{
		action = POLAR_READ_BUFFER_FOR_REDO(record, 0, buffer);

		if (action == BLK_NEEDS_REDO)
		{
			Page		page = (Page) BufferGetPage(*buffer);

			GistPageSetDeleted(page, xldata->deleteXid);
			PageSetLSN(page, lsn);
		}
		return action;
	}

	POLAR_GET_LOG_TAG(record, delete_tag, 1);
	if (BUFFERTAGS_EQUAL(*tag, delete_tag))
	{
		action = POLAR_READ_BUFFER_FOR_REDO(record, 1, buffer);

		if (action == BLK_NEEDS_REDO)
		{
			Page		page = (Page) BufferGetPage(*buffer);

			PageIndexTupleDelete(page, xldata->downlinkOffset);
			PageSetLSN(page, lsn);
		}
		return action;
	}

	return action;
}

void
polar_gist_idx_save(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			polar_gist_redo_page_update_record_save(instance, record);
			break;

		case XLOG_GIST_DELETE:
			polar_logindex_save_block(instance, record, 0);
			break;

		case XLOG_GIST_PAGE_REUSE:
			/* POLAR: doesn't modify one block. */
			break;

		case XLOG_GIST_PAGE_SPLIT:
			polar_gist_redo_page_split_record_save(instance, record);
			break;

		case XLOG_GIST_PAGE_DELETE:
			polar_logindex_save_block(instance, record, 0);
			polar_logindex_save_block(instance, record, 1);
			break;

		default:
			elog(PANIC, "polar_gist_idx_save: unknown op code %u", info);
			break;
	}
}

bool
polar_gist_idx_parse(polar_logindex_redo_ctl_t instance, XLogReaderState *record)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;

	/*
	 * GiST indexes do not require any conflict processing. NB: If we ever
	 * implement a similar optimization we have in b-tree, and remove killed
	 * tuples outside VACUUM, we'll need to handle that here.
	 */

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			polar_gist_redo_page_update_record_parse(instance, record);
			break;

		case XLOG_GIST_DELETE:
			polar_gist_redo_page_delete_record_parse(instance, record);
			break;

		case XLOG_GIST_PAGE_REUSE:
			polar_gist_redo_page_reuse_record_parse(instance, record);
			break;

		case XLOG_GIST_PAGE_SPLIT:
			polar_gist_redo_page_split_record_parse(instance, record);
			break;

		case XLOG_GIST_PAGE_DELETE:
			polar_gist_redo_page_deletep_record_parse(instance, record);
			break;

		default:
			elog(PANIC, "polar_gist_idx_parse: unknown op code %u", info);
			break;
	}

	return true;
}

XLogRedoAction
polar_gist_idx_redo(polar_logindex_redo_ctl_t instance, XLogReaderState *record, BufferTag *tag, Buffer *buffer)
{
	uint8		info = XLogRecGetInfo(record) & ~XLR_INFO_MASK;
	XLogRedoAction action = BLK_NOTFOUND;

	/*
	 * GiST indexes do not require any conflict processing. NB: If we ever
	 * implement a similar optimization we have in b-tree, and remove killed
	 * tuples outside VACUUM, we'll need to handle that here.
	 */

	switch (info)
	{
		case XLOG_GIST_PAGE_UPDATE:
			action = polar_gist_redo_page_update_record(record, tag, buffer);
			break;

		case XLOG_GIST_DELETE:
			action = polar_gist_redo_page_delete_record(record, tag, buffer);
			break;

		case XLOG_GIST_PAGE_REUSE:
			/* POLAR: doesn't modify one block. */
			break;

		case XLOG_GIST_PAGE_SPLIT:
			action = polar_gist_redo_page_split_record(record, tag, buffer);
			break;

		case XLOG_GIST_PAGE_DELETE:
			action = polar_gist_redo_page_deletep_record(record, tag, buffer);
			break;

		default:
			elog(PANIC, "polar_gist_idx_redo: unknown op code %u", info);
	}

	return action;
}
