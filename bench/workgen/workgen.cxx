/*-
 * Public Domain 2014-2017 MongoDB, Inc.
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

#define __STDC_LIMIT_MACROS   // needed to get UINT64_MAX in C++
#include <iomanip>
#include <iostream>
#include <fstream>
#include <sstream>
#include "wiredtiger.h"
#include "workgen.h"
#include "workgen_int.h"
#include "workgen_time.h"
extern "C" {
// Include some specific WT files, as some files included by wt_internal.h
// have some C-ism's that don't work in C++.
#include <pthread.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <math.h>
#include "error.h"
#include "misc.h"
}

#define LATENCY_US_BUCKETS 1000
#define LATENCY_MS_BUCKETS 1000
#define LATENCY_SEC_BUCKETS 100

#define THROTTLE_PER_SEC  20     // times per sec we will throttle

#define MIN(a, b)		((a) < (b) ? (a) : (b))
#define MAX(a, b)		((a) < (b) ? (b) : (a))
#define TIMESPEC_DOUBLE(ts)	((double)(ts).tv_sec + ts.tv_nsec * 0.000000001)
#define PCT(n, total)		((total) == 0 ? 0 : ((n) * 100) / (total))
#define OPS_PER_SEC(ops, ts)	(int) ((ts) == 0 ? 0.0 : \
    (ops) / TIMESPEC_DOUBLE(ts))

// Get the value of a STL container, even if it is not present
#define CONTAINER_VALUE(container, idx, dfault)   \
    (((container).count(idx) > 0) ? (container)[idx] : (dfault))

#define CROSS_USAGE(a, b)                                               \
    (((a & USAGE_READ) != 0 && (b & USAGE_WRITE) != 0) ||               \
     ((a & USAGE_WRITE) != 0 && (b & USAGE_READ) != 0))

#define ASSERT(cond)                                                    \
    do {                                                                \
        if (!(cond)) {                                                  \
            fprintf(stderr, "%s:%d: ASSERT failed: %s\n",               \
                    __FILE__, __LINE__, #cond);                         \
            abort();                                                    \
        }                                                               \
    } while(0)

#define THROW_ERRNO(e, args)                                            \
    do {                                                                \
        std::stringstream __sstm;                                       \
        __sstm << args;                                                 \
        WorkgenException __wge(e, __sstm.str().c_str());                \
        throw(__wge);                                                   \
    } while(0)

#define THROW(args)   THROW_ERRNO(0, args)

#define VERBOSE(runner, args)                                           \
    do {                                                                \
        if ((runner)._context->_verbose)                                \
            std::cout << args << std::endl;                             \
    } while(0)

#define OP_HAS_VALUE(op)                                                \
    ((op)->_optype == Operation::OP_INSERT ||                           \
      (op)->_optype == Operation::OP_UPDATE)

namespace workgen {

// The number of contexts.  Normally there is one context created, but it will
// be possible to use several eventually.  More than one is not yet
// implemented, but we must at least guard against the caller creating more
// than one.
static uint32_t context_count = 0;

static void *thread_runner_main(void *arg) {
    ThreadRunner *runner = (ThreadRunner *)arg;
    try {
        runner->_errno = runner->run();
    } catch (WorkgenException &wge) {
        runner->_exception = wge;
    }
    return (NULL);
}

static void *monitor_main(void *arg) {
    Monitor *monitor = (Monitor *)arg;
    try {
        monitor->_errno = monitor->run();
    } catch (WorkgenException &wge) {
        monitor->_exception = wge;
    }
    return (NULL);
}

// Exponentiate (like the pow function), except that it returns an exact
// integral 64 bit value, and if it overflows, returns the maximum possible
// value for the return type.
static uint64_t power64(int base, int exp) {
    uint64_t last, result;

    result = 1;
    for (int i = 0; i < exp; i++) {
        last = result;
        result *= base;
        if (result < last)
            return UINT64_MAX;
    }
    return result;
}

OptionsList::OptionsList() : _option_map() {}
OptionsList::OptionsList(const OptionsList &other) :
    _option_map(other._option_map) {}

void OptionsList::add_option(const char *name, const std::string typestr,
  const char *desc) {
    TypeDescPair pair(typestr, desc);
    _option_map[name] = pair;
}

void OptionsList::add_int(const char *name, int default_value,
  const char *desc) {
    std::stringstream sstm;
    sstm << "int, default=" << default_value;
    add_option(name, sstm.str(), desc);
}

void OptionsList::add_bool(const char *name, bool default_value,
  const char *desc) {
    std::stringstream sstm;
    sstm << "boolean, default=" << (default_value ? "true" : "false");
    add_option(name, sstm.str(), desc);
}

void OptionsList::add_double(const char *name, double default_value,
  const char *desc) {
    std::stringstream sstm;
    sstm << "double, default=" << default_value;
    add_option(name, sstm.str(), desc);
}

void OptionsList::add_string(const char *name,
  const std::string &default_value, const char *desc) {
    std::stringstream sstm;
    sstm << "string, default=\"" << default_value << "\"";
    add_option(name, sstm.str(), desc);
}

static void
pretty_print(const char *p, const char *indent, std::stringstream &sstm)
{
    const char *t;

    for (;; p = t + 1) {
        if (strlen(p) <= 70)
            break;
        for (t = p + 70; t > p && *t != ' '; --t)
            ;
        if (t == p)            /* No spaces? */
            break;
        if (indent != NULL)
            sstm << indent;
        std::string line(p, (size_t)(t - p));
        sstm << line << std::endl;
    }
    if (*p != '\0') {
        if (indent != NULL)
            sstm << indent;
        sstm << p << std::endl;
    }
}

std::string OptionsList::help() const {
    std::stringstream sstm;
    for (std::map<std::string, TypeDescPair>::const_iterator i =
         _option_map.begin(); i != _option_map.end(); i++) {
        sstm << i->first << " (" << i->second.first << ")" << std::endl;
        pretty_print(i->second.second.c_str(), "\t", sstm);
    }
    return sstm.str();
}

std::string OptionsList::help_description(const char *option_name) const {
    const std::string key(option_name);
    if (_option_map.count(key) == 0)
        return (std::string(""));
    else
        return (_option_map.find(key)->second.second);
}

std::string OptionsList::help_type(const char *option_name) const {
    const std::string key(option_name);
    if (_option_map.count(key) == 0)
        return std::string("");
    else
        return (_option_map.find(key)->second.first);
}

Context::Context() : _verbose(false), _internal(new ContextInternal()) {}
Context::~Context() { delete _internal; }
Context& Context::operator=(const Context &other) {
    _verbose = other._verbose;
    *_internal = *other._internal;
    return (*this);
}

