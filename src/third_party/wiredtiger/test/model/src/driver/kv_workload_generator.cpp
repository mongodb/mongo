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
    min_tables = 3;
    max_tables = 10;

    min_sequences = 200;
    max_sequences = 1000;
    max_concurrent_transactions = 3;

    max_value_uint64 = 1'000'000;

    finish_transaction = 0.08;
    insert = 0.75;
    remove = 0.15;
    set_commit_timestamp = 0.05;
    truncate = 0.005;

    checkpoint = 0.02;
    crash = 0.002;
    restart = 0.002;
    set_stable_timestamp = 0.2;

    prepared_transaction = 0.25;
    use_set_commit_timestamp = 0.25;
    nonprepared_transaction_rollback = 0.1;
    prepared_transaction_rollback_after_prepare = 0.1;
    prepared_transaction_rollback_before_prepare = 0.1;
}

/*
 * kv_workload_generator::kv_workload_generator --
 *     Create a new workload generator.
 */
kv_workload_generator::kv_workload_generator(kv_workload_generator_spec spec, uint64_t seed)
    : _workload_ptr(std::make_shared<kv_workload>()), _workload(*(_workload_ptr.get())),
      _last_table_id(0), _last_txn_id(0), _random(seed), _spec(spec)
{
}

/*
 * sequence_traversal::sequence_traversal --
 *     Initialize the traversal.
 */
kv_workload_generator::sequence_traversal::sequence_traversal(
  std::deque<kv_workload_sequence_ptr> &sequences,
  std::function<bool(kv_workload_sequence &)> barrier_fn)
    : _sequences(sequences), _barrier_fn(barrier_fn)
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

    _runnable = new_runnable;
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
 * kv_workload_generator::assert_timestamps --
 *     Assert that the timestamps are assigned correctly. Call this function one sequence at a time.
 */
void
kv_workload_generator::assert_timestamps(
  const kv_workload_sequence &sequence, const operation::any &op, timestamp_t &stable)
{
    if (std::holds_alternative<operation::set_stable_timestamp>(op)) {
        timestamp_t t = std::get<operation::set_stable_timestamp>(op).stable_timestamp;
        if (t < stable) {
            std::ostringstream err;
            err << "The stable timestamp went backwards: " << stable << " -> " << t << " (sequence "
                << sequence.seq_no() << ")" << std::endl;
            throw model_exception(err.str());
        }
        stable = t;
    }

    if (std::holds_alternative<operation::prepare_transaction>(op)) {
        timestamp_t t = std::get<operation::prepare_transaction>(op).prepare_timestamp;
        if (t < stable) {
            std::ostringstream err;
            err << "Prepare timestamp is before the stable timestamp: " << t << " < " << stable
                << " (sequence " << sequence.seq_no() << ")" << std::endl;
            throw model_exception(err.str());
        }
    }

    if (std::holds_alternative<operation::set_commit_timestamp>(op)) {
        timestamp_t t = std::get<operation::set_commit_timestamp>(op).commit_timestamp;
        if (t < stable) {
            std::ostringstream err;
            err << "Commit timestamp is before the stable timestamp: " << t << " < " << stable
                << " (sequence " << sequence.seq_no() << ")" << std::endl;
            throw model_exception(err.str());
        }
    }

    if (std::holds_alternative<operation::commit_transaction>(op)) {
        timestamp_t t = std::get<operation::commit_transaction>(op).commit_timestamp;
        if (t < stable) {
            std::ostringstream err;
            err << "Commit timestamp is before the stable timestamp: " << t << " < " << stable
                << " (sequence " << sequence.seq_no() << ")" << std::endl;
            throw model_exception(err.str());
        }
        t = std::get<operation::commit_transaction>(op).durable_timestamp;
        if (t < stable && t != k_timestamp_none) {
            std::ostringstream err;
            err << "Durable timestamp is before the stable timestamp: " << t << " < " << stable
                << " (sequence " << sequence.seq_no() << ")" << std::endl;
            throw model_exception(err.str());
        }
    }
}

/*
 * kv_workload_generator::assign_timestamps --
 *     Assign timestamps to operations in a sequence.
 */
