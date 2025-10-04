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

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include "model/driver/kv_workload.h"
#include "model/driver/kv_workload_sequence.h"
#include "model/random.h"

namespace model {

/*
 * kv_workload_generator_spec --
 *     A high-level workload specification.
 */
struct kv_workload_generator_spec {

    /* Top-level database configuration. */
    float disaggregated;

    /* The minimum and maximum number of tables. */
    size_t min_tables;
    size_t max_tables;

    /* The minimum and maximum number of operation sequences, e.g., transactions. */
    size_t min_sequences;
    size_t max_sequences;

    /* The maximum number of concurrent transactions. */
    size_t max_concurrent_transactions;

    /* The maximum key/value to use in the generation. */
    uint64_t max_recno;
    uint64_t max_value_uint64;

    /* Probabilities for table types. */
    float column_fix;
    float column_var;

    /* The probability of allowing the use of "set commit timestamp" in a transaction. */
    float use_set_commit_timestamp;

    /* The probability of running with connection level logging */
    float conn_logging;

    /* Probabilities of operations within a transaction. */
    float finish_transaction; /* Commit, prepare, or rollback. */
    float get;
    float insert;
    float remove;
    float set_commit_timestamp; /* If allowed. */
    float truncate;

    /* Probabilities of special operations. */
    float checkpoint;
    float checkpoint_crash;
    float crash;
    float evict;
    float restart;
    float rollback_to_stable;
    float set_oldest_timestamp;
    float set_stable_timestamp;

    /* The probabilities for choosing an existing key, if available. */
    float get_existing;
    float remove_existing;
    float update_existing;

    /* The probability of starting a prepared transaction. */
    float prepared_transaction;

    /* The maximum delay after preparing a transaction (expressed as operation count). */
    size_t max_delay_after_prepare;

    /* Probabilities of transaction rollback. */
    float nonprepared_transaction_rollback;
    float prepared_transaction_rollback_after_prepare;
    float prepared_transaction_rollback_before_prepare;

    /* Probabilities of WiredTiger timing stress configurations. */
    /* FIXME-WT-13878 : Refactor this code and move into a separate structure. */
    float timing_stress_ckpt_slow;
    float timing_stress_ckpt_evict_page;
    float timing_stress_ckpt_handle;
    float timing_stress_ckpt_stop;
    float timing_stress_compact_slow;
    float timing_stress_hs_ckpt_delay;
    float timing_stress_hs_search;
    float timing_stress_hs_sweep_race;
    float timing_stress_prepare_ckpt_delay;
    float timing_stress_commit_txn_slow;
    float timing_stress_rec_before_wrapup;

    /*
     * kv_workload_generator_spec::kv_workload_generator_spec --
     *     Create the generator specification using default probability values.
     */
    kv_workload_generator_spec();
};

/*
 * kv_workload_generator --
 *     A workload generator for a key-value database.
 */
class kv_workload_generator {

protected:
    /*
     * op_category --
     *     The operation category.
     */
    enum class op_category {
        none,
        evict,
        get,
        remove,
        update,
    };

    /*
     * key_state --
     *     The state of a key.
     */
    struct key_state {

        /* The number of operations on the key so far. */
        size_t num_ops;

        /*
         * key_state::key_state --
         *     Create a new key state.
         */
        inline key_state(size_t ops = 0) : num_ops(ops) {}
    };

    /*
     * table_context --
     *     The context for a table.
     */
    class table_context {

    public:
        /*
         * table_context::table_context --
         *     Create a new table context.
         */
        inline table_context(table_id_t id, const std::string &name, const std::string &key_format,
          const std::string &value_format, kv_table_type type)
            : _id(id), _name(name), _key_format(key_format), _value_format(value_format),
              _type(type), _sum_key_ops(0)
        {
        }

        /*
         * table_context::id --
         *     Get the table ID.
         */
        inline table_id_t
        id() const noexcept
        {
            return _id;
        }

        /*
         * table_context::name --
         *     Get the table name. Return a reference, which is safe, because its lifetime is tied
         *     to this object.
         */
        inline const std::string &
        name() const noexcept
        {
            return _name;
        }

        /*
         * table_context::key_format --
         *     Get the key format. Return a reference, which is safe, because its lifetime is tied
         *     to this object.
         */
        inline const std::string &
        key_format() const noexcept
        {
            return _key_format;
        }

