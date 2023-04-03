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
#include <map>
#include <ostream>
#include <string>
#include <vector>

// For convenience: A type exposed to Python that cannot be negative.
typedef unsigned int uint_t;

namespace workgen {

struct ContextInternal;
struct OperationInternal;
struct TableInternal;
struct ThreadRunner;
struct Thread;
struct Transaction;

#ifndef SWIG
struct OptionsList {
    OptionsList() = default;
    OptionsList(const OptionsList &other);

    void add_int(const std::string& name, int default_value, const std::string& desc);
    void add_bool(const std::string& name, bool default_value, const std::string& desc);
    void add_double(const std::string& name, double default_value, const std::string& desc);
    void add_string(const std::string& name, const std::string &default_value,
      const std::string& desc);

    std::string help() const;
    std::string help_description(const std::string& option_name) const;
    std::string help_type(const std::string& option_name) const;

private:
    void add_option(const std::string&name, const std::string typestr,
      const std::string&desc);
    typedef std::pair<std::string, std::string> TypeDescPair;
    std::map<std::string, TypeDescPair> _option_map;
};
#endif

/*
 * These classes are all exposed to Python via SWIG. While they may contain
 * data that is private to C++, such data must not prevent the objects from
 * being shared. Tables, Keys, Values, Operations and Threads can be shared: a
 * single Key object might appear in many operations; Operations may appear
 * multiple times in a Thread or in different Threads; the same Thread may
 * appear multiple times in a Workload list, etc.
 *
 * Certain kinds of state are allowed: A Table contains a unique pointer that
 * is used within the internal part of the Context.  Stats contain lots
 * of state, but is made available after a Workload.run().
 *
 * Python controls the lifetime of (nearly) all objects of these classes.
 * The exception is Stat/Track objects, which are also created/used
 * internally to calculate and show statistics during a run.
 */
struct Track {
    // Threads maintain the total thread operation and total latency they've
    // experienced.

    uint64_t ops_in_progress;           // Total operations not completed
    uint64_t ops;                       // Total operations completed
    uint64_t rollbacks;                 // Total operations rolled back
    uint64_t latency_ops;               // Total ops sampled for latency
    uint64_t latency;                   // Total latency
    uint64_t bucket_ops;                // Computed for percentile_latency

    // Minimum/maximum latency, shared with the monitor thread, that is, the
    // monitor thread clears it so it's recalculated again for each period.

    uint32_t min_latency;                // Minimum latency (uS)
    uint32_t max_latency;                // Maximum latency (uS)

    Track(bool latency_tracking = false);
    Track(const Track &other);
    ~Track();

    void add(Track&, bool reset = false);
    void assign(const Track&);
    uint64_t average_latency() const;
    void begin();
    void clear();
    void complete();
    void complete_with_latency(uint64_t usecs);
    uint64_t percentile_latency(int percent) const;
    void subtract(const Track&);
    void track_latency(bool);
    bool track_latency() const { return (us != NULL); }

    void _get_us(long *);
    void _get_ms(long *);
    void _get_sec(long *);

private:
    // Latency buckets. From python, accessed via methods us(), ms(), sec()
    uint32_t *us;                        // < 1us ... 1000us
    uint32_t *ms;                        // < 1ms ... 1000ms
    uint32_t *sec;                       // < 1s 2s ... 100s

    Track & operator=(const Track &other);   // use explicit assign method
};

struct Stats {
    Track checkpoint;
    Track insert;
    Track not_found;
    Track read;
    Track remove;
    Track update;
    Track truncate;

    Stats(bool latency = false);
    Stats(const Stats &other);
    ~Stats() = default;

    void add(Stats&, bool reset = false);
    void assign(const Stats&);
    void clear();
    void describe(std::ostream &os) const;
#ifndef SWIG
    void final_report(std::ostream &os, timespec &totalsecs) const;
    void report(std::ostream &os) const;
#endif
    void subtract(const Stats&);
    void track_latency(bool);
    bool track_latency() const { return (insert.track_latency()); }

private:
    Stats & operator=(const Stats &other);   // use explicit assign method
};

// A Context tracks the current record number for each uri, used
// for key generation.
struct Context {
    bool _verbose;
    ContextInternal *_internal;

    Context();
    ~Context();
    void describe(std::ostream &os) const {
	os << "Context: verbose " << (_verbose ? "true" : "false");
    }

#ifndef SWIG
    Context& operator=(const Context &other);
#endif
};

// To prevent silent errors, this class is set up in Python so that new
// properties are prevented, only existing properties can be set.
struct TableOptions {
    uint_t key_size;
    uint_t value_size;
    uint_t value_compressibility;
    bool random_value;
    uint_t range;

    TableOptions();
    TableOptions(const TableOptions &other);
    ~TableOptions() = default;

