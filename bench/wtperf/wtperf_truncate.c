/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include "wtperf.h"

static inline uint64_t
decode_key(char *key_buf)
{
	return (strtoull(key_buf, NULL, 10));
}

int
setup_truncate(CONFIG *cfg, CONFIG_THREAD *thread, WT_SESSION *session) {

	TRUNCATE_CONFIG *trunc_cfg;
	TRUNCATE_QUEUE_ENTRY *truncate_item;
	WORKLOAD *workload;
	WT_CURSOR *cursor;
	char *key;
	int ret;
	uint64_t end_point, final_stone_gap, i, start_point;

	end_point = final_stone_gap = start_point = 0;
	trunc_cfg = &thread->trunc_cfg;
	workload = thread->workload;

	/* We are limited to only one table when running truncate. */
	if ((ret = session->open_cursor(
	    session, cfg->uris[0], NULL, NULL, &cursor)) != 0)
		goto err;

	/*
	 * If we find the workload getting behind we multiply the number of
	 * records to be truncated.
	 */
	trunc_cfg->catchup_multiplier = 1;

	/* How many entries between each stone. */
	trunc_cfg->stone_gap =
	    (workload->truncate_count * workload->truncate_pct) / 100;
	/* How many stones we need. */
	trunc_cfg->needed_stones =
	    workload->truncate_count / trunc_cfg->stone_gap;

	final_stone_gap = trunc_cfg->stone_gap;

	/* Reset this value for use again. */
	trunc_cfg->stone_gap = 0;

	/*
	 * Here we check if there is data in the collection. If there is
	 * data available, then we need to setup some initial truncation
	 * stones.
	 */
	if ((ret = cursor->next(cursor)) != 0 ||
	    (ret = cursor->get_key(cursor, &key)) != 0) {
		lprintf(cfg, ret, 0, "truncate setup start: failed");
		goto err;
	}

	start_point = decode_key(key);
	if ((cursor->reset(cursor)) != 0 || (ret = cursor->prev(cursor)) != 0 ||
	    (ret = cursor->get_key(cursor, &key)) != 0) {
		lprintf(cfg, ret, 0, "truncate setup end: failed");
		goto err;
	}
	end_point = decode_key(key);

	/* Assign stones if there are enough documents. */
	if (start_point + trunc_cfg->needed_stones > end_point)
		trunc_cfg->stone_gap = 0;
	else
		trunc_cfg->stone_gap =
		    (end_point - start_point) / trunc_cfg->needed_stones;

	/* If we have enough data allocate some stones. */
	if (trunc_cfg->stone_gap != 0) {
		trunc_cfg->expected_total = (end_point - start_point);
		for (i = 1; i <= trunc_cfg->needed_stones; i++) {
			truncate_item =
			    dcalloc(sizeof(TRUNCATE_QUEUE_ENTRY), 1);
			truncate_item->key = dcalloc(cfg->key_sz, 1);
			generate_key(
			    cfg, truncate_item->key, trunc_cfg->stone_gap * i);
			truncate_item->diff =
			    (trunc_cfg->stone_gap * i) - trunc_cfg->last_key;
			TAILQ_INSERT_TAIL(&cfg->stone_head, truncate_item, q);
			trunc_cfg->last_key = trunc_cfg->stone_gap * i;
			trunc_cfg->num_stones++;
		}
	}
	trunc_cfg->stone_gap = final_stone_gap;

err:	if ((ret = cursor->close(cursor)) != 0) {
		lprintf(cfg, ret, 0, "truncate setup: cursor close failed");
	}
	return (ret);
}

int
run_truncate(CONFIG *cfg, CONFIG_THREAD *thread,
    WT_CURSOR *cursor, WT_SESSION *session, int *truncatedp) {

	TRUNCATE_CONFIG *trunc_cfg;
	TRUNCATE_QUEUE_ENTRY *truncate_item;
	int ret, t_ret;
	uint64_t used_stone_gap;

	ret = 0;
	trunc_cfg = &thread->trunc_cfg;

	*truncatedp = 0;
	/* Update the total inserts */
	trunc_cfg->total_inserts = sum_insert_ops(cfg);
	trunc_cfg->expected_total +=
	    (trunc_cfg->total_inserts - trunc_cfg->last_total_inserts);
	trunc_cfg->last_total_inserts = trunc_cfg->total_inserts;

	/* We are done if there isn't enough data to trigger a new milestone. */
	if (trunc_cfg->expected_total <= thread->workload->truncate_count)
		return (0);

	/*
	 * If we are falling behind and using more than one stone per lap we
	 * should widen the stone gap for this lap to try and catch up quicker.
	 */
	if (trunc_cfg->expected_total >
	    thread->workload->truncate_count + trunc_cfg->stone_gap) {
		/*
		 * Increase the multiplier until we create stones that are
		 * almost large enough to truncate the whole expected table size
		 * in one operation.
		 */
		trunc_cfg->catchup_multiplier =
		    WT_MIN(trunc_cfg->catchup_multiplier + 1,
		    trunc_cfg->needed_stones - 1);
	} else {
		/* Back off if we start seeing an improvement */
		trunc_cfg->catchup_multiplier =
		    WT_MAX(trunc_cfg->catchup_multiplier - 1, 1);
	}
	used_stone_gap = trunc_cfg->stone_gap * trunc_cfg->catchup_multiplier;

	while (trunc_cfg->num_stones < trunc_cfg->needed_stones) {
		trunc_cfg->last_key += used_stone_gap;
		truncate_item = dcalloc(sizeof(TRUNCATE_QUEUE_ENTRY), 1);
		truncate_item->key = dcalloc(cfg->key_sz, 1);
		generate_key(cfg, truncate_item->key, trunc_cfg->last_key);
		truncate_item->diff = used_stone_gap;
		TAILQ_INSERT_TAIL(&cfg->stone_head, truncate_item, q);
		trunc_cfg->num_stones++;
	}

	/* We are done if there isn't enough data to trigger a truncate. */
	if (trunc_cfg->num_stones == 0 ||
	    trunc_cfg->expected_total <= thread->workload->truncate_count)
		return (0);

	truncate_item = TAILQ_FIRST(&cfg->stone_head);
	trunc_cfg->num_stones--;
	TAILQ_REMOVE(&cfg->stone_head, truncate_item, q);
	cursor->set_key(cursor,truncate_item->key);
	if ((ret = cursor->search(cursor)) != 0) {
		lprintf(cfg, ret, 0, "Truncate search: failed");
		goto err;
	}

	if ((ret = session->truncate(session, NULL, NULL, cursor, NULL)) != 0) {
		lprintf(cfg, ret, 0, "Truncate: failed");
		goto err;
	}

	*truncatedp = 1;
	trunc_cfg->expected_total -= truncate_item->diff;

err:	free(truncate_item->key);
	free(truncate_item);
	t_ret = cursor->reset(cursor);
	if (t_ret != 0)
		lprintf(cfg, t_ret, 0, "Cursor reset failed");
	if (ret == 0 && t_ret != 0)
		ret = t_ret;
	return (ret);
}

void
cleanup_truncate_config(CONFIG *cfg) {
	TRUNCATE_QUEUE_ENTRY *truncate_item;

	while (!TAILQ_EMPTY(&cfg->stone_head)) {
		truncate_item = TAILQ_FIRST(&cfg->stone_head);
		TAILQ_REMOVE(&cfg->stone_head, truncate_item, q);
		free(truncate_item->key);
		free(truncate_item);
	}
}
