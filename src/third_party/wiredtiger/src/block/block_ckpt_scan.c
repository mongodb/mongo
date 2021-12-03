/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * It wasn't possible to open standalone files in historic WiredTiger databases, you're done if you
 * lose the file's associated metadata. That was a mistake and this code is the workaround. What we
 * need to crack a file is database metadata plus a list of active checkpoints as of the file's
 * clean shutdown (normally stored in the database metadata). The last write done in a block
 * manager's checkpoint is the avail list. If current metadata and checkpoint information is
 * included in that write, we're close. We can open the file, read the blocks, scan until we find
 * the avail list, and read the metadata and checkpoint information from there.
 *	Two problems remain: first, the checkpoint information isn't correct until we write the
 * avail list and the checkpoint information has to include the avail list address plus the final
 * file size after the write. Fortunately, when scanning the file for the avail lists, we're
 * figuring out exactly the information needed to fix up the checkpoint information we wrote, that
 * is, the avail list's offset, size and checksum triplet. As for the final file size, we allocate
 * all space in the file before we calculate block checksums, so we can do that space allocation,
 * then fill in the final file size before calculating the checksum and writing the actual block.
 *  The second problem is we have to be able to find the avail lists that include checkpoint
 * information (ignoring previous files created by previous releases, and, of course, making
 * upgrade/downgrade work seamlessly). Extent lists are written to their own pages, and we could
 * version this change using the page header version. Happily, historic WiredTiger releases have a
 * bug. Extent lists consist of a set of offset/size pairs, with magic offset/size pairs at the
 * beginning and end of the list. Historic releases only verified the offset of the special pair at
 * the end of the list, ignoring the size. To detect avail lists that include appended metadata and
 * checkpoint information, this change adds a version to the extent list: if size is
 * WT_BLOCK_EXTLIST_VERSION_CKPT, then metadata/checkpoint information follows.
 */

/*
 * __wt_block_checkpoint_final --
 *     Append metadata and checkpoint information to a buffer.
 */
int
__wt_block_checkpoint_final(
  WT_SESSION_IMPL *session, WT_BLOCK *block, WT_ITEM *buf, uint8_t **file_sizep)
{
    WT_CKPT *ckpt;
    size_t align_size, file_size_offset, len, size;
    uint8_t *p;

    *file_sizep = 0;

    ckpt = block->final_ckpt;
    p = (uint8_t *)buf->mem + buf->size;

    /*
     * First, add in a counter to uniquely order checkpoints at our level.
     * There's order and time information in the checkpoint itself, but the
     * order isn't written and the time is only at second granularity.
     *	I'm using the Btree write generation for this purpose. That's
     * safe and guaranteed correct because everything is locked down for the
     * checkpoint, we're the only writer. Plus, because we use the write
     * generation as a database connection generation, it's guaranteed to
     * move forward and never repeat.
     *	It's a layering violation though, this is the only place the
     * block manager uses the write generation. The alternative would be to
     * add our own write-generation scheme in the block manager, storing a
     * value and recovering it when we open the file. We could do that, as
     * reading the final avail list when a file is opened is unavoidable,
     * so we can retrieve the value written here when we open the file, but
     * this approach is simpler.
     */
    size = buf->size + WT_INTPACK64_MAXSIZE;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    WT_RET(__wt_vpack_uint(&p, 0, ++S2BT(session)->write_gen));
    buf->size = WT_PTRDIFF(p, buf->mem);

    /*
     * Second, add space for the final file size as a packed value. We don't know how large it will
     * be so skip the maximum required space.
     */
    size = buf->size + WT_INTPACK64_MAXSIZE;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    memset(p, 0, WT_INTPACK64_MAXSIZE);
    file_size_offset = buf->size;
    buf->size = size;

    /* 3a, copy the metadata length into the buffer. */
    len = strlen(ckpt->block_metadata);
    size = buf->size + WT_INTPACK64_MAXSIZE;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    WT_RET(__wt_vpack_uint(&p, 0, (uint64_t)len));
    buf->size = WT_PTRDIFF(p, buf->mem);

    /* 3b, copy the metadata into the buffer. */
    size = buf->size + len;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    memcpy(p, ckpt->block_metadata, len);
    buf->size = size;

    /* 4a, copy the checkpoint list length into the buffer. */
    len = strlen(ckpt->block_checkpoint);
    size = buf->size + WT_INTPACK64_MAXSIZE;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    WT_RET(__wt_vpack_uint(&p, 0, (uint64_t)len));
    buf->size = WT_PTRDIFF(p, buf->mem);

    /* 4b, copy the checkpoint list into the buffer. */
    size = buf->size + len;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    memcpy(p, ckpt->block_checkpoint, len);
    buf->size = size;

    /*
     * 5a, copy the not-quite-right checkpoint information length into the
     * buffer.
     */
    len = ckpt->raw.size;
    size = buf->size + WT_INTPACK64_MAXSIZE;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    WT_RET(__wt_vpack_uint(&p, 0, (uint64_t)len));
    buf->size = WT_PTRDIFF(p, buf->mem);

    /*
     * 5b, copy the not-quite-right checkpoint information into the buffer.
     */
    size = buf->size + len;
    WT_RET(__wt_buf_extend(session, buf, size));
    p = (uint8_t *)buf->mem + buf->size;
    memcpy(p, ckpt->raw.data, len);
    buf->size = size;

    /*
     * We might have grown the buffer beyond the original allocation size, make sure that we're
     * still in compliance.
     */
    align_size = WT_ALIGN(buf->size, block->allocsize);
    if (align_size > buf->memsize)
        WT_RET(__wt_buf_extend(session, buf, align_size));

    *file_sizep = (uint8_t *)buf->mem + file_size_offset;

    return (0);
}

