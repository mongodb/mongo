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

#include <algorithm>
#include <sstream>
#include "model/driver/kv_workload_generator.h"
#include "model/util.h"

namespace model {

/*
 * kv_workload_generator_spec::kv_workload_generator_spec --
 *     Create the generator specification using default probability values.
 */
kv_workload_generator_spec::kv_workload_generator_spec()
{
    disaggregated = 0; /* FIXME-WT-15042 Enable this when ready. */

    min_tables = 3;
    max_tables = 10;

    min_sequences = 200;
    max_sequences = 1000;
    max_concurrent_transactions = 3;

    max_recno = 100'000;
    max_value_uint64 = 1'000'000;

    column_fix = 0.1;
    column_var = 0.1;

    finish_transaction = 0.08;
    get = 0.5;
    insert = 0.75;
    remove = 0.15;
    set_commit_timestamp = 0.05;
    truncate = 0.005;

    checkpoint = 0.02;
    checkpoint_crash = 0.002;
    crash = 0.002;
    evict = 0.1;
    restart = 0.002;
    rollback_to_stable = 0.005;
    set_oldest_timestamp = 0.1;
    set_stable_timestamp = 0.2;

    get_existing = 0.9;
    remove_existing = 0.9;
    update_existing = 0.1;

    conn_logging = 0.5;

    prepared_transaction = 0.25;
    max_delay_after_prepare = 25; /* FIXME-WT-13232 This must be a small number until it's fixed. */
    use_set_commit_timestamp = 0.25;
    nonprepared_transaction_rollback = 0.1;
    prepared_transaction_rollback_after_prepare = 0.1;
    prepared_transaction_rollback_before_prepare = 0.1;

    timing_stress_ckpt_slow = 0.1;
    timing_stress_ckpt_evict_page = 0.1;
    timing_stress_ckpt_handle = 0.1;
    timing_stress_ckpt_stop = 0.1;
    timing_stress_compact_slow = 0.1;
    timing_stress_hs_ckpt_delay = 0.1;
    timing_stress_hs_search = 0.1;
    timing_stress_hs_sweep_race = 0.1;
    timing_stress_prepare_ckpt_delay = 0.1;
    timing_stress_commit_txn_slow = 0.1;
}

/*
 * kv_workload_generator::_default_spec --
 *     The default workload specification.
 */
const kv_workload_generator_spec kv_workload_generator::_default_spec;

/*
 * kv_workload_generator::kv_workload_generator --
 *     Create a new workload generator.
 */
kv_workload_generator::kv_workload_generator(const kv_workload_generator_spec &spec, uint64_t seed)
    : _workload_ptr(std::make_shared<kv_workload>()), _workload(*(_workload_ptr.get())),
      _last_table_id(0), _last_txn_id(0), _random(seed), _spec(spec)
{
}

/*
 * table_context::choose_existing_key --
 *     Randomly select an existing key.
 */
data_value
kv_workload_generator::table_context::choose_existing_key(random &r)
{
    if (_keys.empty())
        throw std::runtime_error("The table is empty");

    /*
     * For now, just pick a key at random. We may add weights based on number of operations per key
     * in the future.
     */
    size_t index = r.next_index(_keys.size());

    auto iter = _keys.begin();
    std::advance(iter, index);
    assert(iter != _keys.end());

    return iter->first;
}

/*
 * sequence_traversal::sequence_traversal --
 *     Initialize the traversal.
 */
kv_workload_generator::sequence_traversal::sequence_traversal(
  std::deque<kv_workload_sequence_ptr> &sequences,
  std::function<bool(kv_workload_sequence &)> barrier_fn)
    : _sequences(sequences), _barrier_fn(std::move(barrier_fn))
{
    for (kv_workload_sequence_ptr &seq : _sequences)
        _per_sequence_state.emplace(seq.get(), new sequence_state(seq.get()));

    /* Find the next sequence number based barrier in the traversal. */
    _barrier_seq_no = find_next_barrier(0);

    /* Find the sequences that can be processed next (with all dependencies satisfied).  */
    for (kv_workload_sequence_ptr &seq : _sequences) {
        sequence_state *seq_state = _per_sequence_state[seq.get()];
        if (seq_state->num_unsatisfied_dependencies == 0) {
            if (seq->seq_no() <= _barrier_seq_no)
                _runnable.push_back(seq_state);
            else
                _runnable_after_barrier.push_back(seq_state);
        }
    }
}

/*
 * sequence_traversal::advance_barrier --
 *     Advance the barrier.
 */
void
kv_workload_generator::sequence_traversal::advance_barrier()
{
    if (_barrier_seq_no >= _sequences.size())
        return;

    _barrier_seq_no = find_next_barrier(_barrier_seq_no + 1);
    for (auto i = _runnable_after_barrier.begin(); i != _runnable_after_barrier.end();)
        if ((*i)->sequence->seq_no() <= _barrier_seq_no) {
            _runnable.push_back(*i);
            i = _runnable_after_barrier.erase(i);
        } else
            i++;
}

/*
 * sequence_traversal::find_next_barrier --
 *     Find the next barrier.
 */
size_t
kv_workload_generator::sequence_traversal::find_next_barrier(size_t start)
{
    for (size_t i = start; i < _sequences.size(); i++) {
        kv_workload_sequence_ptr &seq = _sequences[i];
        if (_barrier_fn(*seq.get()))
            return seq->seq_no();
    }
    return _sequences.size();
}

/*
 * sequence_traversal::complete_all --
 *     Complete all "runnable" sequences and advance the iterator.
 */
void
kv_workload_generator::sequence_traversal::complete_all()
{
    std::deque<sequence_state *> new_runnable;
    for (sequence_state *r : _runnable)
        for (kv_workload_sequence *n : r->sequence->unblocks()) {
            sequence_state *n_state = _per_sequence_state[n];
            if (n_state->num_unsatisfied_dependencies.fetch_sub(1) == 1) {
                if (n->seq_no() <= _barrier_seq_no) {
                    if (n->type() == kv_workload_sequence_type::transaction)
                        new_runnable.push_back(n_state);
                    else
                        /* We need to do this to keep these sequences at the expected positions. */
                        new_runnable.push_front(n_state);
                } else
                    _runnable_after_barrier.push_back(n_state);
            }
        }

    _runnable = std::move(new_runnable);
    if (_runnable.empty())
        advance_barrier();
}

/*
 * sequence_traversal::complete_one --
 *     Complete one "runnable" sequence and advance the iterator.
 */
void
kv_workload_generator::sequence_traversal::complete_one(sequence_state *s)
{
    for (auto i = _runnable.begin(); i != _runnable.end(); i++)
        if (*i == s) {
            _runnable.erase(i);
            break;
        }

    for (kv_workload_sequence *n : s->sequence->unblocks()) {
        sequence_state *n_state = _per_sequence_state[n];
        if (n_state->num_unsatisfied_dependencies.fetch_sub(1) == 1) {
            if (n->seq_no() <= _barrier_seq_no) {
                if (n->type() == kv_workload_sequence_type::transaction)
                    _runnable.push_back(n_state);
                else
                    /* We need to do this to keep these sequences at the expected positions. */
                    _runnable.push_front(n_state);
            } else
                _runnable_after_barrier.push_back(n_state);
        }
    }

    if (_runnable.empty())
        advance_barrier();
}

/*
 * kv_workload_generator::assign_timestamps --
 *     Assign timestamps to operations in a sequence.
 */
void
kv_workload_generator::assign_timestamps(kv_workload_sequence &sequence, timestamp_t first,
  timestamp_t last, timestamp_t &oldest, timestamp_t &stable)
{
    if (sequence.size() > 1 && first + 10 >= last)
        throw model_exception("Need a bigger difference between first and last timestamp");

    /* Assume that there is at most one transaction in the sequence. */
    bool prepared = false;

    /* Collect operations that require timestamps. */
    std::deque<operation::any *> timestamped_ops;
    for (size_t i = 0; i < sequence.size(); i++) {
        operation::any &op = sequence[i];
        if (std::holds_alternative<operation::commit_transaction>(op) ||
          std::holds_alternative<operation::prepare_transaction>(op) ||
          std::holds_alternative<operation::set_commit_timestamp>(op) ||
          std::holds_alternative<operation::set_oldest_timestamp>(op) ||
          std::holds_alternative<operation::set_stable_timestamp>(op))
            timestamped_ops.push_back(&op);
        if (std::holds_alternative<operation::prepare_transaction>(op))
            prepared = true;
    }
    if (timestamped_ops.empty())
        return;

    /* Assign timestamps. */
    size_t timestamps_needed = timestamped_ops.size() + (prepared ? 1 /* durable timestamp */ : 0);
    double step = (last - first) / (double)timestamps_needed;
    double x = first;
    size_t count = 0;
    for (operation::any *op : timestamped_ops) {

        /* Generate the next timestamp. */
        x = x + 1 + _random.next_double() * (first + (++count) * step - x);
        timestamp_t t = std::min((timestamp_t)x, last);

        /* Assign. */
        if (std::holds_alternative<operation::commit_transaction>(*op)) {
            std::get<operation::commit_transaction>(*op).commit_timestamp = t;
            if (prepared) {
                x = x + 1 + _random.next_double() * (first + (++count) * step - x);
                std::get<operation::commit_transaction>(*op).durable_timestamp =
                  std::min((timestamp_t)x, last);
            }
        } else if (std::holds_alternative<operation::prepare_transaction>(*op))
            std::get<operation::prepare_transaction>(*op).prepare_timestamp = t;
        else if (std::holds_alternative<operation::set_commit_timestamp>(*op))
            std::get<operation::set_commit_timestamp>(*op).commit_timestamp = t;
        else if (std::holds_alternative<operation::set_oldest_timestamp>(*op))
            std::get<operation::set_oldest_timestamp>(*op).oldest_timestamp = oldest = t;
        else if (std::holds_alternative<operation::set_stable_timestamp>(*op))
            std::get<operation::set_stable_timestamp>(*op).stable_timestamp = stable = t;
    }
}

/*
 * kv_workload_generator::choose_table --
 *     Choose a table for an operation, creating one if necessary.
 */
kv_workload_generator::table_context_ptr
kv_workload_generator::choose_table(kv_workload_sequence_ptr txn)
{
    /* TODO: In the future, transaction context will specify its own table distribution. */
    (void)txn;

    if (_tables.empty())
        throw model_exception("No tables.");

    return _tables_list[_random.next_index(_tables_list.size())];
}

/*
 * kv_workload_generator::generate_connection_stress_config --
 *     Generate random time stress configurations.
 */
std::string
kv_workload_generator::generate_connection_stress_config()
{
    std::string wt_env_config;
    probability_switch(_random.next_float())
    {
        probability_case(_spec.timing_stress_ckpt_slow) wt_env_config +=
          "timing_stress_for_test=[checkpoint_slow]";
        probability_case(_spec.timing_stress_ckpt_evict_page) wt_env_config +=
          "timing_stress_for_test=[checkpoint_evict_page]";
        probability_case(_spec.timing_stress_ckpt_handle) wt_env_config +=
          "timing_stress_for_test=[checkpoint_handle]";
        probability_case(_spec.timing_stress_ckpt_stop) wt_env_config +=
          "timing_stress_for_test=[checkpoint_stop]";
        probability_case(_spec.timing_stress_compact_slow) wt_env_config +=
          "timing_stress_for_test=[compact_slow]";
        probability_case(_spec.timing_stress_hs_ckpt_delay) wt_env_config +=
          "timing_stress_for_test=[history_store_checkpoint_delay]";
        probability_case(_spec.timing_stress_hs_search) wt_env_config +=
          "timing_stress_for_test=[history_store_search]";
        probability_case(_spec.timing_stress_hs_sweep_race) wt_env_config +=
          "timing_stress_for_test=[history_store_sweep_race]";
        probability_case(_spec.timing_stress_prepare_ckpt_delay) wt_env_config +=
          "timing_stress_for_test=[prepare_checkpoint_delay]";
        probability_case(_spec.timing_stress_commit_txn_slow) wt_env_config +=
          "timing_stress_for_test=[commit_transaction_slow]";
    }
    return wt_env_config;
}

/*
 * kv_workload_generator::generate_connection_log_config --
 *     Generate random WiredTiger log configurations.
 */
std::string
kv_workload_generator::generate_connection_log_config()
{
    std::string wt_env_config;

    if (_spec.conn_logging > _random.next_float())
        wt_env_config = model::join(wt_env_config, "log=(enabled=true)");

    return wt_env_config;
}

/*
 * kv_workload_generator::create_table --
 *     Create a table.
 */
void
kv_workload_generator::create_table()
{
    table_id_t id = ++_last_table_id;
    std::string name = "table" + std::to_string(id);
    std::string key_format = "Q";
    std::string value_format = "Q";
    kv_table_type type = kv_table_type::row;

    probability_switch(_random.next_float())
    {
        probability_case(_spec.column_fix)
        {
            key_format = "r";
            value_format = "8t";
            type = kv_table_type::column_fix;
        }
        probability_case(_spec.column_var)
        {
            key_format = "r";
            type = kv_table_type::column;
        }
    }

    table_context_ptr table =
      std::make_shared<table_context>(id, name, key_format, value_format, type);
    _tables_list.push_back(table);
    _tables[id] = table;

    _workload << operation::create_table(table->id(), table->name().c_str(),
      table->key_format().c_str(), table->value_format().c_str());
}

/*
 * kv_workload_transaction_ptr --
 *     Generate a random transaction.
 */
kv_workload_sequence_ptr
kv_workload_generator::generate_transaction(size_t seq_no)
{
    /* Choose the transaction ID and whether this will be a prepared transaction. */
    txn_id_t txn_id = ++_last_txn_id;
    bool prepared = _random.next_float() < _spec.prepared_transaction;

    /* Start the new transaction. */
    kv_workload_sequence_ptr txn_ptr =
      std::make_shared<kv_workload_sequence>(seq_no, kv_workload_sequence_type::transaction);
    kv_workload_sequence &txn = *txn_ptr.get();
    txn << operation::begin_transaction(txn_id);

    /* If we're going to use "set commit timestamp," start with it. */
    bool use_set_commit_timestamp =
      !prepared && _random.next_float() < _spec.use_set_commit_timestamp;
    if (use_set_commit_timestamp)
        txn << operation::set_commit_timestamp(txn_id, k_timestamp_none /* placeholder */);

    /* Add all operations. But do not actually fill in timestamps; we'll do that later. */
    bool done = false;
    while (!done) {
        float total = _spec.finish_transaction + _spec.get + _spec.insert + _spec.remove +
          _spec.set_commit_timestamp + _spec.truncate;
        probability_switch(_random.next_float() * total)
        {
            probability_case(_spec.finish_transaction)
            {
                if (prepared) {
                    if (_random.next_float() < _spec.prepared_transaction_rollback_before_prepare)
                        txn << operation::rollback_transaction(txn_id);
                    else {
                        txn << operation::prepare_transaction(txn_id, k_timestamp_none);

                        /* Add a delay before finishing the transaction. */
                        size_t delay = _random.next_uint64(_spec.max_delay_after_prepare);
                        for (size_t i = 0; i < delay; i++)
                            txn << operation::nop();

                        /* Finish the transaction. */
                        if (_random.next_float() <
                          _spec.prepared_transaction_rollback_after_prepare)
                            txn << operation::rollback_transaction(txn_id);
                        else
                            txn << operation::commit_transaction(txn_id);
                    }
                } else
                    txn << operation::commit_transaction(txn_id);
                done = true;
            }
            probability_case(_spec.get)
            {
                table_context_ptr table = choose_table(txn_ptr);
                /*
                 * FIXME-WT-14903 Under FLCS, get operations expose some relatively complex effects.
                 * For instance, eviction changes implicit records to explicit. To re-enable this,
                 * check that all FLCS interactions are accounted for.
                 */
                if (table->type() == kv_table_type::column_fix)
                    break;

                data_value key = generate_key(table, op_category::get);
                /* A get operation shouldn't affect context. */
                txn << operation::get(table->id(), txn_id, key);
            }
            probability_case(_spec.insert)
            {
                table_context_ptr table = choose_table(txn_ptr);
                data_value key = generate_key(table, op_category::update);
                data_value value = generate_value(table);
                table->update_key(key);
                txn << operation::insert(table->id(), txn_id, key, value);
            }
            probability_case(_spec.remove)
            {
                table_context_ptr table = choose_table(txn_ptr);
                data_value key = generate_key(table, op_category::remove);
                table->remove_key(key);
                txn << operation::remove(table->id(), txn_id, key);
            }
            probability_case(_spec.set_commit_timestamp)
            {
                if (use_set_commit_timestamp)
                    txn << operation::set_commit_timestamp(txn_id, k_timestamp_none);
            }
            probability_case(_spec.truncate)
            {
                table_context_ptr table = choose_table(txn_ptr);

                /*
                 * FIXME-WT-13232 Don't use truncate on FLCS tables, because a truncate on an FLCS
                 * table can conflict with operations adjacent to the truncation range's key range.
                 * For example, if a user wants to truncate range 10-12 on a table with keys [10,
                 * 11, 12, 13, 14], a concurrent update to key 13 would result in a conflict (while
                 * an update to 14 would be able proceed).
                 *
                 * FIXME-WT-13350 Similarly, truncating an implicitly created range of keys in an
                 * FLCS table conflicts with a concurrent insert operation that caused this range of
                 * keys to be created.
                 *
                 * The workload generator cannot currently account for this, so don't use truncate
                 * with FLCS tables for now.
                 */
                if (table->type() == kv_table_type::column_fix)
                    break;

                data_value start = generate_key(table);
                data_value stop = generate_key(table);
                if (start > stop)
                    std::swap(start, stop);
                table->remove_key_range(start, stop);
                txn << operation::truncate(table->id(), txn_id, start, stop);
            }
        }
    }

    return txn_ptr;
}

/*
 * kv_workload_generator::run --
 *     Generate the workload.
 */
void
kv_workload_generator::run()
{
    /* Top-level configuration. */
    if (_random.next_float() < _spec.disaggregated) {
        _database_config.disaggregated = true;
        _workload << operation::config("database", "disaggregated=true");

        /* Adjust the specs based on what's not supported. */
        _spec.column_fix = 0;
        _spec.column_var = 0;
        _spec.rollback_to_stable = 0;

        /* FIXME-WT-15040 Prepared transactions are not yet supported. */
        _spec.prepared_transaction = 0;

        /* FIXME-WT-15041 Handling abandoned checkpoints is not yet supported. */
        _spec.crash = 0;
        _spec.checkpoint_crash = 0;

        /* FIXME-WT-14998 Truncate is not yet supported. */
        _spec.truncate = 0;
    }

    /* Create tables. */
    uint64_t num_tables = _random.next_uint64(_spec.min_tables, _spec.max_tables);
    for (uint64_t i = 0; i < num_tables; i++)
        create_table();

    /*
     * Start by generating a serialized workload. The workload contains operation sequences in
     * order, with no overlap between different transactions and between transactions and special
     * operations such as "checkpoint" or "set stable timestamp."
     *
     * Generating one transaction at a time is more straightforward than trying to generate a
     * concurrent workload directly. Working on one transaction at a time, makes it, for example,
     * easier to reason about aspects such as operation probabilities, transaction length, and
     * causal ordering. It will also make it easier to implement a concurrent workload execution in
     * the future, which would allow us to achieve better test coverage.
     *
     * We will add concurrency to the workload later, respecting dependencies between transactions,
     * so that a concurrent schedule would have the same semantics as the serial-equivalent
     * schedule.
     *
     * At this point, we will generate just the operations without timestamps; we will add
     * timestamps later after we figure out all the dependencies between transactions. This would
     * allow us to produce more interesting schedules.
     *
     * And why do we go through all this trouble? Most existing workload generators play tricks such
     * as partitioning key spaces and timestamps between threads, which puts constraints on the
     * kinds of database states that can be generated. We would like to generate states and tree
     * shapes that cannot be generated by existing workload generators, so that we can explore as
     * many interesting corner cases as possible.
     *
     * For disaggregated storage, we need to ensure that the stable timestamp is set before the
     * first checkpoint (as this is required by precise checkpoints), and that the checkpoint is
     * taken before the first crash (as this is required for the tables to exist after crash).
     */
    uint64_t num_sequences = _random.next_uint64(_spec.min_sequences, _spec.max_sequences);
    bool has_checkpoint = false;
    bool has_stable_timestamp = false;
    for (uint64_t i = 0; i < num_sequences; i++)
        probability_switch(_random.next_float())
        {
            probability_case(_spec.checkpoint)
            {
                if (_database_config.disaggregated && !has_stable_timestamp)
                    break;

                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::checkpoint);
                *p << operation::checkpoint();
                _sequences.push_back(p);

                has_checkpoint = true;
            }
            probability_case(_spec.checkpoint_crash)
            {
                if (_database_config.disaggregated && !has_checkpoint)
                    break;

                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::checkpoint_crash);
                uint64_t random_number = _random.next_uint64(1000);
                *p << operation::checkpoint_crash(random_number);
                _sequences.push_back(p);

                if (!has_checkpoint)
                    has_stable_timestamp = false;
            }
            probability_case(_spec.crash)
            {
                if (_database_config.disaggregated && !has_checkpoint)
                    break;

                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::crash);
                *p << operation::crash();
                _sequences.push_back(p);

