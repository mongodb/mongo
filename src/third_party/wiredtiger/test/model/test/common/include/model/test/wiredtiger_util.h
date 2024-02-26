/*-
 * Public Domain 2014-present MongoDB, Inc.
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

#pragma once

#include "model/data_value.h"
#include "model/kv_database.h"
#include "wiredtiger.h"

/*
 * wt_get --
 *     Read from WiredTiger.
 */
model::data_value wt_get(WT_SESSION *session, const char *uri, const model::data_value &key,
  model::timestamp_t timestamp = model::k_timestamp_latest);

/*
 * wt_get_ext --
 *     Read from WiredTiger, but also return the error code.
 */
int wt_get_ext(WT_SESSION *session, const char *uri, const model::data_value &key,
  model::data_value &out, model::timestamp_t timestamp = model::k_timestamp_latest);

/*
 * wt_insert --
 *     Write to WiredTiger.
 */
int wt_insert(WT_SESSION *session, const char *uri, const model::data_value &key,
  const model::data_value &value, model::timestamp_t timestamp = 0, bool overwrite = true);

/*
 * wt_remove --
 *     Delete from WiredTiger.
 */
int wt_remove(WT_SESSION *session, const char *uri, const model::data_value &key,
  model::timestamp_t timestamp = 0);

/*
 * wt_truncate --
 *     Truncate a key range in WiredTiger.
 */
int wt_truncate(WT_SESSION *session, const char *uri, const model::data_value &start,
  const model::data_value &stop, model::timestamp_t timestamp = 0);

/*
 * wt_update --
 *     Update a key in WiredTiger.
 */
int wt_update(WT_SESSION *session, const char *uri, const model::data_value &key,
  const model::data_value &value, model::timestamp_t timestamp = 0, bool overwrite = true);

/*
 * wt_txn_begin --
 *     Begin a transaction.
 */
void wt_txn_begin(
  WT_SESSION *session, model::timestamp_t read_timestamp = model::k_timestamp_latest);

/*
 * wt_txn_commit --
 *     Commit a transaction.
 */
void wt_txn_commit(WT_SESSION *session,
  model::timestamp_t commit_timestamp = model::k_timestamp_none,
  model::timestamp_t durable_timestamp = model::k_timestamp_none);

/*
 * wt_txn_prepare --
 *     Prepare a transaction.
 */
void wt_txn_prepare(WT_SESSION *session, model::timestamp_t prepare_timestamp);

/*
 * wt_txn_reset_snapshot --
 *     Reset the transaction snapshot.
 */
void wt_txn_reset_snapshot(WT_SESSION *session);

/*
 * wt_txn_rollback --
 *     Roll back a transaction.
 */
void wt_txn_rollback(WT_SESSION *session);

/*
 * wt_txn_set_commit_timestamp --
 *     Set the commit timestamp for all subsequent updates.
 */
void wt_txn_set_commit_timestamp(WT_SESSION *session, model::timestamp_t commit_timestamp);

/*
 * wt_txn_get --
 *     Read from WiredTiger.
 */
model::data_value wt_txn_get(WT_SESSION *session, const char *uri, const model::data_value &key);

/*
 * wt_txn_insert --
 *     Write to WiredTiger.
 */
int wt_txn_insert(WT_SESSION *session, const char *uri, const model::data_value &key,
  const model::data_value &value, bool overwrite = true);

/*
 * wt_ckpt_get --
 *     Read from WiredTiger.
 */
model::data_value wt_ckpt_get(WT_SESSION *session, const char *uri, const model::data_value &key,
  const char *ckpt_name = nullptr,
  model::timestamp_t debug_read_timestamp = model::k_timestamp_none);

/*
 * wt_ckpt_create --
 *     Create a WiredTiger checkpoint.
 */
void wt_ckpt_create(WT_SESSION *session, const char *ckpt_name = nullptr);

/*
 * wt_get_stable_timestamp --
 *     Get the stable timestamp in WiredTiger.
 */
model::timestamp_t wt_get_stable_timestamp(WT_CONNECTION *conn);

/*
 * wt_set_stable_timestamp --
 *     Set the stable timestamp in WiredTiger.
 */
void wt_set_stable_timestamp(WT_CONNECTION *conn, model::timestamp_t timestamp);

/*
 * wt_print_debug_log --
 *     Print the contents of a debug log to a file.
 */
void wt_print_debug_log(WT_CONNECTION *conn, const char *file);

/*
 * wt_rollback_to_stable --
 *     Rollback to stable.
 */
inline void
wt_rollback_to_stable(WT_CONNECTION *conn)
{
    testutil_check(conn->rollback_to_stable(conn, nullptr));
}

/*
 * wt_model_assert --
 *     Check that the key has the same value in the model as in the database.
 */