void
kv_workload_generator::assign_timestamps(
  kv_workload_sequence &sequence, timestamp_t first, timestamp_t last)
{
    if (first + 10 >= last)
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
        timestamp_t t = (timestamp_t)x;

        /* Assign. */
        if (std::holds_alternative<operation::commit_transaction>(*op)) {
            std::get<operation::commit_transaction>(*op).commit_timestamp = t;
            if (prepared) {
                x = x + 1 + _random.next_double() * (first + (++count) * step - x);
                std::get<operation::commit_transaction>(*op).durable_timestamp = (timestamp_t)x;
            }
        }
        if (std::holds_alternative<operation::prepare_transaction>(*op))
            std::get<operation::prepare_transaction>(*op).prepare_timestamp = t;
        if (std::holds_alternative<operation::set_commit_timestamp>(*op))
            std::get<operation::set_commit_timestamp>(*op).commit_timestamp = t;
        if (std::holds_alternative<operation::set_stable_timestamp>(*op))
            std::get<operation::set_stable_timestamp>(*op).stable_timestamp = t;
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

    table_context_ptr table = std::make_shared<table_context>(id, name, key_format, value_format);
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
        float total = _spec.finish_transaction + _spec.insert + _spec.remove +
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
            probability_case(_spec.insert)
            {
                table_context_ptr table = choose_table(txn_ptr);
                data_value key = generate_key(table);
                data_value value = generate_value(table);
                txn << operation::insert(table->id(), txn_id, key, value);
            }
            probability_case(_spec.remove)
            {
                table_context_ptr table = choose_table(txn_ptr);
                data_value key = generate_key(table);
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
                data_value start = generate_key(table);
                data_value stop = generate_key(table);
                if (start > stop)
                    std::swap(start, stop);
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
     */
    uint64_t num_sequences = _random.next_uint64(_spec.min_sequences, _spec.max_sequences);
    for (uint64_t i = 0; i < num_sequences; i++)
        probability_switch(_random.next_float())
        {
            probability_case(_spec.checkpoint)
            {
                kv_workload_sequence_ptr p =
                  std::make_shared<kv_workload_sequence>(_sequences.size());
                *p << operation::checkpoint();
                _sequences.push_back(p);
            }
            probability_case(_spec.crash)
            {
                kv_workload_sequence_ptr p =
                  std::make_shared<kv_workload_sequence>(_sequences.size());
                *p << operation::crash();
                _sequences.push_back(p);
            }
            probability_case(_spec.restart)
            {
                kv_workload_sequence_ptr p =
                  std::make_shared<kv_workload_sequence>(_sequences.size());
                *p << operation::restart();
                _sequences.push_back(p);
            }
            probability_case(_spec.set_stable_timestamp)
            {
                kv_workload_sequence_ptr p = std::make_shared<kv_workload_sequence>(
                  _sequences.size(), kv_workload_sequence_type::set_stable_timestamp);
                *p << operation::set_stable_timestamp(k_timestamp_none); /* Placeholder. */
                _sequences.push_back(p);
            }
            probability_default
            {
                _sequences.push_back(generate_transaction(_sequences.size()));
            }
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
     * sequences at a time.
     */
    const auto barrier_fn = [](kv_workload_sequence &seq) {
        return seq.type() != kv_workload_sequence_type::transaction;
    };
    timestamp_t step = 1000;
    timestamp_t first = step + 1;
    timestamp_t last = first + step;

    for (sequence_traversal t(_sequences, barrier_fn); t.has_more(); t.complete_all()) {
        for (sequence_state *s : t.runnable())
            if (s->sequence->type() == kv_workload_sequence_type::set_stable_timestamp)
                /* Operations such as "set stable timestamp" must lag a little behind. */
                assign_timestamps(*s->sequence, first - step, last - step);
            else
                assign_timestamps(*s->sequence, first, last);
        first = last + 1;
        last = first + step - 1;
    }

    /*
     * Create an execution schedule, mixing operations from different transactions. We do this by
     * traversing the sequences in dependency order, and at each step, choosing one runnable
     * operation at random.
     */
    timestamp_t stable = k_timestamp_none;
    for (sequence_traversal t(_sequences); t.has_more();) {
        const std::deque<sequence_state *> &runnable = t.runnable();

        /* Choose a sequence. */
        sequence_state *s = runnable[_random.next_index(
          std::min(runnable.size(), _spec.max_concurrent_transactions))];

        /* Get the next operation from the sequence. */
        if (s->next_operation_index >= s->sequence->size())
            throw model_exception("Internal error: No more operations left in a sequence");
        const operation::any &op = (*s->sequence)[s->next_operation_index++];
        _workload << kv_workload_operation(op, s->sequence->seq_no());

        /* Validate that we filled in the timestamps in the correct order. */
        assert_timestamps(*s->sequence, op, stable);

        /* If the operation resulted in a database crash or restart, stop all started sequences. */
        if (std::holds_alternative<operation::crash>(op) ||
          std::holds_alternative<operation::restart>(op)) {
            t.complete_all();
            continue;
        }

        /* If this is the last operation, complete the sequence execution. */
        if (s->next_operation_index >= s->sequence->size())
            t.complete_one(s);
    }
}

/*
 * kv_workload_generator::random_data_value --
 *     Generate a random data value, which can be used either as a key or a value.
 */
data_value
kv_workload_generator::random_data_value(const std::string &format)
{
    if (format.length() != 1)
        throw model_exception("The model does not currently support structs or types with sizes");

    switch (format[0]) {
    case 'Q':
        return data_value(_random.next_uint64(_spec.max_value_uint64));
    default:
        throw model_exception("Unsupported type.");
    };
}

} /* namespace model */
