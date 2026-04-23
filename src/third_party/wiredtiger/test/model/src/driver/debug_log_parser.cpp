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

#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>

using json = nlohmann::json;

extern "C" {
#include "wt_internal.h"
}

#include "model/driver/debug_log_parser.h"
#include "model/util.h"

namespace model {

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::col_put &out)
{
    std::string value;

    j.at("fileid").get_to(out.fileid);
    j.at("recno").get_to(out.recno);
    j.at("value").get_to(value);

    out.value = decode_utf8(value);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::col_remove &out)
{
    j.at("fileid").get_to(out.fileid);
    j.at("recno").get_to(out.recno);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::col_truncate &out)
{
    j.at("fileid").get_to(out.fileid);
    j.at("start").get_to(out.start);
    j.at("stop").get_to(out.stop);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::commit_header &out)
{
    j.at("txnid").get_to(out.txnid);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::prev_lsn &out)
{
    const json &prev_lsn = j.at("prev_lsn");
    if (!prev_lsn.is_array() || prev_lsn.size() != 2)
        throw model_exception("The \"prev_lsn\" entry is not an array of size 2");

    out.fileid = prev_lsn.at(0);
    out.offset = prev_lsn.at(1);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::row_put &out)
{
    std::string key, value;

    j.at("fileid").get_to(out.fileid);
    j.at("key").get_to(key);
    j.at("value").get_to(value);

    out.key = decode_utf8(key);
    out.value = decode_utf8(value);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::row_remove &out)
{
    std::string key;

    j.at("fileid").get_to(out.fileid);
    j.at("key").get_to(key);

    out.key = decode_utf8(key);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::row_truncate &out)
{
    std::string start, stop;

    j.at("fileid").get_to(out.fileid);
    j.at("mode").get_to(out.mode);
    j.at("start").get_to(start);
    j.at("stop").get_to(stop);

    out.start = decode_utf8(start);
    out.stop = decode_utf8(stop);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::txn_timestamp &out)
{
    j.at("commit_ts").get_to(out.commit_ts);
    j.at("durable_ts").get_to(out.durable_ts);
    j.at("prepare_ts").get_to(out.prepare_ts);
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, debug_log_parser::col_put &out)
{
    int ret;
    uint32_t fileid;
    uint64_t recno;
    WT_ITEM value;

    if ((ret = __wt_logop_col_put_unpack(session, pp, end, &fileid, &recno, &value)) != 0)
        return ret;

    out.fileid = fileid;
    out.recno = recno;
    out.value = std::string((const char *)value.data, value.size);
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  debug_log_parser::col_remove &out)
{
    int ret;
    uint32_t fileid;
    uint64_t recno;

    if ((ret = __wt_logop_col_remove_unpack(session, pp, end, &fileid, &recno)) != 0)
        return ret;

    out.fileid = fileid;
    out.recno = recno;
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  debug_log_parser::col_truncate &out)
{
    int ret;
    uint32_t fileid;
    uint64_t start, stop;

    if ((ret = __wt_logop_col_truncate_unpack(session, pp, end, &fileid, &start, &stop)) != 0)
        return ret;

    out.fileid = fileid;
    out.start = start;
    out.stop = stop;
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  debug_log_parser::commit_header &out)
{
    int ret;
    uint64_t txnid;
    WT_UNUSED(session);

    if ((ret = __wt_vunpack_uint(pp, WT_PTRDIFF(end, *pp), &txnid)) != 0)
        return ret;

    out.txnid = txnid;
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, debug_log_parser::prev_lsn &out)
{
    int ret;
    WT_LSN prev_lsn;

    if ((ret = __wt_logop_prev_lsn_unpack(session, pp, end, &prev_lsn)) != 0)
        return ret;

    out.fileid = prev_lsn.l.file;
    out.offset = prev_lsn.l.offset;
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(
  WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, debug_log_parser::row_put &out)
{
    int ret;
    uint32_t fileid;
    WT_ITEM key;
    WT_ITEM value;

    if ((ret = __wt_logop_row_put_unpack(session, pp, end, &fileid, &key, &value)) != 0)
        return ret;

    out.fileid = fileid;
    out.key = std::string((const char *)key.data, key.size);
    out.value = std::string((const char *)value.data, value.size);
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  debug_log_parser::row_remove &out)
{
    int ret;
    uint32_t fileid;
    WT_ITEM key;

    if ((ret = __wt_logop_row_remove_unpack(session, pp, end, &fileid, &key)) != 0)
        return ret;

    out.fileid = fileid;
    out.key = std::string((const char *)key.data, key.size);
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  debug_log_parser::row_truncate &out)
{
    int ret;
    uint32_t fileid;
    WT_ITEM start, stop;
    uint32_t mode;

    if ((ret = __wt_logop_row_truncate_unpack(session, pp, end, &fileid, &start, &stop, &mode)) !=
      0)
        return ret;

    out.fileid = fileid;
    out.start = std::string((const char *)start.data, start.size);
    out.stop = std::string((const char *)stop.data, stop.size);
    out.mode = mode;
    return 0;
}

/*
 * from_debug_log --
 *     Parse the given debug log entry.
 */
static int
from_debug_log(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
  debug_log_parser::txn_timestamp &out)
{
    int ret;
    uint64_t time_sec;
    uint64_t time_nsec;
    uint64_t commit_ts;
    uint64_t durable_ts;
    uint64_t first_commit_ts;
    uint64_t prepare_ts;
    uint64_t read_ts;

    if ((ret = __wt_logop_txn_timestamp_unpack(session, pp, end, &time_sec, &time_nsec, &commit_ts,
           &durable_ts, &first_commit_ts, &prepare_ts, &read_ts)) != 0)
        return ret;

    out.commit_ts = commit_ts;
    out.durable_ts = durable_ts;
    out.prepare_ts = prepare_ts;
    return 0;
}

/*
 * debug_log_parser::table_by_fileid --
 *     Find a table by the file ID.
 */
kv_table_ptr
debug_log_parser::table_by_fileid(uint64_t fileid)
{
    /* Remove the WT_LOGOP_IGNORE bit from the file ID. */
    uint64_t id = fileid & (WT_LOGOP_IGNORE - 1);
    auto table_itr = _fileid_to_table.find(id);
    if (table_itr == _fileid_to_table.end())
        throw model_exception("Unknown file ID: " + std::to_string(id));
    return table_itr->second;
}

/*
 * debug_log_parser::metadata_apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::metadata_apply(kv_transaction_ptr txn, const row_put &op)
{
    std::string key =
      std::get<std::string>(data_value::unpack(op.key.c_str(), op.key.length(), "S"));
    std::string value =
      std::get<std::string>(data_value::unpack(op.value.c_str(), op.value.length(), "S"));

    /* Parse the configuration string. */
    std::shared_ptr<config_map> m =
      std::make_shared<config_map>(config_map::from_string(value.c_str()));

    /* Remember the metadata. */
    _metadata[key] = m;

    /* Special handling for column groups. */
    if (starts_with(key, "colgroup:")) {
        std::string name = key.substr(std::strlen("colgroup:"));
        if (name.find(':') != std::string::npos)
            throw model_exception("The model does not currently support column groups");

        std::string source = m->get_string("source");
        _file_to_colgroup_name[source] = name;

        /* Establish mapping from the file ID to the table name. */
        auto i = _file_to_fileid.find(source);
        if (i == _file_to_fileid.end())
            throw model_exception("The database does not yet contain file " + source);
        _fileid_to_table_name[i->second] = name;

        /* Create the table, if it does not exist. */
        if (!_database.contains_table(name)) {
            std::shared_ptr<config_map> file_config = _metadata[source];
            std::shared_ptr<config_map> table_config = _metadata["table:" + name];

            std::string key_format = table_config->get_string("key_format");
            std::string value_format = table_config->get_string("value_format");

            kv_table_config config;
            if (file_config->contains("log"))
                config.log_enabled = file_config->get_map("log")->get_bool("enabled");
            config.type = kv_table::type_by_key_value_format(key_format, value_format);

            kv_table_ptr table = _database.create_table(name, config);
            table->set_key_value_format(key_format, value_format);
        }

        /* Establish mapping from the file ID to the table object. */
        _fileid_to_table[i->second] = _database.table(name);
    }

    /* Special handling for files. */
    else if (starts_with(key, "file:")) {
        uint64_t id = m->get_uint64("id");
        _fileid_to_file[id] = key;
        _file_to_fileid[key] = id;
    }

    /* Special handling for tables. */
    else if (starts_with(key, "table:")) {
        /* There is currently nothing to do. The table will get created with the colgroup. */
    }

    /* Special handling for the system prefix. */
    else if (starts_with(key, "system:")) {

        /* Set the base write generation. */
        if (key == "system:checkpoint_base_write_gen") {
            _base_write_gen = m->get_uint64("base_write_gen");
        }

        /* Handle checkpoints. */
        else if (starts_with(key, "system:checkpoint") || starts_with(key, "system:oldest")) {
            /*
             * WiredTiger uses the following naming conventions:
             *     - system:checkpoint, system:checkpoint_snapshot, system:oldest, etc., for
             *       nameless checkpoints
             *     - system:checkpoint.NAME, system:checkpoint_snapshot.NAME, etc., for named
             *       checkpoints
             *
             * We don't need to handle these kinds of metadata differently as the config strings
             * within them have different names, so we just build one unified configuration map from
             * all of them.
             */

            /* If this is a named checkpoint, the name follows the '.' character. */
            size_t p = key.find('.');
            std::string ckpt_name = p == std::string::npos ? WT_CHECKPOINT : key.substr(p + 1);

            /* Accumulate checkpoint metadata for future handling. */
            auto &ckpt_metadata_map = _txn_ckpt_metadata[txn->id()];
            auto i = ckpt_metadata_map.find(ckpt_name);
            if (i == ckpt_metadata_map.end())
                ckpt_metadata_map[ckpt_name] = std::move(m);
            else
                ckpt_metadata_map[ckpt_name] =
                  config_map::merge(ckpt_metadata_map[ckpt_name], std::move(m));
        }

        /* Unsupported system URI. */
        else
            throw model_exception("Unsupported metadata system URI: " + key);
    }

    /* Otherwise this is an unsupported URI type. */
    else
        throw model_exception("Unsupported metadata URI: " + key);
}

