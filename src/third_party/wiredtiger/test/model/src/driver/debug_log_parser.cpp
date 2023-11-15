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
from_json(const json &j, debug_log_parser::row_put &out)
{
    j.at("fileid").get_to(out.fileid);
    j.at("key").get_to(out.key);
    j.at("value").get_to(out.value);
}

/*
 * from_json --
 *     Parse the given log entry.
 */
static void
from_json(const json &j, debug_log_parser::row_remove &out)
{
    j.at("fileid").get_to(out.fileid);
    j.at("key").get_to(out.key);
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
debug_log_parser::metadata_apply(const row_put &op)
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

        /* Establish mapping from the file ID to the table name and table object, if possible. */
        auto i = _file_to_fileid.find(source);
        if (i != _file_to_fileid.end()) {
            _fileid_to_table_name[i->second] = name;
            if (!_database.contains_table(name))
                throw model_exception("The database does not yet contain table " + name);
            _fileid_to_table[i->second] = _database.table(name);
        }
    }

    /* Special handling for files. */
    else if (starts_with(key, "file:")) {
        uint64_t id = m->get_uint64("id");
        _fileid_to_file[id] = key;
        _file_to_fileid[key] = id;

        /* Establish mapping from the file ID to the table name and table object, if possible. */
        auto i = _file_to_colgroup_name.find(key);
        if (i != _file_to_colgroup_name.end()) {
            _fileid_to_table_name[id] = i->second;
            if (!_database.contains_table(i->second))
                throw model_exception("The database does not yet contain table " + i->second);
            _fileid_to_table[id] = _database.table(i->second);
        }
    }

    /* Special handling for LSM. */
    else if (starts_with(key, "lsm:"))
        throw model_exception("The model does not currently support LSM");

    /* Special handling for tables. */
    else if (starts_with(key, "table:")) {
        std::string name = key.substr(std::strlen("table:"));
        if (!_database.contains_table(name)) {
            kv_table_ptr table = _database.create_table(name);
            table->set_key_value_format(m->get_string("key_format"), m->get_string("value_format"));
        }
    }

    /* Special handling for the system prefix. */
    else if (starts_with(key, "system:")) {
        /* We don't currently need to handle this. */
    }

    /* Otherwise this is an unsupported URI type. */
    else
        throw model_exception("Unsupported metadata URI: " + key);
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
        metadata_apply(op);
        return;
    }

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the key and the value. */
    data_value key = data_value::unpack(op.key, table->key_format());
    data_value value = data_value::unpack(op.value, table->value_format());

    /* Perform the operation. */
    table->insert(txn, key, value);
}

/*
 * debug_log_parser::apply --
 *     Apply the given operation to the model.
 */
void
debug_log_parser::apply(kv_transaction_ptr txn, const row_remove &op)
{
    /* Handle metadata operations. */
    if (op.fileid == 0) {
        throw model_exception("Unsupported metadata operation: row_remove");
        return;
    }

    /* Find the table. */
    kv_table_ptr table = table_by_fileid(op.fileid);

    /* Parse the key. */
    data_value key = data_value::unpack(op.key, table->key_format());

    /* Perform the operation. */
    table->remove(txn, key);
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
 * from_debug_log_helper_args --
 *     Arguments for the helper function.
 */
struct from_debug_log_helper_args {
    debug_log_parser &parser;
    kv_database &database;
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
        return 0;

    /* Process supported record types. */
    switch (rec_type) {
    case WT_LOGREC_COMMIT: {
        /* The commit entry, which contains the list of operations in the transaction. */
        uint64_t txnid;
        if ((ret = __wt_vunpack_uint(&p, WT_PTRDIFF(rec_end, p), &txnid)) != 0)
            return ret;

        /* Start the transaction. */
        kv_transaction_ptr txn = args.database.begin_transaction();

        /* Iterate over the list of operations. */
        const uint8_t **pp = &p;
        while (*pp < rec_end && **pp) {
            uint32_t op_type, op_size;

            /* Get the operation record's type and size. */
            if ((ret = __wt_logop_read(session, pp, rec_end, &op_type, &op_size)) != 0)
                return ret;
            const uint8_t *op_end = *pp + op_size;

            /* Parse and apply the operation. */
            switch (op_type) {
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
            default:
                *pp += op_size;
                throw model_exception("Unsupported operation type #" + std::to_string(op_type));
            }
        }

        if (txn->state() != kv_transaction_state::committed)
            txn->commit();
        break;
    }

    case WT_LOGREC_CHECKPOINT:
    case WT_LOGREC_FILE_SYNC:
    case WT_LOGREC_MESSAGE:
    case WT_LOGREC_SYSTEM:
        /* Ignored record types. */
        break;

    default:
        throw model_exception("Unsupported record type #" + std::to_string(rec_type));
    }

    return 0;
}

/*
 * debug_log_parser::from_debug_log --
 *     Parse the debug log into the model.
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
}

/*
 * debug_log_parser::from_json --
 *     Parse the debug log JSON file into the model.
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
            kv_transaction_ptr txn = database.begin_transaction();

            /* Replay all operations. */
            for (auto &op_entry : log_entry.at("ops")) {
                std::string op_type = op_entry.at("optype").get<std::string>();

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
                if (op_type == "row_truncate")
                    throw model_exception("Unsupported operation: " + op_type);

                /* Transaction operations. */
                if (op_type == "txn_timestamp") {
                    parser.apply(txn, op_entry.get<txn_timestamp>());
                    continue;
                }

                /* Operations that we can skip... for now. */
                if (op_type == "prev_lsn" || op_type == "checkpoint_start")
                    continue;

                /* Column-store operations (unsupported). */
                if (op_type.substr(0, 4) == "col_")
                    throw model_exception("The parser does not currently support column stores.");

                throw model_exception("Unsupported operation \"" + op_type + "\"");
            }

            if (txn->state() != kv_transaction_state::committed)
                txn->commit();
            continue;
        }

        /* Ignore these fields. */
        if (log_entry_type == "checkpoint" || log_entry_type == "file_sync" ||
          log_entry_type == "message" || log_entry_type == "system")
            continue;

        throw model_exception("Unsupported log entry type \"" + log_entry_type + "\"");
    }
}

} /* namespace model */
