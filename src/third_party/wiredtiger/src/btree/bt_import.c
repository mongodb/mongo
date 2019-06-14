/*-
 * Copyright (c) 2014-2019 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_import --
 *	Import a WiredTiger file into the database.
 */
int
__wt_import(WT_SESSION_IMPL *session, const char *uri)
{
	WT_BM *bm;
	WT_CKPT *ckpt, *ckptbase;
	WT_CONFIG_ITEM v;
	WT_DECL_ITEM(a);
	WT_DECL_ITEM(b);
	WT_DECL_ITEM(checkpoint);
	WT_DECL_RET;
	WT_KEYED_ENCRYPTOR *kencryptor;
	const char *filename;
	const char *filecfg[] = {
	   WT_CONFIG_BASE(session, file_meta), NULL, NULL, NULL, NULL, NULL };
	char *checkpoint_list, *fileconf, *metadata, fileid[64];

	ckptbase = NULL;
	checkpoint_list = fileconf = metadata = NULL;

	WT_ERR(__wt_scr_alloc(session, 0, &a));
	WT_ERR(__wt_scr_alloc(session, 0, &b));
	WT_ERR(__wt_scr_alloc(session, 0, &checkpoint));

	WT_ASSERT(session, WT_PREFIX_MATCH(uri, "file:"));
	filename = uri;
	WT_PREFIX_SKIP(filename, "file:");

	/*
	 * Open the file, request block manager checkpoint information.
	 * We don't know the allocation size, but 512B allows us to read
	 * the descriptor block and that's all we care about.
	 */
	WT_ERR(__wt_block_manager_open(
	    session, filename, filecfg, false, true, 512, &bm));
	ret = bm->checkpoint_last(
	    bm, session, &metadata, &checkpoint_list, checkpoint);
	WT_TRET(bm->close(bm, session));
	WT_ERR(ret);
	__wt_verbose(session,
	    WT_VERB_CHECKPOINT, "import metadata: %s", metadata);
	__wt_verbose(session,
	    WT_VERB_CHECKPOINT, "import checkpoint-list: %s", checkpoint_list);

	/*
	 * The metadata may have been encrypted, in which case it's also
	 * hexadecimal encoded. The checkpoint included a boolean value
	 * set if the metadata was encrypted for easier failure diagnosis.
	 */
	WT_ERR(__wt_config_getones(
	    session, metadata, "block_metadata_encrypted", &v));
	WT_ERR(__wt_btree_config_encryptor(session, filecfg, &kencryptor));
	if ((kencryptor == NULL && v.val != 0) ||
	    (kencryptor != NULL && v.val == 0))
		WT_ERR_MSG(session, EINVAL,
		    "%s: loaded object's encryption configuration doesn't "
		    "match the database's encryption configuration",
		    filename);
	/*
	 * The metadata was quoted to avoid configuration string characters
	 * acting as separators. Discard any quote characters.
	 */
	WT_ERR(__wt_config_getones(session, metadata, "block_metadata", &v));
	if (v.len > 0 && (v.str[0] == '[' || v.str[0] == '(')) {
		++v.str;
		v.len -= 2;
	}
	if (kencryptor == NULL) {
		WT_ERR(__wt_buf_grow(session, a, v.len + 1));
		WT_ERR(__wt_buf_set(session, a, v.str, v.len));
		((uint8_t *)a->data)[a->size] = '\0';
	} else {
		WT_ERR(__wt_buf_grow(session, b, v.len));
		WT_ERR(__wt_nhex_to_raw(session, v.str, v.len, b));
		WT_ERR(__wt_buf_grow(session, a, b->size + 1));
		WT_ERR(__wt_decrypt(session, kencryptor->encryptor, 0, b, a));
		((uint8_t *)a->data)[a->size] = '\0';
	}

	/*
	 * OK, we've now got three chunks of data: the file's metadata from when
	 * the last checkpoint started, the array of checkpoints as of when the
	 * last checkpoint was almost complete (everything written but the avail
	 * list), and fixed-up checkpoint information from the last checkpoint.
	 *
	 * Build and flatten the metadata and the checkpoint list, then insert
	 * it into the metadata for this file.
	 *
	 * Strip out the checkpoint-LSN, an imported file isn't associated
	 * with any log files.
	 * Assign a unique file ID.
	 */
	filecfg[1] = a->data;
	filecfg[2] = checkpoint_list;
	filecfg[3] = "checkpoint_lsn=";
	WT_WITH_SCHEMA_LOCK(session, ret =
	    __wt_snprintf(fileid, sizeof(fileid),
	    "id=%" PRIu32, ++S2C(session)->next_file_id));
	WT_ERR(ret);
	filecfg[4] = fileid;
	WT_ERR(__wt_config_collapse(session, filecfg, &fileconf));
	WT_ERR(__wt_metadata_insert(session, uri, fileconf));
	__wt_verbose(session,
	    WT_VERB_CHECKPOINT, "import configuration: %s/%s", uri, fileconf);

	/*
	 * The just inserted metadata was correct as of immediately before the
	 * before the final checkpoint, but it's not quite right. The block
	 * manager returned the corrected final checkpoint, put it all together.
	 *
	 * Get the checkpoint information from the file's metadata as an array
	 * of WT_CKPT structures.
	 *
	 * XXX
	 * There's a problem here. If a file is imported from our future (leaf
	 * pages with unstable entries that have write-generations ahead of the
	 * current database's base write generation), we'll read the values and
	 * treat them as stable. A restart will fix this: when we added the
	 * imported file to our metadata, the write generation in the imported
	 * file's checkpoints updated our database's maximum write generation,
	 * and so a restart will have a maximum generation newer than the
	 * imported file's write generation. An alternative solution is to add
	 * a "base write generation" value to the imported file's metadata, and
	 * use that value instead of the connection's base write generation when
	 * deciding what page items should be read. Since all future writes to
	 * the imported file would be ahead of that write generation, it would
	 * have the effect we want.
	 *
	 * Update the last checkpoint with the corrected information.
	 * Update the file's metadata with the new checkpoint information.
	 */
	WT_ERR(__wt_meta_ckptlist_get(session, uri, false, &ckptbase));
	WT_CKPT_FOREACH(ckptbase, ckpt)
		if (ckpt->name == NULL || (ckpt + 1)->name == NULL)
			break;
	if (ckpt->name == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "no checkpoint information available to import");
	F_SET(ckpt, WT_CKPT_UPDATE);
	WT_ERR(__wt_buf_set(
	    session, &ckpt->raw, checkpoint->data, checkpoint->size));
	WT_ERR(__wt_meta_ckptlist_set(session, uri, ckptbase, NULL));

err:
	__wt_meta_ckptlist_free(session, &ckptbase);

	__wt_free(session, fileconf);
	__wt_free(session, metadata);
	__wt_free(session, checkpoint_list);

	__wt_scr_free(session, &a);
	__wt_scr_free(session, &b);
	__wt_scr_free(session, &checkpoint);

	return (ret);
}