        /*
         * table_context::value_format --
         *     Get the value format. Return a reference, which is safe, because its lifetime is tied
         *     to this object.
         */
        inline const std::string &
        value_format() const noexcept
        {
            return _value_format;
        }

        /*
         * table_context::empty --
         *     Check if the table is empty
         */
        inline bool
        empty() const noexcept
        {
            return _keys.empty();
        }

        /*
         * table_context::type --
         *     Get the table type.
         */
        inline kv_table_type
        type() const noexcept
        {
            return _type;
        }

        /*
         * table_context::remove_key --
         *     Mark the given key as removed.
         */
        inline void
        remove_key(const data_value &key)
        {
            auto iter = _keys.find(key);
            if (iter != _keys.end()) {
                _sum_key_ops -= iter->second.num_ops;
                _keys.erase(iter);
            }
        }

        /*
         * table_context::remove_key_range --
         *     Mark the given key range as removed.
         */
        inline void
        remove_key_range(const data_value &start, const data_value &stop)
        {
            auto start_iter = _keys.lower_bound(start);
            auto stop_iter = _keys.upper_bound(stop);
            for (auto i = start_iter; i != stop_iter && i != _keys.end(); i++)
                _sum_key_ops -= i->second.num_ops;
            _keys.erase(start_iter, stop_iter);
        }

        /*
         * table_context::update_key --
         *     Mark the given key as updated.
         */
        inline void
        update_key(const data_value &key)
        {
            auto iter = _keys.find(key);
            if (iter == _keys.end())
                _keys[key] = key_state{1};
            else
                iter->second.num_ops++;
            _sum_key_ops++;
        }

        /*
         * table_context::choose_existing_key --
         *     Randomly select an existing key.
         */
        data_value choose_existing_key(random &r);

    private:
        table_id_t _id;
        std::string _name;
        std::string _key_format, _value_format;
        kv_table_type _type;

        std::map<data_value, key_state> _keys;
        size_t _sum_key_ops;
    };

    /*
     * table_context_ptr --
     *     Pointer to a table context.
     */
    using table_context_ptr = std::shared_ptr<table_context>;

    /*
     * sequence_state --
     *     The traversal state for a sequence.
     */
    struct sequence_state {

        /* The actual sequence. An unsafe pointer is okay, as we assume no concurrent uses. */
        kv_workload_sequence *sequence;

        /* The index of the next operation, if we are also going through their operations. */
        size_t next_operation_index;

        /* The number of operation sequences that must finish before we can visit this one. */
        /* Make this atomic in anticipation of a future parallel executor. */
        std::atomic<size_t> num_unsatisfied_dependencies;

        /*
         * sequence_state::sequence_state --
         *     Initialize.
         */
        inline sequence_state(kv_workload_sequence *sequence)
            : sequence(sequence), next_operation_index(0),
              num_unsatisfied_dependencies(sequence->dependencies().size())
        {
        }
    };

    /*
     * sequence_traversal --
     *     The traversal state, when traversing the collection of sequences in order that satisfies
     *     their dependencies. The traversal can be optionally also constrained by block the
     *     sequence numbers (we need this when assigning timestamps).
     */
    struct sequence_traversal {

    public:
        /*
         * sequence_traversal::sequence_traversal --
         *     Initialize the traversal.
         */
        sequence_traversal(
          std::deque<kv_workload_sequence_ptr> &sequences,
          std::function<bool(kv_workload_sequence &)> barrier_fn = [](kv_workload_sequence &) {
              return false;
          });

        /*
         * sequence_traversal::sequence_traversal --
         *     Delete the copy constructor.
         */
        sequence_traversal(const sequence_traversal &) = delete;

        /*
         * sequence_traversal::~sequence_traversal --
         *     Clean up after the traversal.
         */
        ~sequence_traversal()
        {
            for (auto p : _per_sequence_state)
                delete p.second;
        }

        /*
         * sequence_traversal::operator= --
         *     Delete the assignment operator.
         */
        sequence_traversal &operator=(const sequence_traversal &) = delete;

        /*
         * sequence_traversal::has_more --
         *     Check if there are any more sequences left.
         */
        inline bool
        has_more() const noexcept
        {
            return !_runnable.empty();
        }

        /*
         * sequence_traversal::runnable --
         *     Get the list of "runnable" operation sequences (which have satisfied dependencies).
         */
        inline const std::deque<sequence_state *> &
        runnable() const
        {
            return _runnable;
        }