#define wt_model_assert(table, uri, key, ...)                              \
    {                                                                      \
        model::data_value __out_model, __out_wt;                           \
        int __ret_model, __ret_wt;                                         \
        __ret_model = table->get_ext(key, __out_model, ##__VA_ARGS__);     \
        __ret_wt = wt_get_ext(session, uri, key, __out_wt, ##__VA_ARGS__); \
        testutil_assert(__ret_model == __ret_wt);                          \
        testutil_assert(__out_model == __out_wt);                          \
    }

/*
 * wt_model_insert_both --
 *     Insert both into the model and the database.
 */
#define wt_model_insert_both(table, uri, key, value, ...)       \
    testutil_assert(table->insert(key, value, ##__VA_ARGS__) == \
      wt_insert(session, uri, key, value, ##__VA_ARGS__));

/*
 * wt_model_remove_both --
 *     Remove both from the model and from the database.
 */
#define wt_model_remove_both(table, uri, key, ...) \
    testutil_assert(                               \
      table->remove(key, ##__VA_ARGS__) == wt_remove(session, uri, key, ##__VA_ARGS__));

/*
 * wt_model_truncate_both --
 *     Truncate in both from the model and from the database.
 */
#define wt_model_truncate_both(table, uri, start, ...) \
    testutil_assert(                                   \
      table->truncate(start, ##__VA_ARGS__) == wt_truncate(session, uri, start, ##__VA_ARGS__));

/*
 * wt_model_update_both --
 *     Update both in the model and in the database.
 */
#define wt_model_update_both(table, uri, key, value, ...)       \
    testutil_assert(table->update(key, value, ##__VA_ARGS__) == \
      wt_update(session, uri, key, value, ##__VA_ARGS__));

/*
 * wt_model_txn_assert --
 *     Check that the key has the same value in the model as in the database.
 */
#define wt_model_txn_assert(table, uri, txn, session, key, ...) \
    testutil_assert(                                            \
      table->get(txn, key, ##__VA_ARGS__) == wt_txn_get(session, uri, key, ##__VA_ARGS__));

/*
 * wt_model_txn_begin_both --
 *     Begin transaction in both the model and the database.
 */
#define wt_model_txn_begin_both(txn, session, ...)     \
    {                                                  \
        wt_txn_begin(session, ##__VA_ARGS__);          \
        txn = database.begin_transaction(__VA_ARGS__); \
    }

/*
 * wt_model_txn_commit_both --
 *     Commit transaction in both the model and the database.
 */
#define wt_model_txn_commit_both(txn, session, ...) \
    {                                               \
        wt_txn_commit(session, ##__VA_ARGS__);      \
        txn->commit(__VA_ARGS__);                   \
    }

/*
 * wt_model_txn_prepare_both --
 *     Prepare transaction in both the model and the database.
 */
#define wt_model_txn_prepare_both(txn, session, ...) \
    {                                                \
        wt_txn_prepare(session, ##__VA_ARGS__);      \
        txn->prepare(__VA_ARGS__);                   \
    }

/*
 * wt_model_txn_reset_snapshot_both --
 *     Reset transaction snapshot in both the model and the database.
 */
#define wt_model_txn_reset_snapshot_both(txn, session, ...) \
    {                                                       \
        wt_txn_reset_snapshot(session, ##__VA_ARGS__);      \
        txn->reset_snapshot(__VA_ARGS__);                   \
    }

/*
 * wt_model_txn_rollback_both --
 *     Roll back transaction in both the model and the database.
 */
#define wt_model_txn_rollback_both(txn, session, ...) \
    {                                                 \
        wt_txn_rollback(session, ##__VA_ARGS__);      \
        txn->rollback(__VA_ARGS__);                   \
    }

/*
 * wt_model_txn_set_timestamp_both --
 *     Set the timestamp in both the model and the database.
 */
#define wt_model_txn_set_timestamp_both(txn, session, ...)   \
    {                                                        \
        wt_txn_set_commit_timestamp(session, ##__VA_ARGS__); \
        txn->set_commit_timestamp(__VA_ARGS__);              \
    }

/*
 * wt_model_insert_both --
 *     Insert both into the model and the database.
 */
#define wt_model_txn_insert_both(table, uri, txn, session, key, value, ...) \
    testutil_assert(table->insert(txn, key, value, ##__VA_ARGS__) ==        \
      wt_txn_insert(session, uri, key, value, ##__VA_ARGS__));

/*
 * wt_model_ckpt_assert --
 *     Check that the key has the same value in the model as in the database.
 */
#define wt_model_ckpt_assert(table, uri, ckpt_name, key, ...)                         \
    testutil_assert(table->get(database.checkpoint(ckpt_name), key, ##__VA_ARGS__) == \
      wt_ckpt_get(session, uri, key, ckpt_name, ##__VA_ARGS__));

/*
 * wt_model_ckpt_create_both --
 *     Create a checkpoint in both the model and the database.
 */
#define wt_model_ckpt_create_both(...)           \
    {                                            \
        wt_ckpt_create(session, ##__VA_ARGS__);  \
        database.create_checkpoint(__VA_ARGS__); \
    }

/*
 * wt_model_set_stable_timestamp_both --
 *     Set the stable timestamp in both the model and the database.
 */
#define wt_model_set_stable_timestamp_both(timestamp) \
    {                                                 \
        wt_set_stable_timestamp(conn, timestamp);     \
        database.set_stable_timestamp(timestamp);     \
    }

/*
 * wt_model_rollback_to_stable_both --
 *     Rollback to stable in both the model and the database.
 */
#define wt_model_rollback_to_stable_both() \
    {                                      \
        wt_rollback_to_stable(conn);       \
        database.rollback_to_stable();     \
    }