ContextInternal::ContextInternal() : _tint(), _table_names(),
    _recno(NULL), _recno_alloced(0), _tint_last(0), _context_count(0) {
    uint32_t count;
    if ((count = workgen_atomic_add32(&context_count, 1)) != 1)
        THROW("multiple Contexts not supported");
    _context_count = count;
}

ContextInternal::~ContextInternal() {
    if (_recno != NULL)
        delete _recno;
}

int ContextInternal::create_all() {
    if (_recno_alloced != _tint_last) {
        // The array references are 1-based, we'll waste one entry.
        uint64_t *new_recno = new uint64_t[_tint_last + 1];
        memcpy(new_recno, _recno, sizeof(uint64_t) * _recno_alloced);
        memset(&new_recno[_recno_alloced], 0,
          sizeof(uint64_t) * (_tint_last - _recno_alloced + 1));
        delete _recno;
        _recno = new_recno;
        _recno_alloced = _tint_last;
    }
    return (0);
}

Monitor::Monitor(WorkloadRunner &wrunner) :
    _errno(0), _exception(), _wrunner(wrunner), _stop(false), _handle(),
    _out(NULL), _json(NULL) {}
Monitor::~Monitor() {}

int Monitor::run() {
    struct timespec t;
    struct tm *tm, _tm;
    char time_buf[64], version[100];
    Stats prev_totals;
    WorkloadOptions *options = &_wrunner._workload->options;
    uint64_t latency_max = (uint64_t)options->max_latency;
    bool first;

    (*_out) << "#time,"
            << "totalsec,"
            << "read ops per second,"
            << "insert ops per second,"
            << "update ops per second,"
            << "checkpoints,"
            << "read average latency(uS),"
            << "read minimum latency(uS),"
            << "read maximum latency(uS),"
            << "insert average latency(uS),"
            << "insert min latency(uS),"
            << "insert maximum latency(uS),"
            << "update average latency(uS),"
            << "update min latency(uS),"
            << "update maximum latency(uS)"
            << std::endl;

    first = true;
    workgen_version(version, sizeof(version));
    Stats prev_interval;
    while (!_stop) {
        for (int i = 0; i < options->sample_interval && !_stop; i++)
            sleep(1);
        if (_stop)
            break;

        workgen_epoch(&t);
        tm = localtime_r(&t.tv_sec, &_tm);
        (void)strftime(time_buf, sizeof(time_buf), "%b %d %H:%M:%S", tm);

        Stats new_totals(true);
        for (std::vector<ThreadRunner>::iterator tr =
          _wrunner._trunners.begin(); tr != _wrunner._trunners.end(); tr++)
            new_totals.add(tr->_stats, true);
        Stats interval(new_totals);
        interval.subtract(prev_totals);
        interval.smooth(prev_interval);

        int interval_secs = options->sample_interval;
        uint64_t cur_reads = interval.read.ops / interval_secs;
        uint64_t cur_inserts = interval.insert.ops / interval_secs;
        uint64_t cur_updates = interval.update.ops / interval_secs;

        uint64_t totalsec = ts_sec(t - _wrunner._start);
        (*_out) << time_buf
                << "," << totalsec
                << "," << cur_reads
                << "," << cur_inserts
                << "," << cur_updates
                << "," << 'N'   // checkpoint in progress
                << "," << interval.read.average_latency()
                << "," << interval.read.min_latency
                << "," << interval.read.max_latency
                << "," << interval.insert.average_latency()
                << "," << interval.insert.min_latency
                << "," << interval.insert.max_latency
                << "," << interval.update.average_latency()
                << "," << interval.update.min_latency
                << "," << interval.update.max_latency
                << std::endl;

        if (_json != NULL) {
#define	WORKGEN_TIMESTAMP_JSON		"%Y-%m-%dT%H:%M:%S.000Z"
            (void)strftime(time_buf, sizeof(time_buf),
              WORKGEN_TIMESTAMP_JSON, tm);

#define TRACK_JSON(name, t)                                        \
            "\"" << (name) << "\":{"                               \
            << "\"ops per sec\":" << ((t).ops / interval_secs)     \
            << ",\"average latency\":" << (t).average_latency()    \
            << ",\"min latency\":" << (t).min_latency              \
            << ",\"max latency\":" << (t).max_latency              \
            << "}"

            (*_json) << "{";
            if (first) {
                (*_json) << "\"version\":\"" << version << "\",";
                first = false;
            }
            (*_json) << "\"localTime\":\"" << time_buf
                     << "\",\"workgen\":{"
                     << TRACK_JSON("read", interval.read) << ","
                     << TRACK_JSON("insert", interval.insert) << ","
                     << TRACK_JSON("update", interval.update)
                     << "}}" << std::endl;
        }

        uint64_t read_max = interval.read.max_latency;
        uint64_t insert_max = interval.read.max_latency;
        uint64_t update_max = interval.read.max_latency;

        if (latency_max != 0 &&
          (read_max > latency_max || insert_max > latency_max ||
          update_max > latency_max)) {
            std::cerr << "WARNING: max latency exceeded:"
                      << " threshold " << latency_max
                      << " read max " << read_max
                      << " insert max " << insert_max
                      << " update max " << update_max << std::endl;
        }

        prev_interval.assign(interval);
        prev_totals.assign(new_totals);
    }
    return (0);
}

ThreadRunner::ThreadRunner() :
    _errno(0), _exception(), _thread(NULL), _context(NULL), _icontext(NULL),
    _workload(NULL), _wrunner(NULL), _rand_state(NULL),
    _throttle(NULL), _throttle_ops(0), _throttle_limit(0),
    _in_transaction(false), _number(0), _stats(false), _table_usage(),
    _cursors(NULL), _stop(false), _session(NULL), _keybuf(NULL),
    _valuebuf(NULL), _repeat(false) {
}

ThreadRunner::~ThreadRunner() {
    free_all();
}

int ThreadRunner::create_all(WT_CONNECTION *conn) {
    size_t keysize, valuesize;

    WT_RET(close_all());
    ASSERT(_session == NULL);
    WT_RET(conn->open_session(conn, NULL, NULL, &_session));
    _table_usage.clear();
    _stats.track_latency(_workload->options.sample_interval > 0);
    WT_RET(workgen_random_alloc(_session, &_rand_state));
    _throttle_ops = 0;
    _throttle_limit = 0;
    _in_transaction = 0;
    keysize = 1;
    valuesize = 1;
    op_create_all(&_thread->_op, keysize, valuesize);
    _keybuf = new char[keysize];
    _valuebuf = new char[valuesize];
    _keybuf[keysize - 1] = '\0';
    _valuebuf[valuesize - 1] = '\0';
    return (0);
}