struct saved_block_info {
    uint64_t write_gen;
    wt_off_t offset;
    uint32_t size;
    uint32_t checksum;
    uint64_t file_size;

    char *metadata;
    char *checkpoint_list;

    WT_ITEM *checkpoint;
};

/*
 * __block_checkpoint_update --
 *     Update the checkpoint information for the file.
 */
static int
__block_checkpoint_update(WT_SESSION_IMPL *session, WT_BLOCK *block, struct saved_block_info *info)
{
    WT_BLOCK_CKPT ci;
    WT_ITEM *checkpoint;
    uint8_t *endp;

    memset(&ci, 0, sizeof(ci));
    checkpoint = info->checkpoint;

    if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
        __wt_ckpt_verbose(
          session, block, "import original", NULL, checkpoint->mem, checkpoint->size);

    /*
     * Convert the final checkpoint data blob to a WT_BLOCK_CKPT structure, update it with the avail
     * list information, and convert it back to a data blob.
     */
    WT_RET(__wt_block_ckpt_unpack(session, block, checkpoint->data, checkpoint->size, &ci));
    ci.avail.offset = info->offset;
    ci.avail.size = info->size;
    ci.avail.checksum = info->checksum;
    ci.file_size = (wt_off_t)info->file_size;
    WT_RET(__wt_buf_extend(session, checkpoint, WT_BLOCK_CHECKPOINT_BUFFER));
    endp = checkpoint->mem;
    WT_RET(__wt_block_ckpt_pack(session, block, &endp, &ci, false));
    checkpoint->size = WT_PTRDIFF(endp, checkpoint->mem);

    if (WT_VERBOSE_ISSET(session, WT_VERB_CHECKPOINT))
        __wt_ckpt_verbose(
          session, block, "import replace", NULL, checkpoint->mem, checkpoint->size);

    return (0);
}

#define WT_BLOCK_SKIP(a) \
    do {                 \
        if ((a) != 0)    \
            continue;    \
    } while (0)

/*
 * __wt_block_checkpoint_last --
 *     Scan a file for checkpoints, returning the last one we find.
 */