                if (!has_checkpoint)
                    has_stable_timestamp = false;
            }
            probability_case(_spec.evict)
            {
                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::evict);
                table_context_ptr table = choose_table(std::move(kv_workload_sequence_ptr()));
                data_value key = generate_key(table, op_category::evict);
                *p << operation::evict(table->id(), key);
                _sequences.push_back(p);
            }
            probability_case(_spec.restart)
            {
                if (_database_config.disaggregated && !has_stable_timestamp)
                    break; /* Need stable timestamp before shutdown takes a checkpoint. */

                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::restart);
                *p << operation::restart();
                _sequences.push_back(p);

                has_checkpoint = true; /* Shutdown takes a checkpoint. */
            }
            probability_case(_spec.rollback_to_stable)
            {
                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::rollback_to_stable);
                *p << operation::rollback_to_stable();
                _sequences.push_back(p);
            }
            probability_case(_spec.set_oldest_timestamp)
            {
                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::set_oldest_timestamp);
                *p << operation::set_oldest_timestamp(k_timestamp_none); /* Placeholder. */
                _sequences.push_back(p);
            }
            probability_case(_spec.set_stable_timestamp)
            {
                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::set_stable_timestamp);
                *p << operation::set_stable_timestamp(k_timestamp_none); /* Placeholder. */
                _sequences.push_back(p);

                has_stable_timestamp = true;
            }
            probability_default
            {
                _sequences.push_back(generate_transaction(_sequences.size()));
            }
        }

    /* Ensure that the stable timestamp is set for disaggregated storage. */
    if (_database_config.disaggregated && !has_stable_timestamp) {
        kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
          _sequences.size(), kv_workload_sequence_type::set_stable_timestamp);
        *p << operation::set_stable_timestamp(k_timestamp_none); /* Placeholder. */
        _sequences.push_back(p);
        has_stable_timestamp = true;
    }

    /*
     * Now that we have a serial-equivalent schedule, we need to ensure that special,
     * non-transactional operation sequences, such as "checkpoint" or "set stable timestamp," will
     * get executed roughly at the positions selected by the serial workload generation above. To do
     * this, we will make each preceding transaction, and the last special sequences, as
     * dependencies.
     */
    for (size_t last_nontransaction = 0, i = 0; i < _sequences.size(); i++)
        if (_sequences[i]->type() != kv_workload_sequence_type::transaction) {
            for (size_t prev = last_nontransaction; prev < i; prev++)
                _sequences[prev]->must_finish_before(_sequences[i].get());
            last_nontransaction = i;
        }

    /*
     * Rollback to stable must be executed outside of transactions, so make sure to add the
     * corresponding dependencies.
     */
    for (size_t i = 0; i < _sequences.size(); i++)
        if (_sequences[i]->type() == kv_workload_sequence_type::rollback_to_stable) {
            for (size_t j = 0; j < i; j++)
                _sequences[j]->must_finish_before(_sequences[i].get());
            for (size_t j = i + 1; j < _sequences.size(); j++)
                _sequences[i]->must_finish_before(_sequences[j].get());
        }

    /*
     * Find dependencies between workload sequences: If two sequences operate on the same keys, they
     * must be run serially to preserve the serial workload's semantics. It is not sufficient to
     * just ensure that conflicting transactions commit in the correct order, because WiredTiger
     * would abort the second transaction with WT_ROLLBACK.
     */
    for (size_t i = 0; i < _sequences.size(); i++)
        for (size_t j = i + 1; j < _sequences.size(); j++)
            if (_sequences[i]->overlaps_with(_sequences[j]))
                _sequences[i]->must_finish_before(_sequences[j].get());

    /*
     * Fill in the timestamps. Break up the collection of sequences into blocks of transactions
     * (breaking them up by non-transactional sequences, such as the ones for "set stable
     * timestamp"), and traverse them in the dependency order, processing a block of independent
     * sequences at a time. Keep track of the oldest and stable timestamps to ensure that we assign
     * them in the correct order.
     */
    const auto barrier_fn = [](kv_workload_sequence &seq) {
        return seq.type() != kv_workload_sequence_type::transaction;
    };

    timestamp_t step = 1000;
    timestamp_t first = step + 1;
    timestamp_t last = first + step;

    timestamp_t ckpt_oldest = k_timestamp_none;
    timestamp_t ckpt_stable = k_timestamp_none;
    timestamp_t oldest = k_timestamp_none;
    timestamp_t stable = k_timestamp_none;

    for (sequence_traversal t(_sequences, barrier_fn); t.has_more(); t.complete_all()) {
        for (sequence_state *s : t.runnable()) {

            /* Simulate how checkpoints, crashes, and restarts manipulate the timestamps. */
            if (s->sequence->type() == kv_workload_sequence_type::checkpoint ||
              s->sequence->type() == kv_workload_sequence_type::restart ||
              s->sequence->type() == kv_workload_sequence_type::rollback_to_stable) {
                ckpt_oldest = oldest;
                ckpt_stable = stable;
                if (ckpt_stable == k_timestamp_none)
                    ckpt_oldest = k_timestamp_none;
            }
            if (s->sequence->type() == kv_workload_sequence_type::crash ||
              s->sequence->type() == kv_workload_sequence_type::checkpoint_crash ||
              s->sequence->type() == kv_workload_sequence_type::restart) {
                oldest = ckpt_oldest;
                stable = ckpt_stable;
            }

            /* Assign the timestamps. */
            if (s->sequence->type() == kv_workload_sequence_type::set_oldest_timestamp)
                /* The oldest timestamp must lag behind the stable timestamp. */
                assign_timestamps(*s->sequence, oldest,
                  stable != k_timestamp_none ? stable : first - step, oldest, stable);
            else if (s->sequence->type() == kv_workload_sequence_type::set_stable_timestamp)
                /* The stable timestamp must lag behind the other operations. */
                assign_timestamps(*s->sequence, first - step, last - step, oldest, stable);
            else
                assign_timestamps(*s->sequence, first, last, oldest, stable);
        }

        first = last + 1;
        last = first + step - 1;
    }

    /*
     * Create an execution schedule, mixing operations from different transactions. We do this by
     * traversing the sequences in dependency order, and at each step, choosing one runnable
     * operation at random.
     */
    for (sequence_traversal t(_sequences); t.has_more();) {
        const std::deque<sequence_state *> &runnable = t.runnable();

        /* Choose a sequence. */
        sequence_state *s = runnable[_random.next_index(
          std::min(runnable.size(), _spec.max_concurrent_transactions))];

        /* Get the next operation from the sequence. */
        if (s->next_operation_index >= s->sequence->size())
            throw model_exception("Internal error: No more operations left in a sequence");
        const operation::any &op = (*s->sequence)[s->next_operation_index++];
        if (!std::holds_alternative<operation::nop>(op))
            _workload << kv_workload_operation(op, s->sequence->seq_no());

        /* If the operation resulted in a database crash or restart, stop all started sequences. */
        if (std::holds_alternative<operation::crash>(op) ||
          std::holds_alternative<operation::checkpoint_crash>(op) ||
          std::holds_alternative<operation::restart>(op)) {
            t.complete_all();
            continue;
        }

        /* If this is the last operation, complete the sequence execution. */
        if (s->next_operation_index >= s->sequence->size())
            t.complete_one(s);
    }

    /*
     * Validate that the workload is correct, such checking that we filled in the timestamps in the
     * correct order.
     */
    _workload.verify();
}