int ThreadRunner::open_all() {
    typedef WT_CURSOR *WT_CURSOR_PTR;
    if (_cursors != NULL)
        delete _cursors;
    _cursors = new WT_CURSOR_PTR[_icontext->_tint_last + 1];
    memset(_cursors, 0, sizeof (WT_CURSOR *) * (_icontext->_tint_last + 1));
    for (std::map<uint32_t, uint32_t>::iterator i = _table_usage.begin();
         i != _table_usage.end(); i++) {
        uint32_t tindex = i->first;
        const char *uri = _icontext->_table_names[tindex].c_str();
        WT_RET(_session->open_cursor(_session, uri, NULL, NULL,
          &_cursors[tindex]));
    }
    return (0);
}

int ThreadRunner::close_all() {
    if (_throttle != NULL) {
        delete _throttle;
        _throttle = NULL;
    }
    if (_session != NULL) {
        WT_RET(_session->close(_session, NULL));
        _session = NULL;
    }
    free_all();
    return (0);
}

void ThreadRunner::free_all() {
    if (_rand_state != NULL) {
        workgen_random_free(_rand_state);
        _rand_state = NULL;
    }
    if (_cursors != NULL) {
        delete _cursors;
        _cursors = NULL;
    }
    if (_keybuf != NULL) {
        delete _keybuf;
        _keybuf = NULL;
    }
    if (_valuebuf != NULL) {
        delete _valuebuf;
        _valuebuf = NULL;
    }
}

int ThreadRunner::cross_check(std::vector<ThreadRunner> &runners) {
    std::map<uint32_t, uint32_t> usage;

    // Determine which tables have cross usage
    for (std::vector<ThreadRunner>::iterator r = runners.begin();
      r != runners.end(); r++) {
        for (std::map<uint32_t, uint32_t>::iterator i = r->_table_usage.begin();
          i != r->_table_usage.end(); i++) {
            uint32_t tindex = i->first;
            uint32_t thisusage = i->second;
            uint32_t curusage = CONTAINER_VALUE(usage, tindex, 0);
            if (CROSS_USAGE(curusage, thisusage))
                curusage |= USAGE_MIXED;
            usage[tindex] = curusage;
        }
    }
    for (std::map<uint32_t, uint32_t>::iterator i = usage.begin();
         i != usage.end(); i++) {
        if ((i->second & USAGE_MIXED) != 0) {
            for (std::vector<ThreadRunner>::iterator r = runners.begin();
                 r != runners.end(); r++) {
                r->_table_usage[i->first] |= USAGE_MIXED;
            }
        }
    }
    return (0);
}

int ThreadRunner::run() {
    WT_DECL_RET;
    ThreadOptions *options = &_thread->options;
    std::string name = options->name;

    VERBOSE(*this, "thread " << name << " running");
    if (options->throttle != 0) {
        _throttle = new Throttle(*this, options->throttle,
          options->throttle_burst);
    }
    for (int cnt = 0; !_stop && (_repeat || cnt < 1) && ret == 0; cnt++)
        WT_ERR(op_run(&_thread->_op));

err:
#ifdef _DEBUG
    {
        std::string messages = this->get_debug();
        if (!messages.empty())
            std::cerr << "DEBUG (thread " << name << "): "
                      << messages << std::endl;
    }
#endif
    if (ret != 0)
        std::cerr << "thread " << name << " failed err=" << ret << std::endl;
    VERBOSE(*this, "thread " << name << "finished");
    return (ret);
}

void ThreadRunner::get_static_counts(Stats &stats) {
    _thread->_op.get_static_counts(stats, 1);
}

void ThreadRunner::op_create_all(Operation *op, size_t &keysize,
  size_t &valuesize) {
    tint_t tint;

    op->size_check();
    if (op->_optype != Operation::OP_NONE) {
        op->kv_compute_max(true);
        if (OP_HAS_VALUE(op))
            op->kv_compute_max(false);
        op->kv_size_buffer(true, keysize);
        op->kv_size_buffer(false, valuesize);

        // Note: to support multiple contexts we'd need a generation
        // count whenever we execute.
        if (op->_table._internal->_context_count != 0 &&
          op->_table._internal->_context_count != _icontext->_context_count)
            THROW("multiple Contexts not supported");
        if ((tint = op->_table._internal->_tint) == 0) {
            std::string uri = op->_table._uri;

            // We are single threaded in this function, so do not have
            // to worry about locking.
            if (_icontext->_tint.count(uri) == 0) {
                // TODO: don't use atomic add, it's overkill.
                tint = workgen_atomic_add32(&_icontext->_tint_last, 1);
                _icontext->_tint[uri] = tint;
                _icontext->_table_names[tint] = uri;
            } else
                tint = _icontext->_tint[uri];
            op->_table._internal->_tint = tint;
        }
        uint32_t usage_flags = CONTAINER_VALUE(_table_usage,
          op->_table._internal->_tint, 0);
        if (op->_optype == Operation::OP_SEARCH)
            usage_flags |= ThreadRunner::USAGE_READ;
        else
            usage_flags |= ThreadRunner::USAGE_WRITE;
        _table_usage[op->_table._internal->_tint] = usage_flags;
    }
    if (op->_group != NULL)
        for (std::vector<Operation>::iterator i = op->_group->begin();
            i != op->_group->end(); i++)
            op_create_all(&*i, keysize, valuesize);
}

uint64_t ThreadRunner::op_get_key_recno(Operation *op, tint_t tint) {
    uint64_t recno_count;
    uint32_t rand;

    recno_count = _icontext->_recno[tint];
    if (recno_count == 0)
        // The file has no entries, returning 0 forces a WT_NOTFOUND return.
        return (0);
    rand = workgen_random(_rand_state);
    return (rand % recno_count + 1);  // recnos are one-based.
}

