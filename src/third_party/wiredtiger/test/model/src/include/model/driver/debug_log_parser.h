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

#include <memory>
#include <unordered_map>
#include "model/kv_database.h"
#include "model/util.h"
#include "wiredtiger.h"

namespace model {

/*
 * debug_log_parser --
 *     A parser that feeds the model from the database's debug log.
 */
class debug_log_parser {

public:
    /*
     * debug_log_parser::commit_header --
     *     The header for the commit log entry.
     */
    struct commit_header {
        txn_id_t txnid;
    };

    /*
     * debug_log_parser::prev_lsn --
     *     The prev_lsn log entry.
     */
    struct prev_lsn {
        uint64_t fileid;
        uint64_t offset;
    };

    /*
     * debug_log_parser::row_put --
     *     The row_put log entry.
     */
    struct row_put {
        uint64_t fileid;
        std::string key;
        std::string value;

        /*
         * debug_log_parser::row_put::row_put --
         *     Default constructor.
         */
        inline row_put() : fileid(0) {}
    };

    /*
     * debug_log_parser::row_remove --
     *     The row_remove log entry.
     */
    struct row_remove {
        uint64_t fileid;
        std::string key;

        /*
         * debug_log_parser::row_remove::row_remove --
         *     Default constructor.
         */
        inline row_remove() : fileid(0) {}
    };

    /*
     * debug_log_parser::row_truncate --
     *     The row_truncate log entry.
     */
    struct row_truncate {
        uint64_t fileid;
        uint32_t mode;
        std::string start;
        std::string stop;

        /*
         * debug_log_parser::row_truncate::row_truncate --
         *     Default constructor.
         */
        inline row_truncate() : fileid(0), mode(0) {}
    };

    /*
     * debug_log_parser::txn_timestamp --
     *     The txn_timestamp log entry.
     */
    struct txn_timestamp {
        timestamp_t commit_ts;
        timestamp_t durable_ts;
        timestamp_t prepare_ts;
    };

public:
    /*
     * debug_log_parser::debug_log_parser --
     *     Create a new instance of the parser. Make sure that the database instance outlives the
     *     lifetime of this parser object.
     */
    inline debug_log_parser(kv_database &database)
        : _database(database), _base_write_gen(k_write_gen_first)
    {
    }

    /*
     * debug_log_parser::from_debug_log --
     *     Parse the debug log into the model. This function must be called after opening the
     *     database but before performing any writes, because otherwise the debug log may not
     *     contain records of the most recent operations.
     */
    static void from_debug_log(kv_database &database, WT_CONNECTION *conn);

    /*
     * debug_log_parser::from_json --
     *     Parse the debug log JSON file into the model. The input debug log must be printed to JSON
     *     after opening the database but before performing any writes, because it may otherwise
     *     miss most recent operations.
     */
    static void from_json(kv_database &database, const char *path);

    /*
     * debug_log_parser::apply --
     *     Apply the given operation to the model.
     */
    void apply(kv_transaction_ptr txn, const row_put &op);

    /*
     * debug_log_parser::apply --
     *     Apply the given operation to the model.
     */
    void apply(kv_transaction_ptr txn, const row_remove &op);

    /*
     * debug_log_parser::apply --
     *     Apply the given operation to the model.
     */
    void apply(kv_transaction_ptr txn, const txn_timestamp &op);

    /*
     * debug_log_parser::apply --
     *     Apply the given operation to the model.
     */
    void apply(kv_transaction_ptr txn, const row_truncate &op);

    /*
     * debug_log_parser::apply --
     *     Apply the given operation to the model.
     */
    void apply(const prev_lsn &op);

    /*
     * debug_log_parser::begin_transaction --
     *     Begin a transaction.
     */
    kv_transaction_ptr begin_transaction(const commit_header &op);

    /*
     * debug_log_parser::commit_transaction --
     *     Commit/finalize a transaction.
     */
    void commit_transaction(kv_transaction_ptr txn);

protected:
    /*
     * debug_log_parser::metadata_apply --
     *     Handle the given metadata operation.
     */
    void metadata_apply(kv_transaction_ptr txn, const row_put &op);

    /*
     * debug_log_parser::metadata_checkpoint_apply --
     *     Handle the given checkpoint metadata operation.
     */
    void metadata_checkpoint_apply(const std::string &name, std::shared_ptr<config_map> config);

    /*
     * debug_log_parser::table_by_fileid --
     *     Find a table by the file ID.
     */
    kv_table_ptr table_by_fileid(uint64_t fileid);

private:
    kv_database &_database;

    /* Metadata and the relevant indexes. */
    std::unordered_map<std::string, std::shared_ptr<config_map>> _metadata;
    std::unordered_map<std::string, std::string> _file_to_colgroup_name;
    std::unordered_map<std::string, uint64_t> _file_to_fileid;
    std::unordered_map<uint64_t, std::string> _fileid_to_file;
    std::unordered_map<uint64_t, std::string> _fileid_to_table_name;
    std::unordered_map<uint64_t, kv_table_ptr> _fileid_to_table;

    /* The current base write generation. */
    write_gen_t _base_write_gen;

    /* Place for accumulating checkpoint metadata: TXN ID -> checkpoint name -> config map. */
    std::unordered_map<txn_id_t, std::unordered_map<std::string, std::shared_ptr<config_map>>>
      _txn_ckpt_metadata;
};

} /* namespace model */