int
__wt_block_checkpoint_last(WT_SESSION_IMPL *session, WT_BLOCK *block, char **metadatap,
  char **checkpoint_listp, WT_ITEM *checkpoint)
{
    struct saved_block_info *best, _best, *current, _current, *saved_tmp;
    WT_BLOCK_HEADER *blk;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_FH *fh;
    const WT_PAGE_HEADER *dsk;
    wt_off_t ext_off, ext_size, offset;
    uint64_t len, nblocks, write_gen;
    uint32_t checksum, objectid, size;
    const uint8_t *p, *t;
    bool found;

    *metadatap = *checkpoint_listp = NULL;
    WT_RET(__wt_buf_init(session, checkpoint, WT_BLOCK_CHECKPOINT_BUFFER));

    /* Tiered tables aren't supported yet. */
    objectid = 0;

    /*
     * Initialize a pair of structures that track the best and current checkpoints found so far.
     * This is a little trickier than normal because we don't want to start saving a checkpoint only
     * to find out it's not one we can use. I doubt that can happen and it suggests corruption, but
     * half-a-checkpoint isn't a good place to be. Only swap to a new "best" checkpoint if we read
     * the whole thing successfully.
     *
     * Don't re-order these lines: it's done this way so the WT_ITEMs are always initialized and
     * error handling works.
     */
    memset((best = &_best), 0, sizeof(_best));
    memset((current = &_current), 0, sizeof(_current));
    WT_ERR(__wt_scr_alloc(session, 0, &best->checkpoint));
    WT_ERR(__wt_scr_alloc(session, 0, &current->checkpoint));

    found = false;
    ext_off = 0; /* [-Werror=maybe-uninitialized] */
    ext_size = 0;
    len = write_gen = 0;

    WT_ERR(__wt_scr_alloc(session, 64 * 1024, &tmp));

    F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);

    /*
     * Scan the file for pages, using the minimum possible WiredTiger allocation size.
     */
    fh = block->fh;
    for (nblocks = 0, offset = 0; offset < block->size; offset += size) {
/* Report progress occasionally. */
#define WT_CHECKPOINT_LIST_PROGRESS_INTERVAL 100
        if (++nblocks % WT_CHECKPOINT_LIST_PROGRESS_INTERVAL == 0)
            WT_ERR(__wt_progress(session, NULL, nblocks));

        /*
         * Read the start of a possible page and get a block length from it. Move to the next
         * allocation sized boundary, we'll never consider this one again.
         */
        if (__wt_read(session, fh, offset, (size_t)WT_BTREE_MIN_ALLOC_SIZE, tmp->mem) != 0)
            break;
        blk = WT_BLOCK_HEADER_REF(tmp->mem);
        __wt_block_header_byteswap(blk);
        size = blk->disk_size;
        checksum = blk->checksum;

        /*
         * Check the block size: if it's not insane, read the block. Reading the block validates any
         * checksum. The file might reasonably have garbage at the end, and we're not here to detect
         * that. Ignore problems, subsequent file verification can deal with any corruption. If the
         * block isn't valid, skip to the next possible block.
         */
        if (__wt_block_offset_invalid(block, offset, size) ||
          __wt_block_read_off(session, block, tmp, objectid, offset, size, checksum) != 0) {
            size = WT_BTREE_MIN_ALLOC_SIZE;
            continue;
        }

        dsk = tmp->mem;
        if (dsk->type != WT_PAGE_BLOCK_MANAGER)
            continue;

        p = WT_BLOCK_HEADER_BYTE(tmp->mem);
        WT_BLOCK_SKIP(__wt_extlist_read_pair(&p, &ext_off, &ext_size));
        if (ext_off != WT_BLOCK_EXTLIST_MAGIC || ext_size != 0)
            continue;
        for (;;) {
            if ((ret = __wt_extlist_read_pair(&p, &ext_off, &ext_size)) != 0)
                break;
            if (ext_off == WT_BLOCK_INVALID_OFFSET)
                break;
        }
        if (ret != 0) {
            WT_NOT_READ(ret, 0);
            continue;
        }
        /*
         * Note the less-than check of WT_BLOCK_EXTLIST_VERSION_CKPT, that way we can extend this
         * with additional values in the future.
         */
        if (ext_size < WT_BLOCK_EXTLIST_VERSION_CKPT)
            continue;

        /*
         * Skip any entries that aren't the most recent we've seen so far.
         */
        WT_BLOCK_SKIP(__wt_vunpack_uint(&p, 0, &write_gen));
        if (write_gen < best->write_gen)
            continue;

        __wt_verbose(session, WT_VERB_CHECKPOINT,
          "scan: checkpoint block at offset %" PRIuMAX ", generation #%" PRIu64, (uintmax_t)offset,
          write_gen);

        current->write_gen = write_gen;
        current->offset = offset;
        current->size = size;
        current->checksum = checksum;

        /*
         * The file size is in a fixed-size chunk of data, although it's packed (for portability).
         */
        t = p;
        WT_BLOCK_SKIP(__wt_vunpack_uint(&t, 0, &current->file_size));
        p += WT_INTPACK64_MAXSIZE;

        /* Save a copy of the metadata. */
        __wt_free(session, current->metadata);
        WT_BLOCK_SKIP(__wt_vunpack_uint(&p, 0, &len));
        WT_ERR(__wt_strndup(session, p, len, &current->metadata));
        p += len;

        /* Save a copy of the checkpoint list. */
        __wt_free(session, current->checkpoint_list);
        WT_BLOCK_SKIP(__wt_vunpack_uint(&p, 0, &len));
        WT_ERR(__wt_strndup(session, p, len, &current->checkpoint_list));
        p += len;

        /* Save a copy of the checkpoint information. */
        WT_BLOCK_SKIP(__wt_vunpack_uint(&p, 0, &len));
        WT_ERR(__wt_buf_set(session, current->checkpoint, p, len));

        /* A new winner, swap the "best" and "current" information. */
        saved_tmp = best;
        best = current;
        current = saved_tmp;
        found = true;
    }

    if (!found)
        WT_ERR_MSG(session, WT_NOTFOUND, "%s: no final checkpoint found in file scan", block->name);

    /* Correct the checkpoint. */
    WT_ERR(__block_checkpoint_update(session, block, best));

    /*
     * Copy the information out to our caller. Do the WT_ITEM first, it's the only thing left that
     * can fail and simplifies error handling.
     */
    WT_ERR(__wt_buf_set(session, checkpoint, best->checkpoint->data, best->checkpoint->size));
    *metadatap = best->metadata;
    best->metadata = NULL;
    *checkpoint_listp = best->checkpoint_list;
    best->checkpoint_list = NULL;

err:
    __wt_free(session, best->metadata);
    __wt_free(session, best->checkpoint_list);
    __wt_scr_free(session, &best->checkpoint);
    __wt_free(session, current->metadata);
    __wt_free(session, current->checkpoint_list);
    __wt_scr_free(session, &current->checkpoint);

    __wt_scr_free(session, &tmp);

    F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);
    return (ret);
}