    void describe(std::ostream &os) const {
	os << "key_size " << key_size;
	os << ", value_size " << value_size;
	os << ", random_value " << random_value;
	os << ", range " << range;
    }

    std::string help() const { return _options.help(); }
    std::string help_description(const std::string& option_name) const {
	return _options.help_description(option_name); }
    std::string help_type(const std::string& option_name) const {
	return _options.help_type(option_name); }

private:
    OptionsList _options;
};

struct Table {
    TableOptions options;
    std::string _uri;
    TableInternal *_internal;

    /* XXX select table from range */

    Table();
    Table(const std::string& tablename);
    Table(const Table &other);
    ~Table();

    void describe(std::ostream &os) const;

#ifndef SWIG
    Table& operator=(const Table &other);
#endif
};

struct ParetoOptions {
    int param;
    double range_low;
    double range_high;
    ParetoOptions(int param = 0);
    ParetoOptions(const ParetoOptions &other);
    ~ParetoOptions() = default;

    void describe(std::ostream &os) const {
	os << "Pareto: parameter " << param;
	if (range_low != 0.0 || range_high != 1.0) {
	    os << "range [" << range_low << "-" << range_high << "]";
	}
    }

    std::string help() const { return _options.help(); }
    std::string help_description(const std::string& option_name) const {
	return _options.help_description(option_name); }
    std::string help_type(const std::string& option_name) const {
	return _options.help_type(option_name); }

    static ParetoOptions DEFAULT;
private:
    OptionsList _options;
};

struct Key {
    typedef enum {
	KEYGEN_AUTO, KEYGEN_APPEND, KEYGEN_PARETO, KEYGEN_UNIFORM } KeyType;
    KeyType _keytype;
    int _size;
    ParetoOptions _pareto;

    /* XXX specify more about key distribution */
    Key() : _keytype(KEYGEN_AUTO), _size(0), _pareto(ParetoOptions::DEFAULT) {}
    Key(KeyType keytype, int size=0,
      const ParetoOptions &pareto=ParetoOptions::DEFAULT) :
	_keytype(keytype), _size(size), _pareto(pareto) {}
    Key(const Key &other) : _keytype(other._keytype), _size(other._size),
	_pareto(other._pareto) {}
    ~Key() {}

    void describe(std::ostream &os) const {
	os << "Key: type " << _keytype << ", size " << _size;
        if (_pareto.param != ParetoOptions::DEFAULT.param) {
            os << ", ";
            _pareto.describe(os);
        }
    }
};

struct Value {
    int _size;

    /* XXX specify how value is calculated */
    Value() : _size(0) {}
    Value(int size) : _size(size) {}
    Value(const Value &other) : _size(other._size) {}
    ~Value() {}

    void describe(std::ostream &os) const { os << "Value: size " << _size; }
};

struct Operation {
    enum OpType {
	OP_CHECKPOINT, OP_INSERT, OP_LOG_FLUSH, OP_NONE, OP_NOOP,
	OP_REMOVE, OP_SEARCH, OP_SLEEP, OP_UPDATE };
    OpType _optype;
    OperationInternal *_internal;

    Table _table;
    Key _key;
    Value _value;
    std::string _config;
    Transaction *transaction;
    std::vector<Operation> *_group;
    int _repeatgroup;
    double _timed;
    // Indicates whether a table is selected randomly to be worked on.
    bool _random_table;
    // Maintain the random table being used by each thread running the operation.
    std::vector<std::string> _tables;

    Operation();
    Operation(OpType optype, Table table, Key key, Value value);
    Operation(OpType optype, Table table, Key key);
    Operation(OpType optype, Table table);
    // Operation working on random tables.
    Operation(OpType optype, Key key, Value value);
    // Constructor with string applies to NOOP, SLEEP, CHECKPOINT
    Operation(OpType optype, const std::string& config);
    Operation(const Operation &other);
    ~Operation();

    // Check if adding (via Python '+') another operation to this one is
    // as easy as appending the new operation to the _group.
    bool combinable() const;
    void describe(std::ostream &os) const;
#ifndef SWIG
    Operation& operator=(const Operation &other);
    void init_internal(OperationInternal *other);
    void create_all();
    void get_static_counts(Stats &stats, int multiplier);
    bool is_table_op() const;
    void kv_compute_max(bool iskey, bool has_random);
    void kv_gen(ThreadRunner *runner, bool iskey, uint64_t compressibility,
       uint64_t n, char *result) const;
    void kv_size_buffer(bool iskey, size_t &size) const;
    void size_check() const;
    void synchronized_check() const;
#endif
};

// To prevent silent errors, this class is set up in Python so that new
// properties are prevented, only existing properties can be set.
struct ThreadOptions {
    std::string name;
    std::string session_config;
    double throttle;
    double throttle_burst;
    bool synchronized;

    ThreadOptions();
    ThreadOptions(const ThreadOptions &other);
    ~ThreadOptions() = default;