int ThreadRunner::op_run(Operation *op) {
    Track *track;
    tint_t tint = op->_table._internal->_tint;
    WT_CURSOR *cursor = _cursors[tint];
    WT_DECL_RET;
    uint64_t recno;
    bool measure_latency;

    recno = 0;
    track = NULL;
    if (_throttle != NULL) {
        if (_throttle_ops >= _throttle_limit && !_in_transaction) {
            WT_ERR(_throttle->throttle(_throttle_ops,
              &_throttle_limit));
            _throttle_ops = 0;
        }
        if (op->_optype != Operation::OP_NONE)
            ++_throttle_ops;
    }

    // A potential race: thread1 is inserting, and increments
    // Context->_recno[] for fileX.wt. thread2 is doing one of
    // remove/search/update and grabs the new value of Context->_recno[]
    // for fileX.wt.  thread2 randomly chooses the highest recno (which
    // has not yet been inserted by thread1), and when it accesses
    // the record will get WT_NOTFOUND.  It should be somewhat rare
    // (and most likely when the threads are first beginning).  Any
    // WT_NOTFOUND returns are allowed and get their own statistic bumped.
    switch (op->_optype) {
    case Operation::OP_INSERT:
        track = &_stats.insert;
        recno = workgen_atomic_add64(&_icontext->_recno[tint], 1);
        break;
    case Operation::OP_REMOVE:
        track = &_stats.remove;
        recno = op_get_key_recno(op, tint);
        break;
    case Operation::OP_SEARCH:
        track = &_stats.read;
        recno = op_get_key_recno(op, tint);
        break;
    case Operation::OP_UPDATE:
        track = &_stats.update;
        recno = op_get_key_recno(op, tint);
        break;
    case Operation::OP_NONE:
        recno = 0;
        break;
    }

    measure_latency = track != NULL && track->ops != 0 &&
      track->track_latency() &&
      (track->ops % _workload->options.sample_rate == 0);

    timespec start;
    if (measure_latency)
        workgen_epoch(&start);

    if (op->_transaction != NULL) {
        if (_in_transaction)
            THROW("nested transactions not supported");
        _session->begin_transaction(_session,
          op->_transaction->_begin_config.c_str());
        _in_transaction = true;
    }
    if (op->_optype != Operation::OP_NONE) {
        op->kv_gen(true, recno, _keybuf);
        cursor->set_key(cursor, _keybuf);
        if (OP_HAS_VALUE(op)) {
            op->kv_gen(false, recno, _valuebuf);
            cursor->set_value(cursor, _valuebuf);
        }
        switch (op->_optype) {
        case Operation::OP_INSERT:
            WT_ERR(cursor->insert(cursor));
            break;
        case Operation::OP_REMOVE:
            WT_ERR_NOTFOUND_OK(cursor->remove(cursor));
            break;
        case Operation::OP_SEARCH:
            ret = cursor->search(cursor);
            break;
        case Operation::OP_UPDATE:
            WT_ERR_NOTFOUND_OK(cursor->update(cursor));
            break;
        default:
            ASSERT(false);
        }
        if (ret != 0) {
            track = &_stats.not_found;
            ret = 0;  // WT_NOTFOUND allowed.
        }
        cursor->reset(cursor);
    }
    if (measure_latency) {
        timespec stop;
        workgen_epoch(&stop);
        track->incr_with_latency(ts_us(stop - start));
    } else if (track != NULL)
        track->incr();

    if (op->_group != NULL)
        for (int count = 0; !_stop && count < op->_repeatgroup; count++)
            for (std::vector<Operation>::iterator i = op->_group->begin();
              i != op->_group->end(); i++)
                WT_ERR(op_run(&*i));
err:
    if (op->_transaction != NULL) {
        if (ret != 0 || op->_transaction->_rollback)
            WT_TRET(_session->rollback_transaction(_session, NULL));
        else
            ret = _session->commit_transaction(_session,
              op->_transaction->_commit_config.c_str());
        _in_transaction = false;
    }
    return (ret);
}

#ifdef _DEBUG
std::string ThreadRunner::get_debug() {
    return (_debug_messages.str());
}
#endif

Throttle::Throttle(ThreadRunner &runner, double throttle,
    double throttle_burst) : _runner(runner), _throttle(throttle),
    _burst(throttle_burst), _next_div(), _ops_delta(0), _ops_prev(0),
    _ops_per_div(0), _ms_per_div(0), _started(false) {
    ts_clear(_next_div);
    _ms_per_div = ceill(1000.0 / THROTTLE_PER_SEC);
    _ops_per_div = ceill(_throttle / THROTTLE_PER_SEC);
}

Throttle::~Throttle() {}

// Given a random 32-bit value, return a float value equally distributed
// between -1.0 and 1.0.
static float rand_signed(uint32_t r) {
    int sign = ((r & 0x1) == 0 ? 1 : -1);
    return (((float)r * sign) / UINT32_MAX);
}

// Each time throttle is called, we sleep and return a number of operations to
// perform next.  To implement this we keep a time calculation in _next_div set
// initially to the current time + 1/THROTTLE_PER_SEC.  Each call to throttle
// advances _next_div by 1/THROTTLE_PER_SEC, and if _next_div is in the future,
// we sleep for the difference between the _next_div and the current_time.  We
// always return (Thread.options.throttle / THROTTLE_PER_SEC) as the number of
// operations.
//
// The only variation is that the amount of individual sleeps is modified by a
// random amount (which varies more widely as Thread.options.throttle_burst is
// greater).  This has the effect of randomizing how much clumping happens, and
// ensures that multiple threads aren't executing in lock step.
//
int Throttle::throttle(uint64_t op_count, uint64_t *op_limit) {
    uint64_t ops;
    int64_t sleep_ms;
    timespec now;

    workgen_epoch(&now);
    DEBUG_CAPTURE(_runner, "throttle: ops=" << op_count);
    if (!_started) {
        _next_div = ts_add_ms(now, _ms_per_div);
        _started = true;
    } else {
        _ops_delta += (op_count - _ops_prev);
        if (now < _next_div) {
            sleep_ms = ts_ms(_next_div - now);
            sleep_ms += (_ms_per_div * _burst *
              rand_signed(workgen_random(_runner._rand_state)));
            if (sleep_ms > 0) {
                DEBUG_CAPTURE(_runner, ", sleep=" << sleep_ms);
                usleep((useconds_t)ms_to_us(sleep_ms));
            }
        }
        _next_div = ts_add_ms(_next_div, _ms_per_div);
    }
    ops = _ops_per_div;
    if (_ops_delta < (int64_t)ops) {
        ops -= _ops_delta;
        _ops_delta = 0;
    } else {
        _ops_delta -= ops;
        ops = 0;
    }
    *op_limit = ops;
    _ops_prev = ops;
    DEBUG_CAPTURE(_runner, ", return=" << ops << std::endl);
    return (0);
}

ThreadOptions::ThreadOptions() : name(), throttle(0.0), throttle_burst(1.0),
    _options() {
    _options.add_string("name", name, "name of the thread");
    _options.add_double("throttle", throttle,
      "Limit to this number of operations per second");
    _options.add_double("throttle_burst", throttle_burst,
      "Changes characteristic of throttling from smooth (0.0) "
      "to having large bursts with lulls (10.0 or larger)");
}
ThreadOptions::ThreadOptions(const ThreadOptions &other) :
    name(other.name), throttle(other.throttle),
    throttle_burst(other.throttle_burst), _options(other._options) {}
ThreadOptions::~ThreadOptions() {}

void
ThreadListWrapper::extend(const ThreadListWrapper &other) {
    for (std::vector<Thread>::const_iterator i = other._threads.begin();
         i != other._threads.end(); i++)
        _threads.push_back(*i);
}

void
ThreadListWrapper::append(const Thread &t) {
    _threads.push_back(t);
}

void
ThreadListWrapper::multiply(const int n) {
    if (n == 0) {
        _threads.clear();
    } else {
        std::vector<Thread> copy(_threads);
        for (int cnt = 1; cnt < n; cnt++)
            extend(copy);
    }
}