/*
 * debug_log_parser::metadata_checkpoint_apply --
 *     Handle the given checkpoint metadata operation.
 */
void
debug_log_parser::metadata_checkpoint_apply(
  const std::string &name, std::shared_ptr<config_map> config)
{
    /* Get the oldest and stable timestamps. */
    timestamp_t oldest_timestamp = config->contains("oldest_timestamp") ?
      config->get_uint64_hex("oldest_timestamp") :
      k_timestamp_none;
    timestamp_t stable_timestamp = config->contains("checkpoint_timestamp") ?
      config->get_uint64_hex("checkpoint_timestamp") :
      k_timestamp_none;

    /* Get the write generation number. */
    write_gen_t write_gen = config->get_uint64("write_gen");

    /* Get the transaction snapshot. */
    kv_transaction_snapshot_ptr snapshot;
    if (config->contains("snapshot_min")) {
        if (!config->contains("snapshot_max"))
            throw model_exception(
              "The checkpoint metadata contain snapshot_min but not snapshot_max");
        txn_id_t snapshot_min = config->get_uint64("snapshot_min");
        txn_id_t snapshot_max = config->get_uint64("snapshot_max");
        std::shared_ptr<std::vector<uint64_t>> snapshot_ids;
        if (config->contains("snapshots"))
            snapshot_ids = config->get_array_uint64("snapshots");
        else
            snapshot_ids = std::make_shared<std::vector<uint64_t>>();
        snapshot = std::make_shared<kv_transaction_snapshot_wt>(
          write_gen, snapshot_min, snapshot_max, *snapshot_ids);
    } else
        snapshot = std::make_shared<kv_transaction_snapshot_wt>(
          write_gen, k_txn_max, k_txn_max, std::vector<uint64_t>());

    /* Create the checkpoint. */
    _database.create_checkpoint(
      name.c_str(), std::move(snapshot), oldest_timestamp, stable_timestamp);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const col_put &op)
{
    /* Handle metadata operations. */
    if (op.fileid == 0)
        throw model_exception("Unsupported metadata operation: col_put");

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the key and the value. */
    data_value key = data_value(op.recno);
    data_value value = data_value::unpack(op.value, table->value_format());

    /* Perform the operation. */
    int ret = table->insert(std::move(txn), key, value);
    if (ret != 0)
        throw wiredtiger_exception(ret);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const col_remove &op)
{
    /* Handle metadata operations. */
    if (op.fileid == 0)
        throw model_exception("Unsupported metadata operation: col_remove");

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the key. */
    data_value key = data_value(op.recno);

    /* Perform the operation. */
    int ret = table->remove(std::move(txn), key);
    if (ret != 0)
        throw wiredtiger_exception(ret);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const col_truncate &op)
{
    /* Handle metadata operations. */
    if (op.fileid == 0)
        throw model_exception("Unsupported metadata operation: col_truncate");

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the keys. */
    data_value start = op.start == 0 ? NONE : data_value(op.start);
    data_value stop = op.stop == 0 ? NONE : data_value(op.stop);

    /* Perform the operation. */
    int ret = table->truncate(std::move(txn), start, stop);
    if (ret != 0)
        throw wiredtiger_exception(ret);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const row_put &op)
{
    /* Handle metadata operations. */
    if (op.fileid == 0) {
        metadata_apply(txn, op);
        return;
    }

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the key and the value. */
    data_value key = data_value::unpack(op.key, table->key_format());
    data_value value = data_value::unpack(op.value, table->value_format());

    /* Perform the operation. */
    int ret = table->insert(std::move(txn), key, value);
    if (ret != 0)
        throw wiredtiger_exception(ret);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const row_remove &op)
{
    /* Handle metadata operations. */
    if (op.fileid == 0)
        throw model_exception("Unsupported metadata operation: row_remove");

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the key. */
    data_value key = data_value::unpack(op.key, table->key_format());

    /* Perform the operation. */
    int ret = table->remove(std::move(txn), key);
    if (ret != 0)
        throw wiredtiger_exception(ret);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const row_truncate &op)
{
    /* Handle metadata operations. */
    if (op.fileid == 0)
        throw model_exception("Unsupported metadata operation: row_truncate");

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the keys. */
    data_value start;
    data_value stop;

    if (op.mode == WT_TXN_TRUNC_BOTH || op.mode == WT_TXN_TRUNC_START)
        start = data_value::unpack(op.start, table->key_format());
    if (op.mode == WT_TXN_TRUNC_BOTH || op.mode == WT_TXN_TRUNC_STOP)
        stop = data_value::unpack(op.stop, table->key_format());

    /* Perform the operation. */
    int ret = table->truncate(std::move(txn), start, stop);
    if (ret != 0)
        throw wiredtiger_exception(ret);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const txn_timestamp &op)
{
    /* Handle the prepare operation. */
    if (op.commit_ts == k_timestamp_none && op.prepare_ts != k_timestamp_none) {
        txn->prepare(op.prepare_ts);
        return;
    }

    /* Handle the commit of a prepared transaction. */
    if (op.commit_ts != k_timestamp_none && op.prepare_ts != k_timestamp_none) {
        if (txn->state() != kv_transaction_state::prepared)
            throw model_exception("The transaction must be in a prepared state before commit");
        txn->commit(op.commit_ts, op.durable_ts);
        return;
    }

    /* Otherwise it is just an operation to set the commit timestamp. */
    txn->set_commit_timestamp(op.commit_ts);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(const prev_lsn &op)
{
    /* We find this record when the database starts up, either normally or after a crash. */
    if (op.fileid == 1 && op.offset == 0)
        _database.start();
}

/*
 * debug_log_parser::begin_transaction --
 *     Begin a transaction.
 */
kv_transaction_ptr
debug_log_parser::begin_transaction(const debug_log_parser::commit_header &op)
{
    if (_base_write_gen == k_write_gen_none)
        throw model_exception("The base write generation is not set");

    kv_transaction_ptr txn = _database.begin_transaction();
    txn->set_wt_metadata(op.txnid, _base_write_gen);
    return txn;
}

/*
 * debug_log_parser::commit_transaction --
 *     Commit/finalize a transaction.
 */
void
debug_log_parser::commit_transaction(kv_transaction_ptr txn)
{
    kv_transaction_state txn_state = txn->state();
    if (txn_state != kv_transaction_state::in_progress &&
      txn_state != kv_transaction_state::prepared && txn_state != kv_transaction_state::committed)
        throw model_exception("The transaction is in an unexpected state");

    /*
     * Commit the transaction if it has not yet been committed.
     *
     * However, empty prepared transactions require special handling: Because the debug log might
     * not have recorded their commit and durable timestamps, we cannot commit them, so we abort
     * them instead. This does not make any practical difference, because these transactions do not
     * contain any writes and would not conflict with any other transactions.
     */
    if (txn_state != kv_transaction_state::committed) {
        if (txn_state == kv_transaction_state::prepared && txn->empty())
            txn->rollback();
        else
            txn->commit();
    }

    /* Process the checkpoint metadata, if there are any associated with the transaction. */
    auto i = _txn_ckpt_metadata.find(txn->id());
    if (i != _txn_ckpt_metadata.end()) {
        for (auto &p : i->second)
            metadata_checkpoint_apply(p.first, p.second);
        _txn_ckpt_metadata.erase(i);
    }
}

/*
 * from_debug_log_helper_args --
 *     Arguments for the helper function.
 */
struct from_debug_log_helper_args {
    debug_log_parser &parser;
    kv_database &database;

    /*
     * from_debug_log_helper_args::from_debug_log_helper_args --
     *     Create an instance of this struct.
     */
    inline from_debug_log_helper_args(debug_log_parser &parser_arg, kv_database &database_arg)
        : parser(parser_arg), database(database_arg)
    {
    }
};

/*
 * from_debug_log_helper --
 *     Parse the debug log into the model - a helper function.
 */
static int
from_debug_log_helper(WT_SESSION_IMPL *session, WT_ITEM *rawrec, WT_LSN *lsnp, WT_LSN *next_lsnp,
  void *cookie, int firstrecord)
{
    const uint8_t *rec_end, *p;
    int ret;

    from_debug_log_helper_args &args = *((from_debug_log_helper_args *)cookie);

    /* Get the basic record info, including the record type. */
    p = WT_LOG_SKIP_HEADER(rawrec->data);
    rec_end = (const uint8_t *)rawrec->data + rawrec->size;
    uint32_t rec_type;
    if ((ret = __wt_logrec_read(session, &p, rec_end, &rec_type)) != 0)
        return ret;

    /* Process supported record types. */
    switch (rec_type) {
    case WT_LOGREC_COMMIT: {
        const uint8_t **pp = &p;

        /* The commit entry, which contains the list of operations in the transaction. */
        debug_log_parser::commit_header commit;
        if ((ret = from_debug_log(session, pp, rec_end, commit)) != 0)
            return ret;

        /* Start the transaction. */
        kv_transaction_ptr txn = args.parser.begin_transaction(commit);

        /* Iterate over the list of operations. */
        while (*pp < rec_end && **pp) {
            uint32_t op_type, op_size;

            /* Get the operation record's type and size. */
            if ((ret = __wt_logop_read(session, pp, rec_end, &op_type, &op_size)) != 0)
                return ret;
            const uint8_t *op_end = *pp + op_size;

            /* Parse and apply the operation. */
            switch (op_type) {
            case WT_LOGOP_COL_PUT: {
                debug_log_parser::col_put v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(txn, v);
                break;
            }
            case WT_LOGOP_COL_REMOVE: {
                debug_log_parser::col_remove v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(txn, v);
                break;
            }
            case WT_LOGOP_COL_TRUNCATE: {
                debug_log_parser::col_truncate v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(txn, v);
                break;
            }
            case WT_LOGOP_ROW_PUT: {
                debug_log_parser::row_put v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(txn, v);
                break;
            }
            case WT_LOGOP_ROW_REMOVE: {
                debug_log_parser::row_remove v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(txn, v);
                break;
            }
            case WT_LOGOP_TXN_TIMESTAMP: {
                debug_log_parser::txn_timestamp v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(txn, v);
                break;
            }
            case WT_LOGOP_ROW_TRUNCATE: {
                debug_log_parser::row_truncate v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(txn, v);
                break;
            }
            default:
                *pp += op_size;
                throw model_exception("Unsupported operation type #" + std::to_string(op_type));
            }
        }

        args.parser.commit_transaction(std::move(txn));
        break;
    }

    case WT_LOGREC_SYSTEM: {
        const uint8_t **pp = &p;

        /* The system entry, which contains the list of system-level operations. */
        while (*pp < rec_end && **pp) {
            uint32_t op_type, op_size;

            /* Get the operation record's type and size. */
            if ((ret = __wt_logop_read(session, pp, rec_end, &op_type, &op_size)) != 0)
                return ret;
            const uint8_t *op_end = *pp + op_size;

            /* Parse and apply the operation. */
            switch (op_type) {
            case WT_LOGOP_PREV_LSN: {
                debug_log_parser::prev_lsn v;
                if ((ret = from_debug_log(session, pp, op_end, v)) != 0)
                    return ret;
                args.parser.apply(v);
                break;
            }
            default:
                *pp += op_size;
                /* Silently ignore unsupported operations. */
            }
        }

        break;
    }

    case WT_LOGREC_CHECKPOINT:
    case WT_LOGREC_FILE_SYNC:
    case WT_LOGREC_MESSAGE:
        /* Ignored record types. */
        break;

    default:
        throw model_exception("Unsupported record type #" + std::to_string(rec_type));
    }

    return 0;
}

/*
 * debug_log_parser::from_debug_log --
 *     Parse the debug log into the model. This function must be called after opening the database
 *     but before performing any writes, because otherwise the debug log may not contain records of
 *     the most recent operations.
 */
void
debug_log_parser::from_debug_log(kv_database &database, WT_CONNECTION *conn)
{
    int ret;
    WT_SESSION *session;

    ret = conn->open_session(conn, nullptr, nullptr, &session);
    if (ret != 0)
        throw wiredtiger_exception("Cannot open a session: ", ret);
    wiredtiger_session_guard session_guard(session);

    debug_log_parser parser(database);
    from_debug_log_helper_args args{parser, database};

    ret = __wt_log_scan(
      (WT_SESSION_IMPL *)session, nullptr, nullptr, WT_LOGSCAN_FIRST, from_debug_log_helper, &args);
    if (ret != 0)
        throw wiredtiger_exception("Cannot scan the log: ", ret);

    /*
     * Simulate the database starting up. As this function is called right after the database
     * started prior to verification, WiredTiger would have had run rollback to stable by now, even
     * though we would not see it in the debug log. So simulate the database startup, as it has
     * already happened.
     */
    database.start();
}

/*
 * debug_log_parser::from_json --
 *     Parse the debug log JSON file into the model. The input debug log must be printed to JSON
 *     after opening the database but before performing any writes, because it may otherwise miss
 *     most recent operations.
 */
void
debug_log_parser::from_json(kv_database &database, const char *path)
{
    debug_log_parser parser(database);

    /* Load the JSON from the provided file. */
    std::ifstream f(path);
    json data = json::parse(f);

    /* The debug log JSON file is structured as an array of log entries. */
    if (!data.is_array())
        throw model_exception("The top-level element in the JSON file is not an array");

    /* Now parse each individual entry. */
    for (auto &log_entry : data) {
        if (!log_entry.is_object())
            throw model_exception("The second-level element in the JSON file is not an object");

        std::string log_entry_type = log_entry.at("type").get<std::string>();

        /* The commit entry contains full description of a transaction, including all
         * operations. */
        if (log_entry_type == "commit") {
            /* Begin the transaction. */
            kv_transaction_ptr txn = parser.begin_transaction(log_entry.get<commit_header>());

            /* Replay all operations. */
            for (auto &op_entry : log_entry.at("ops")) {
                std::string op_type = op_entry.at("optype").get<std::string>();

                /* Column-store operations. */
                if (op_type == "col_modify")
                    throw model_exception("Unsupported operation: " + op_type);
                if (op_type == "col_put") {
                    parser.apply(txn, op_entry.get<col_put>());
                    continue;
                }
                if (op_type == "col_remove") {
                    parser.apply(txn, op_entry.get<col_remove>());
                    continue;
                }
                if (op_type == "col_truncate") {
                    parser.apply(txn, op_entry.get<col_truncate>());
                    continue;
                }

                /* Row-store operations. */
                if (op_type == "row_modify")
                    throw model_exception("Unsupported operation: " + op_type);
                if (op_type == "row_put") {
                    parser.apply(txn, op_entry.get<row_put>());
                    continue;
                }
                if (op_type == "row_remove") {
                    parser.apply(txn, op_entry.get<row_remove>());
                    continue;
                }
                if (op_type == "row_truncate") {
                    parser.apply(txn, op_entry.get<row_truncate>());
                    continue;
                }

                /* Transaction operations. */
                if (op_type == "txn_timestamp") {
                    parser.apply(txn, op_entry.get<txn_timestamp>());
                    continue;
                }

                /* Operations that we can skip... for now. */
                if (op_type == "prev_lsn" || op_type == "checkpoint_start")
                    continue;

                throw model_exception("Unsupported operation \"" + op_type + "\"");
            }

            /* Commit/finalize the transaction. */
            parser.commit_transaction(std::move(txn));
            continue;
        }

        /* Handle the relevant system entries. */
        if (log_entry_type == "system") {

            /* Replay all supported system operations. */
            for (auto &op_entry : log_entry.at("ops")) {
                std::string op_type = op_entry.at("optype").get<std::string>();

                /* Previous LSN. */
                if (op_type == "prev_lsn") {
                    parser.apply(op_entry.get<prev_lsn>());
                    continue;
                }

                /* Backup IDs. */
                if (op_type == "backup_id")
                    continue; /* Nothing to do. */

                /* Silently ignore the other operation types. */
            }

            continue;
        }

        /* Ignore these fields. */
        if (log_entry_type == "checkpoint" || log_entry_type == "file_sync" ||
          log_entry_type == "message")
            continue;

        throw model_exception("Unsupported log entry type \"" + log_entry_type + "\"");
    }

    /*
     * Simulate the database starting up.
     *
     * There are two cases, both of which require us to do this:
     *     - If the database is not running while we are loading the model, it will start before the
     *       verification and run rollback to stable. So do that here in anticipation.
     *     - If the database has just started prior to loading the model, it would have had run
     *       rollback to stable by now, but we would not have seen the corresponding log record, so
     *       simulate the database startup now as it has already happened.
     */
    database.start();
}

} /* namespace model */