        /*
         * sequence_traversal::complete_all --
         *     Complete all "runnable" sequences and advance the iterator.
         */
        void complete_all();

        /*
         * sequence_traversal::complete_one --
         *     Complete one "runnable" sequence and advance the iterator.
         */
        void complete_one(sequence_state *s);

    protected:
        /*
         * sequence_traversal::advance_barrier --
         *     Advance the barrier.
         */
        void advance_barrier();

        /*
         * sequence_traversal::find_next_barrier --
         *     Find the next barrier.
         */
        size_t find_next_barrier(size_t start);

    private:
        std::function<bool(kv_workload_sequence &)> _barrier_fn;
        std::deque<kv_workload_sequence_ptr> _sequences;

        size_t _barrier_seq_no;
        std::unordered_map<kv_workload_sequence *, sequence_state *> _per_sequence_state;
        std::deque<sequence_state *> _runnable;
        std::deque<sequence_state *> _runnable_after_barrier;
    };

public:
    /*
     * kv_workload_generator::kv_workload_generator --
     *     Create a new workload generator.
     */
    kv_workload_generator() = delete;

    /*
     * kv_workload_generator::generate --
     *     Generate the workload.
     */
    static inline std::shared_ptr<kv_workload>
    generate(const kv_workload_generator_spec &spec = _default_spec, uint64_t seed = 0)
    {
        kv_workload_generator generator(spec, seed);
        generator.run();
        return generator.workload();
    }

    static std::string
    generate_stress_configurations(uint64_t seed = 0)
    {
        kv_workload_generator generator(_default_spec, seed);
        return generator.generate_connection_stress_config();
    }

    static std::string
    generate_log_configurations(uint64_t seed = 0)
    {
        kv_workload_generator generator(_default_spec, seed);
        return generator.generate_connection_log_config();
    }

protected:
    /*
     * kv_workload_generator::kv_workload_generator --
     *     Create a new workload generator.
     */
    kv_workload_generator(const kv_workload_generator_spec &spec, uint64_t seed = 0);

    /*
     * kv_workload_generator::run --
     *     Generate the workload.
     */
    void run();

    /*
     * kv_workload_generator::workload --
     *     Get the generated workload.
     */
    inline std::shared_ptr<kv_workload>
    workload() const
    {
        return _workload_ptr;
    }

    /*
     * kv_workload_generator::assign_timestamps --
     *     Assign timestamps to operations in a sequence.
     */
    void assign_timestamps(kv_workload_sequence &sequence, timestamp_t first, timestamp_t last,
      timestamp_t &oldest, timestamp_t &stable);

    /*
     * kv_workload_generator::choose_table --
     *     Choose a table for an operation, creating one if necessary.
     */
    table_context_ptr choose_table(kv_workload_sequence_ptr txn);

    /*
     * kv_workload_generator::create_table --
     *     Create a table.
     */
    void create_table();

    /*
     * kv_workload_generator::generate_connection_stress_config --
     *     Generate random time stress configurations.
     */
    std::string generate_connection_stress_config();

    /*
     * kv_workload_generator::generate_connection_log_config --
     *     Generate random WiredTiger log configurations.
     */
    std::string generate_connection_log_config();

    /*
     * kv_workload_generator::generate_key --
     *     Generate a key.
     */
    data_value generate_key(table_context_ptr table, op_category op = op_category::none);

    /*
     * kv_workload_generator::generate_transaction --
     *     Generate a random transaction.
     */
    kv_workload_sequence_ptr generate_transaction(size_t seq_no);

    /*
     * kv_workload_generator::generate_value --
     *     Generate a value.
     */
    inline data_value
    generate_value(table_context_ptr table)
    {
        return random_data_value(table->value_format());
    }

    /*
     * kv_workload_generator::random_data_value --
     *     Generate a random data value, which can be used either as a key or a value.
     */
    data_value random_data_value(const std::string &format);

private:
    static const kv_workload_generator_spec _default_spec;

    std::shared_ptr<kv_workload> _workload_ptr;
    kv_workload &_workload;

    kv_workload_generator_spec _spec;
    kv_database_config _database_config;
    random _random;

    table_id_t _last_table_id;
    std::deque<table_context_ptr> _tables_list;
    std::unordered_map<table_id_t, table_context_ptr> _tables;

    txn_id_t _last_txn_id;
    std::deque<kv_workload_sequence_ptr> _sequences;
};

} /* namespace model */