Thread::Thread() : options(), _op() {
}

Thread::Thread(const Operation &op) : options(), _op(op) {
}

Thread::Thread(const Thread &other) : options(other.options), _op(other._op) {
}

Thread::~Thread() {
}

void Thread::describe(std::ostream &os) const {
    os << "Thread: [" << std::endl;
    _op.describe(os); os << std::endl;
    os << "]";
}

Operation::Operation() :
    _optype(OP_NONE), _table(), _key(), _value(), _transaction(NULL),
    _group(NULL), _repeatgroup(0),
    _keysize(0), _valuesize(0), _keymax(0), _valuemax(0) {
}

Operation::Operation(OpType optype, Table table, Key key, Value value) :
    _optype(optype), _table(table), _key(key), _value(value),
    _transaction(NULL), _group(NULL), _repeatgroup(0),
    _keysize(0), _valuesize(0), _keymax(0), _valuemax(0) {
    size_check();
}

Operation::Operation(OpType optype, Table table, Key key) :
    _optype(optype), _table(table), _key(key), _value(), _transaction(NULL),
    _group(NULL), _repeatgroup(0),
    _keysize(0), _valuesize(0), _keymax(0), _valuemax(0) {
    size_check();
}

Operation::Operation(OpType optype, Table table) :
    _optype(optype), _table(table), _key(), _value(), _transaction(NULL),
    _group(NULL), _repeatgroup(0),
    _keysize(0), _valuesize(0), _keymax(0), _valuemax(0) {
    size_check();
}

Operation::Operation(const Operation &other) :
    _optype(other._optype), _table(other._table), _key(other._key),
    _value(other._value), _transaction(other._transaction),
    _group(other._group), _repeatgroup(other._repeatgroup),
    _keysize(other._keysize), _valuesize(other._valuesize),
    _keymax(other._keymax), _valuemax(other._valuemax) {
    // Creation and destruction of _group and _transaction is managed
    // by Python.
}

Operation::~Operation() {
    // Creation and destruction of _group, _transaction is managed by Python.
}

Operation& Operation::operator=(const Operation &other) {
    _optype = other._optype;
    _table = other._table;
    _key = other._key;
    _value = other._value;
    _transaction = other._transaction;
    _group = other._group;
    _repeatgroup = other._repeatgroup;
    _keysize = other._keysize;
    _valuesize = other._valuesize;
    _keymax = other._keymax;
    _valuemax = other._valuemax;
    return (*this);
}

void Operation::describe(std::ostream &os) const {
    os << "Operation: " << _optype;
    if (_optype != OP_NONE) {
        os << ", ";  _table.describe(os);
        os << ", "; _key.describe(os);
        os << ", "; _value.describe(os);
    }
    if (_transaction != NULL) {
        os << ", ["; _transaction->describe(os); os << "]";
    }
    if (_group != NULL) {
        os << ", group[" << _repeatgroup << "]: {";
        bool first = true;
        for (std::vector<Operation>::const_iterator i = _group->begin();
             i != _group->end(); i++) {
            if (!first)
                os << "}, {";
            i->describe(os);
            first = false;
        }
        os << "}";
    }
}

void Operation::get_static_counts(Stats &stats, int multiplier) {
    switch (_optype) {
    case OP_NONE:
        break;
    case OP_INSERT:
        stats.insert.ops += multiplier;
        break;
    case OP_REMOVE:
        stats.remove.ops += multiplier;
        break;
    case OP_SEARCH:
        stats.read.ops += multiplier;
        break;
    case OP_UPDATE:
        stats.update.ops += multiplier;
        break;
    default:
        ASSERT(false);
    }
    if (_group != NULL)
        for (std::vector<Operation>::iterator i = _group->begin();
          i != _group->end(); i++)
            i->get_static_counts(stats, multiplier * _repeatgroup);
}

void Operation::kv_compute_max(bool iskey) {
    uint64_t max;
    int size;

    size = iskey ? _key._size : _value._size;
    if (size == 0)
        size = iskey ? _table.options.key_size : _table.options.value_size;

    if (iskey && size < 2)
        THROW("Key.size too small for table '" << _table._uri << "'");
    if (!iskey && size < 1)
        THROW("Value.size too small for table '" << _table._uri << "'");

    if (size > 1)
        max = power64(10, (size - 1)) - 1;
    else
        max = 0;

    if (iskey) {
        _keysize = size;
        _keymax = max;
    } else {
        _valuesize = size;
        _valuemax = max;
    }
}

void Operation::kv_size_buffer(bool iskey, size_t &maxsize) const {
    if (iskey) {
        if ((size_t)_keysize > maxsize)
            maxsize = _keysize;
    } else {
        if ((size_t)_valuesize > maxsize)
            maxsize = _valuesize;
    }
}

void Operation::kv_gen(bool iskey, uint64_t n, char *result) const {
    uint64_t max;
    int size;

    size = iskey ? _keysize : _valuesize;
    max = iskey ? _keymax : _valuemax;
    if (n > max)
        THROW((iskey ? "Key" : "Value") << " (" << n
          << ") too large for size (" << size << ")");
    workgen_u64_to_string_zf(n, result, size);
}

void Operation::size_check() const {
    if (_optype != OP_NONE && _key._size == 0 && _table.options.key_size == 0)
        THROW("operation requires a key size");
    if (OP_HAS_VALUE(this) && _value._size == 0 &&
      _table.options.value_size == 0)
        THROW("operation requires a value size");
}

Track::Track(bool latency_tracking) : ops(0), latency_ops(0), latency(0),
    min_latency(0), max_latency(0), us(NULL), ms(NULL), sec(NULL) {
    track_latency(latency_tracking);
}