/*
 * kv_workload_generator::generate_key --
 *     Generate a key.
 */
data_value
kv_workload_generator::generate_key(table_context_ptr table, op_category op)
{
    /* Get the probability of choosing an existing key. */
    float p_existing = 0;
    switch (op) {
    case op_category::none:
        p_existing = 0;
        break;

    case op_category::evict:
        p_existing = 1.0;
        break;

    case op_category::get:
        p_existing = _spec.get_existing;
        break;

    case op_category::remove:
        p_existing = _spec.remove_existing;
        break;

    case op_category::update:
        p_existing = _spec.update_existing;
        break;
    }

    /* See if we should get an existing key. */
    if (!table->empty() && _random.next_float() < p_existing)
        return table->choose_existing_key(_random);

    /* Otherwise generate a random key. It's okay if the key already exists. */
    return random_data_value(table->key_format());
}

/*
 * kv_workload_generator::random_data_value --
 *     Generate a random data value, which can be used either as a key or a value.
 */
data_value
kv_workload_generator::random_data_value(const std::string &format)
{
    if (format.empty())
        throw model_exception("The format cannot be an empty string");

    const char *f = format.c_str();

    /* Get the length. */
    unsigned length = 0;
    if (isdigit(f[0]))
        length = (u_int)parse_uint64(f, &f);

    if (strlen(f) != 1)
        throw model_exception("The model does not currently support structs");

    switch (f[0]) {
    case 'Q':
        return data_value(_random.next_uint64(_spec.max_value_uint64));
    case 'r':
        return data_value(_random.next_uint64(1, _spec.max_recno));
    case 't':
        if (length == 0)
            length = 1;
        if (length > 8)
            throw model_exception("The length cannot be higher than 8 for type \"t\"");
        return data_value(_random.next_uint64(1 << (length - 1)));
    default:
        throw model_exception("Unsupported type.");
    };
}

} /* namespace model */