    void describe(std::ostream &os) const {
	os << "throttle " << throttle;
	os << ", throttle_burst " << throttle_burst;
	os << ", synchronized " << synchronized;
	os << ", session_config " << session_config;
    }

    std::string help() const { return _options.help(); }
    std::string help_description(const std::string& option_name) const {
	return _options.help_description(option_name); }
    std::string help_type(const std::string& option_name) const {
	return _options.help_type(option_name); }

private:
    OptionsList _options;
};

/*
 * This is a list of threads, which may be used in the Workload constructor.
 * It participates with ThreadList defined on the SWIG/Python side and
 * some Python operators added to Thread to allow Threads to be easily
 * composed using '+' and multiplied (by integer counts) using '*'.
 * Users of the workgen API in Python don't ever need to use
 * ThreadListWrapper or ThreadList.
 */
struct ThreadListWrapper {
    std::vector<Thread> _threads;

    ThreadListWrapper() : _threads() {}
    ThreadListWrapper(const ThreadListWrapper &other) :
	_threads(other._threads) {}
    ThreadListWrapper(const std::vector<Thread> &threads) : _threads(threads) {}
    void extend(const ThreadListWrapper &);
    void append(const Thread &);
    void multiply(const int);
};

struct Thread {
    ThreadOptions options;
    Operation _op;

    Thread();
    Thread(const Operation &op);
    Thread(const Thread &other);
    ~Thread() = default;

    void describe(std::ostream &os) const;
};

struct Transaction {
    bool _rollback;
    bool use_commit_timestamp;
    bool use_prepare_timestamp;
    std::string _begin_config;
    std::string _commit_config;
    double read_timestamp_lag;

    Transaction() : _rollback(false), use_commit_timestamp(false),
      use_prepare_timestamp(false), _begin_config(""), _commit_config(), read_timestamp_lag(0.0)
    {}

    Transaction(const Transaction &other) : _rollback(other._rollback),
      use_commit_timestamp(other.use_commit_timestamp),
      use_prepare_timestamp(other.use_prepare_timestamp),
      _begin_config(other._begin_config), _commit_config(other._commit_config),
      read_timestamp_lag(other.read_timestamp_lag)
    {}

    void describe(std::ostream &os) const {
	os << "Transaction: ";
	if (_rollback)
	    os << "(rollback) ";
	if (use_commit_timestamp)
	    os << "(use_commit_timestamp) ";
	if (use_prepare_timestamp)
	    os << "(use_prepare_timestamp) ";
	os << "begin_config: " << _begin_config;
	if (!_commit_config.empty())
	    os << ", commit_config: " << _commit_config;
	if (read_timestamp_lag != 0.0)
	    os << ", read_timestamp_lag: " << read_timestamp_lag;
    }
};

// To prevent silent errors, this class is set up in Python so that new
// properties are prevented, only existing properties can be set.
struct WorkloadOptions {
    int max_latency;
    bool report_enabled;
    std::string report_file;
    int report_interval;
    int run_time;
    int sample_interval_ms;
    int sample_rate;
    int max_idle_table_cycle;
    std::string sample_file;
    int warmup;
    double oldest_timestamp_lag;
    double stable_timestamp_lag;
    double timestamp_advance;
    bool max_idle_table_cycle_fatal;
    /* Dynamic create/drop options */
    int create_count;
    int create_interval;
    std::string create_prefix;
    int create_target;
    int create_trigger;
    int drop_count;
    int drop_interval;
    int drop_target;
    int drop_trigger;
    bool random_table_values;
    bool mirror_tables;
    std::string mirror_suffix;

    WorkloadOptions();
    WorkloadOptions(const WorkloadOptions &other);
    ~WorkloadOptions() = default;

    void describe(std::ostream &os) const {
	os << "run_time " << run_time;
	os << ", report_interval " << report_interval;
    }

    std::string help() const { return _options.help(); }
    std::string help_description(const std::string& option_name) const {
	return _options.help_description(option_name); }
    std::string help_type(const std::string& option_name) const {
	return _options.help_type(option_name); }

private:
    OptionsList _options;
};

struct Workload {
    WorkloadOptions options;
    Stats stats;
    Context *_context;
    std::vector<Thread> _threads;

    Workload(Context *context, const ThreadListWrapper &threadlist);
    Workload(Context *context, const Thread &thread);
    Workload(const Workload &other);
    ~Workload() = default;

#ifndef SWIG
    Workload& operator=(const Workload &other);
#endif

    void describe(std::ostream &os) const {
	os << "Workload: ";
	_context->describe(os);
	os << ", ";
	options.describe(os);
	os << ", [" << std::endl;
	for (std::vector<Thread>::const_iterator i = _threads.begin(); i != _threads.end(); i++) {
	    os << "  "; i->describe(os); os << std::endl;
	}
	os << "]";
    }
    int run(WT_CONNECTION *conn);
};

}