Track::Track(const Track &other) : ops(other.ops),
    latency_ops(other.latency_ops), latency(other.latency),
    min_latency(other.min_latency), max_latency(other.max_latency),
    us(NULL), ms(NULL), sec(NULL) {
    if (other.us != NULL) {
        us = new uint32_t[LATENCY_US_BUCKETS];
        ms = new uint32_t[LATENCY_MS_BUCKETS];
        sec = new uint32_t[LATENCY_SEC_BUCKETS];
        memcpy(us, other.us, sizeof(uint32_t) * LATENCY_US_BUCKETS);
        memcpy(ms, other.ms, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
        memcpy(sec, other.sec, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
    }
}

Track::~Track() {
    if (us != NULL) {
        delete us;
        delete ms;
        delete sec;
    }
}

void Track::add(Track &other, bool reset) {
    ops += other.ops;
    latency_ops += other.latency_ops;
    latency += other.latency;

    min_latency = MIN(min_latency, other.min_latency);
    if (reset)
        other.min_latency = 0;
    max_latency = MAX(max_latency, other.max_latency);
    if (reset)
        other.max_latency = 0;

    if (us != NULL && other.us != NULL) {
        for (int i = 0; i < LATENCY_US_BUCKETS; i++)
            us[i] += other.us[i];
        for (int i = 0; i < LATENCY_MS_BUCKETS; i++)
            ms[i] += other.ms[i];
        for (int i = 0; i < LATENCY_SEC_BUCKETS; i++)
            sec[i] += other.sec[i];
    }
}

void Track::assign(const Track &other) {
    ops = other.ops;
    latency_ops = other.latency_ops;
    latency = other.latency;
    min_latency = other.min_latency;
    max_latency = other.max_latency;

    if (other.us == NULL && us != NULL) {
        delete us;
        delete ms;
        delete sec;
        us = NULL;
        ms = NULL;
        sec = NULL;
    }
    else if (other.us != NULL && us == NULL) {
        us = new uint32_t[LATENCY_US_BUCKETS];
        ms = new uint32_t[LATENCY_MS_BUCKETS];
        sec = new uint32_t[LATENCY_SEC_BUCKETS];
    }
    if (us != NULL) {
        memcpy(us, other.us, sizeof(uint32_t) * LATENCY_US_BUCKETS);
        memcpy(ms, other.ms, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
        memcpy(sec, other.sec, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
    }
}

uint64_t Track::average_latency() const {
    if (latency_ops == 0)
        return (0);
    else
        return (latency / latency_ops);
}

void Track::clear() {
    ops = 0;
    latency_ops = 0;
    latency = 0;
    min_latency = 0;
    max_latency = 0;
    if (us != NULL) {
        memset(us, 0, sizeof(uint32_t) * LATENCY_US_BUCKETS);
        memset(ms, 0, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
        memset(sec, 0, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
    }
}

void Track::incr() {
    ops++;
}

void Track::incr_with_latency(uint64_t usecs) {
    ASSERT(us != NULL);

    ops++;
    latency_ops++;
    latency += usecs;
    if (usecs > max_latency)
        max_latency = (uint32_t)usecs;
    if (usecs < min_latency)
        min_latency = (uint32_t)usecs;

    // Update a latency bucket.
    // First buckets: usecs from 100us to 1000us at 100us each.
    if (usecs < LATENCY_US_BUCKETS)
        us[usecs]++;

    // Second buckets: milliseconds from 1ms to 1000ms, at 1ms each.
    else if (usecs < ms_to_us(LATENCY_MS_BUCKETS))
        ms[us_to_ms(usecs)]++;

    // Third buckets are seconds from 1s to 100s, at 1s each.
    else if (usecs < sec_to_us(LATENCY_SEC_BUCKETS))
        sec[us_to_sec(usecs)]++;

    // >100 seconds, accumulate in the biggest bucket. */
    else
        sec[LATENCY_SEC_BUCKETS - 1]++;
}

void Track::subtract(const Track &other) {
    ops -= other.ops;
    latency_ops -= other.latency_ops;
    latency -= other.latency;

    // There's no sensible thing to be done for min/max_latency.

    if (us != NULL && other.us != NULL) {
        for (int i = 0; i < LATENCY_US_BUCKETS; i++)
            us[i] -= other.us[i];
        for (int i = 0; i < LATENCY_MS_BUCKETS; i++)
            ms[i] -= other.ms[i];
        for (int i = 0; i < LATENCY_SEC_BUCKETS; i++)
            sec[i] -= other.sec[i];
    }
}

// If there are no entries in this Track, take them from
// a previous Track. Used to smooth graphs.  We don't worry
// about latency buckets here.
void Track::smooth(const Track &other) {
    if (latency_ops == 0) {
        ops = other.ops;
        latency = other.latency;
        latency_ops = other.latency_ops;
        min_latency = other.min_latency;
        max_latency = other.max_latency;
    }
}

void Track::track_latency(bool newval) {
    if (newval) {
        if (us == NULL) {
            us = new uint32_t[LATENCY_US_BUCKETS];
            ms = new uint32_t[LATENCY_MS_BUCKETS];
            sec = new uint32_t[LATENCY_SEC_BUCKETS];
            memset(us, 0, sizeof(uint32_t) * LATENCY_US_BUCKETS);
            memset(ms, 0, sizeof(uint32_t) * LATENCY_MS_BUCKETS);
            memset(sec, 0, sizeof(uint32_t) * LATENCY_SEC_BUCKETS);
        }
    } else {
        if (us != NULL) {
            delete us;
            delete ms;
            delete sec;
            us = NULL;
            ms = NULL;
            sec = NULL;
        }
    }
}

void Track::_get_us(long *result) {
    if (us != NULL) {
        for (int i = 0; i < LATENCY_US_BUCKETS; i++)
            result[i] = (long)us[i];
    } else
        memset(result, 0, sizeof(long) * LATENCY_US_BUCKETS);
}
void Track::_get_ms(long *result) {
    if (ms != NULL) {
        for (int i = 0; i < LATENCY_MS_BUCKETS; i++)
            result[i] = (long)ms[i];
    } else
        memset(result, 0, sizeof(long) * LATENCY_MS_BUCKETS);
}
void Track::_get_sec(long *result) {
    if (sec != NULL) {
        for (int i = 0; i < LATENCY_SEC_BUCKETS; i++)
            result[i] = (long)sec[i];
    } else
        memset(result, 0, sizeof(long) * LATENCY_SEC_BUCKETS);
}

Stats::Stats(bool latency) : insert(latency), not_found(latency),
    read(latency), remove(latency), update(latency), truncate(latency) {
}

Stats::Stats(const Stats &other) : insert(other.insert),
    not_found(other.not_found), read(other.read), remove(other.remove),
    update(other.update), truncate(other.truncate) {
}

Stats::~Stats() {}

void Stats::add(Stats &other, bool reset) {
    insert.add(other.insert, reset);
    not_found.add(other.not_found, reset);
    read.add(other.read, reset);
    remove.add(other.remove, reset);
    update.add(other.update, reset);
    truncate.add(other.truncate, reset);
}

void Stats::assign(const Stats &other) {
    insert.assign(other.insert);
    not_found.assign(other.not_found);
    read.assign(other.read);
    remove.assign(other.remove);
    update.assign(other.update);
    truncate.assign(other.truncate);
}

void Stats::clear() {
    insert.clear();
    not_found.clear();
    read.clear();
    remove.clear();
    update.clear();
    truncate.clear();
}

void Stats::describe(std::ostream &os) const {
    os << "Stats: reads " << read.ops;
    if (not_found.ops > 0) {
        os << " (" << not_found.ops << " not found)";
    }
    os << ", inserts " << insert.ops;
    os << ", updates " << update.ops;
    os << ", truncates " << truncate.ops;
    os << ", removes " << remove.ops;
}

void Stats::final_report(std::ostream &os, timespec &totalsecs) const {
    uint64_t ops = 0;
    ops += read.ops;
    ops += not_found.ops;
    ops += insert.ops;
    ops += update.ops;
    ops += truncate.ops;
    ops += remove.ops;

#define FINAL_OUTPUT(os, field, singular, ops, totalsecs)               \
    os << "Executed " << field << " " #singular " operations ("         \
       << PCT(field, ops) << "%) " << OPS_PER_SEC(field, totalsecs)     \
       << " ops/sec" << std::endl

    FINAL_OUTPUT(os, read.ops, read, ops, totalsecs);
    FINAL_OUTPUT(os, not_found.ops, not found, ops, totalsecs);
    FINAL_OUTPUT(os, insert.ops, insert, ops, totalsecs);
    FINAL_OUTPUT(os, update.ops, update, ops, totalsecs);
    FINAL_OUTPUT(os, truncate.ops, truncate, ops, totalsecs);
    FINAL_OUTPUT(os, remove.ops, remove, ops, totalsecs);
}

void Stats::report(std::ostream &os) const {
    os << read.ops << " reads";
    if (not_found.ops > 0) {
        os << " (" << not_found.ops << " not found)";
    }
    os << ", " << insert.ops << " inserts, ";
    os << update.ops << " updates, ";
    os << truncate.ops << " truncates, ";
    os << remove.ops << " removes";
}

void Stats::smooth(const Stats &other) {
    insert.smooth(other.insert);
    not_found.smooth(other.not_found);
    read.smooth(other.read);
    remove.smooth(other.remove);
    update.smooth(other.update);
    truncate.smooth(other.truncate);
}

void Stats::subtract(const Stats &other) {
    insert.subtract(other.insert);
    not_found.subtract(other.not_found);
    read.subtract(other.read);
    remove.subtract(other.remove);
    update.subtract(other.update);
    truncate.subtract(other.truncate);
}

void Stats::track_latency(bool latency) {
    insert.track_latency(latency);
    not_found.track_latency(latency);
    read.track_latency(latency);
    remove.track_latency(latency);
    update.track_latency(latency);
    truncate.track_latency(latency);
}

TableOptions::TableOptions() : key_size(0), value_size(0), _options() {
    _options.add_int("key_size", key_size,
      "default size of the key, unless overridden by Key.size");
    _options.add_int("value_size", value_size,
      "default size of the value, unless overridden by Value.size");
}
TableOptions::TableOptions(const TableOptions &other) :
    key_size(other.key_size), value_size(other.value_size),
    _options(other._options) {}
TableOptions::~TableOptions() {}

Table::Table() : options(), _uri(), _internal(new TableInternal()) {
}
Table::Table(const char *uri) : options(), _uri(uri),
    _internal(new TableInternal()) {
}
Table::Table(const Table &other) : options(other.options), _uri(other._uri),
    _internal(new TableInternal(*other._internal)) {
}
Table::~Table() { delete _internal; }
Table& Table::operator=(const Table &other) {
    options = other.options;
    _uri = other._uri;
    *_internal = *other._internal;
    return (*this);
}

void Table::describe(std::ostream &os) const {
    os << "Table: " << _uri;
}

TableInternal::TableInternal() : _tint(0), _context_count(0) {}
TableInternal::TableInternal(const TableInternal &other) : _tint(other._tint),
    _context_count(other._context_count) {}
TableInternal::~TableInternal() {}

WorkloadOptions::WorkloadOptions() : max_latency(0),
    report_file("workload.stat"), report_interval(0), run_time(0),
    sample_file("sample.json"), sample_interval(0), sample_rate(1),
    _options() {
    _options.add_int("max_latency", max_latency,
      "prints warning if any latency measured exceeds this number of "
      "milliseconds. Requires sample_interval to be configured.");
    _options.add_int("report_interval", report_interval,
      "output throughput information every interval seconds, 0 to disable");
    _options.add_string("report_file", report_file,
      "file name for collecting run output, "
      "including output from the report_interval option. "
      "The file name is relative to the connection's home directory. "
      "When set to the empty string, stdout is used.");
    _options.add_int("run_time", run_time, "total workload seconds");
    _options.add_string("sample_file", sample_file,
      "file name for collecting latency output in a JSON-like format, "
      "enabled by the report_interval option. "
      "The file name is relative to the connection's home directory. "
      "When set to the empty string, no JSON is emitted.");
    _options.add_int("sample_interval", sample_interval,
      "performance logging every interval seconds, 0 to disable");
    _options.add_int("sample_rate", sample_rate,
      "how often the latency of operations is measured. 1 for every operation, "
      "2 for every second operation, 3 for every third operation etc.");
}

WorkloadOptions::WorkloadOptions(const WorkloadOptions &other) :
    max_latency(other.max_latency), report_interval(other.report_interval),
    run_time(other.run_time), sample_interval(other.sample_interval),
    sample_rate(other.sample_rate), _options(other._options) {}
WorkloadOptions::~WorkloadOptions() {}

Workload::Workload(Context *context, const ThreadListWrapper &tlw) :
    options(), stats(), _context(context), _threads(tlw._threads) {
    if (context == NULL)
        THROW("Workload contructor requires a Context");
}

Workload::Workload(Context *context, const Thread &thread) :
    options(), stats(), _context(context), _threads() {
    if (context == NULL)
        THROW("Workload contructor requires a Context");
    _threads.push_back(thread);
}

Workload::Workload(const Workload &other) :
    options(other.options), stats(other.stats), _context(other._context),
    _threads(other._threads) {}
Workload::~Workload() {}

Workload& Workload::operator=(const Workload &other) {
    options = other.options;
    stats.assign(other.stats);
    *_context = *other._context;
    _threads = other._threads;
    return (*this);
}

int Workload::run(WT_CONNECTION *conn) {
    WorkloadRunner runner(this);

    return (runner.run(conn));
}

WorkloadRunner::WorkloadRunner(Workload *workload) :
    _workload(workload), _trunners(workload->_threads.size()),
    _report_out(&std::cout), _start() {
    ts_clear(_start);
}
WorkloadRunner::~WorkloadRunner() {}

int WorkloadRunner::run(WT_CONNECTION *conn) {
    WT_DECL_RET;
    WorkloadOptions *options = &_workload->options;
    std::ofstream report_out;

    _wt_home = conn->get_home(conn);
    if (options->sample_interval > 0 && options->sample_rate <= 0)
        THROW("Workload.options.sample_rate must be positive");
    if (!options->report_file.empty()) {
        open_report_file(report_out, options->report_file.c_str(),
          "Workload.options.report_file");
        _report_out = &report_out;
    }
    WT_ERR(create_all(conn, _workload->_context));
    WT_ERR(open_all());
    WT_ERR(ThreadRunner::cross_check(_trunners));
    WT_ERR(run_all());
  err:
    //TODO: (void)close_all();
    _report_out = &std::cout;
    return (ret);
}

int WorkloadRunner::open_all() {
    for (size_t i = 0; i < _trunners.size(); i++) {
        WT_RET(_trunners[i].open_all());
    }
    return (0);
}

void WorkloadRunner::open_report_file(std::ofstream &of, const char *filename,
  const char *desc) {
    std::stringstream sstm;

    if (!_wt_home.empty())
        sstm << _wt_home << "/";
    sstm << filename;
    of.open(sstm.str().c_str(), std::fstream::app);
    if (!of)
        THROW_ERRNO(errno, desc << ": \"" << sstm.str()
          << "\" could not be opened");
}

int WorkloadRunner::create_all(WT_CONNECTION *conn, Context *context) {
    for (size_t i = 0; i < _trunners.size(); i++) {
        ThreadRunner *runner = &_trunners[i];
        std::stringstream sstm;
        Thread *thread = &_workload->_threads[i];
        if (thread->options.name.empty()) {
            sstm << "thread" << i;
            thread->options.name = sstm.str();
        }
        runner->_thread = thread;
        runner->_context = context;
        runner->_icontext = context->_internal;
        runner->_workload = _workload;
        runner->_wrunner = this;
        runner->_number = (uint32_t)i;
        // TODO: recover from partial failure here
        WT_RET(runner->create_all(conn));
    }
    WT_RET(context->_internal->create_all());
    return (0);
}

int WorkloadRunner::close_all() {
    for (size_t i = 0; i < _trunners.size(); i++)
        _trunners[i].close_all();

    return (0);
}

void WorkloadRunner::get_stats(Stats *result) {
    for (size_t i = 0; i < _trunners.size(); i++)
        result->add(_trunners[i]._stats);
}

void WorkloadRunner::report(time_t interval, time_t totalsecs,
  Stats *prev_totals) {
    std::ostream &out = *_report_out;
    Stats new_totals(prev_totals->track_latency());

    get_stats(&new_totals);
    Stats diff(new_totals);
    diff.subtract(*prev_totals);
    prev_totals->assign(new_totals);
    diff.report(out);
    out << " in " << interval << " secs ("
        << totalsecs << " total secs)" << std::endl;
}

void WorkloadRunner::final_report(timespec &totalsecs) {
    std::ostream &out = *_report_out;
    Stats *stats = &_workload->stats;

    stats->clear();
    stats->track_latency(_workload->options.sample_interval > 0);

    get_stats(stats);
    stats->final_report(out, totalsecs);
    out << "Run completed: " << totalsecs << " seconds" << std::endl;
}

int WorkloadRunner::run_all() {
    void *status;
    std::vector<pthread_t> thread_handles;
    Stats counts(false);
    WorkgenException *exception;
    WorkloadOptions *options = &_workload->options;
    Monitor monitor(*this);
    std::ofstream monitor_out;
    std::ofstream monitor_json;
    std::ostream &out = *_report_out;
    WT_DECL_RET;

    for (size_t i = 0; i < _trunners.size(); i++)
        _trunners[i].get_static_counts(counts);
    out << "Starting workload: " << _trunners.size() << " threads, ";
    counts.report(out);
    out << std::endl;

    workgen_epoch(&_start);
    timespec end = _start + options->run_time;
    timespec next_report = _start + options->report_interval;

    // Start all threads
    if (options->sample_interval > 0) {
        open_report_file(monitor_out, "monitor", "monitor output file");
        monitor._out = &monitor_out;

        if (!options->sample_file.empty()) {
            open_report_file(monitor_json, options->sample_file.c_str(),
              "sample JSON output file");
            monitor._json = &monitor_json;
        }

        if ((ret = pthread_create(&monitor._handle, NULL, monitor_main,
          &monitor)) != 0) {
            std::cerr << "monitor thread failed err=" << ret << std::endl;
            return (ret);
        }
    }

    for (size_t i = 0; i < _trunners.size(); i++) {
        pthread_t thandle;
        ThreadRunner *runner = &_trunners[i];
        runner->_stop = false;
        runner->_repeat = (options->run_time != 0);
        if ((ret = pthread_create(&thandle, NULL, thread_runner_main,
          runner)) != 0) {
            std::cerr << "pthread_create failed err=" << ret << std::endl;
            std::cerr << "Stopping all threads." << std::endl;
            for (size_t j = 0; j < thread_handles.size(); j++) {
                _trunners[j]._stop = true;
                (void)pthread_join(thread_handles[j], &status);
                _trunners[j].close_all();
            }
            return (ret);
        }
        thread_handles.push_back(thandle);
        runner->_stats.clear();
    }

    // Let the test run, reporting as needed.
    Stats curstats(false);
    timespec now = _start;
    while (now < end) {
        timespec sleep_amt;

        sleep_amt = end - now;
        if (next_report != 0) {
            timespec next_diff = next_report - now;
            if (next_diff < next_report)
                sleep_amt = next_diff;
        }
        if (sleep_amt.tv_sec > 0)
            sleep((unsigned int)sleep_amt.tv_sec);
        else
            usleep((useconds_t)((sleep_amt.tv_nsec + 999)/ 1000));

        workgen_epoch(&now);
        if (now >= next_report && now < end && options->report_interval != 0) {
            report(options->report_interval, (now - _start).tv_sec, &curstats);
            while (now >= next_report)
                next_report += options->report_interval;
        }
    }

    // signal all threads to stop
    if (options->run_time != 0)
        for (size_t i = 0; i < _trunners.size(); i++)
            _trunners[i]._stop = true;
    if (options->sample_interval > 0)
        monitor._stop = true;

    // wait for all threads
    exception = NULL;
    for (size_t i = 0; i < _trunners.size(); i++) {
        WT_TRET(pthread_join(thread_handles[i], &status));
        if (_trunners[i]._errno != 0)
            VERBOSE(_trunners[i],
                    "Thread " << i << " has errno " << _trunners[i]._errno);
        WT_TRET(_trunners[i]._errno);
        _trunners[i].close_all();
        if (exception == NULL && !_trunners[i]._exception._str.empty())
            exception = &_trunners[i]._exception;
    }
    if (options->sample_interval > 0) {
        WT_TRET(pthread_join(monitor._handle, &status));
        if (monitor._errno != 0)
            std::cerr << "Monitor thread has errno " << monitor._errno
                      << std::endl;
        if (exception == NULL && !monitor._exception._str.empty())
            exception = &monitor._exception;

        monitor_out.close();
        if (!options->sample_file.empty())
            monitor_json.close();
    }

    // issue the final report
    timespec finalsecs = now - _start;
    final_report(finalsecs);

    if (ret != 0)
        std::cerr << "run_all failed err=" << ret << std::endl;
    (*_report_out) << std::endl;
    if (exception != NULL)
        throw *exception;
    return (ret);
}

};
