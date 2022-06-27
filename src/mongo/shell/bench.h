/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/client/dbclient_base.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/ops/write_ops.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/pcre.h"
#include "mongo/util/timer.h"

namespace mongo {

enum class OpType {
    NONE,
    NOP,
    FINDONE,
    COMMAND,
    FIND,
    UPDATE,
    INSERT,
    REMOVE,
    CREATEINDEX,
    DROPINDEX,
    LET,
    CPULOAD
};

inline bool isReadOp(OpType opType) {
    return opType == OpType::FINDONE || opType == OpType::FIND;
}

inline bool isWriteOp(OpType opType) {
    return opType == OpType::UPDATE || opType == OpType::INSERT || opType == OpType::REMOVE;
}

class BenchRunConfig;
struct BenchRunStats;
class BsonTemplateEvaluator;
class LogicalSessionIdToClient;

/**
 * Object representing one operation passed to benchRun
 */
struct BenchRunOp {
    struct State {
        State(PseudoRandom* rng_,
              BsonTemplateEvaluator* bsonTemplateEvaluator_,
              BenchRunStats* stats_)
            : rng(rng_), bsonTemplateEvaluator(bsonTemplateEvaluator_), stats(stats_) {}

        PseudoRandom* rng;
        BsonTemplateEvaluator* bsonTemplateEvaluator;
        BenchRunStats* stats;

        // Transaction state
        TxnNumber txnNumber = 0;
        bool inProgressMultiStatementTxn = false;
    };

    void executeOnce(DBClientBase* conn,
                     const boost::optional<LogicalSessionIdToClient>& lsid,
                     const BenchRunConfig& config,
                     State* state) const;

    int batchSize = 0;
    BSONElement check;
    BSONObj command;
    BSONObj context;
    double cpuFactor = 1;
    int delay = 0;
    BSONObj doc;
    bool isDocAnArray = false;
    int expected = -1;
    bool handleError = false;
    BSONObj key;
    int limit = 0;
    bool multi = false;
    std::string ns;
    OpType op = OpType::NONE;
    int options = 0;
    BSONObj projection;
    BSONObj query;
    int skip = 0;
    BSONObj sort;
    bool showError = false;
    std::string target;
    bool throwGLE = false;
    write_ops::UpdateModification update;
    bool upsert = false;
    bool useCheck = false;
    bool useReadCmd = false;
    bool useWriteCmd = false;
    BSONObj writeConcern;
    BSONObj value;

    // Only used for find cmds when set greater than 0. A find operation will retrieve the latest
    // cluster time from the oplog and randomly chooses a time between that timestamp and
    // 'useClusterTimeWithinSeconds' seconds in the past at which to read.
    //
    // Must be used with 'useSnapshotReads=true' BenchRunConfig because atClusterTime is only
    // allowed within a transaction.
    int useAClusterTimeWithinPastSeconds = 0;

    // Delays getMore commands following a find cmd. A uniformly distributed random value between 0
    // and maxRandomMillisecondDelayBeforeGetMore will be generated for each getMore call. Useful
    // for keeping a snapshot read open for a longer time duration, say to simulate the basic
    // resources that a snapshot transaction would hold for a time.
    int maxRandomMillisecondDelayBeforeGetMore{0};

    // Format: {$readPreference: {mode: modeStr}}.  Only mode field is allowed.
    BSONObj readPrefObj;

    // This is an owned copy of the raw operation. All unowned members point into this.
    BSONObj myBsonOp;
};

/**
 * Configuration object describing a bench run activity.
 */
class BenchRunConfig {
    BenchRunConfig(const BenchRunConfig&) = delete;
    BenchRunConfig& operator=(const BenchRunConfig&) = delete;

public:
    /**
     * Create a new BenchRunConfig object, and initialize it from the BSON
     * document, "args".
     *
     * Caller owns the returned object, and is responsible for its deletion.
     */
    static BenchRunConfig* createFromBson(const BSONObj& args);

    static std::unique_ptr<DBClientBase> createConnectionImpl(const BenchRunConfig&);

    BenchRunConfig();

    void initializeFromBson(const BSONObj& args);

    // Create a new connection to the mongo instance specified by this configuration.
    std::unique_ptr<DBClientBase> createConnection() const;

    /**
     * Connection std::string describing the host to which to connect.
     */
    std::string host;

    /**
     * Name of the database on which to operate.
     */
    std::string db;

    /**
     * Optional username for authenticating to the database.
     */
    std::string username;

    /**
     * Optional password for authenticating to the database.
     *
     * Only useful if username is non-empty.
     */
    std::string password;

    /**
     * Number of parallel threads to perform the bench run activity.
     */
    unsigned parallel;

    /**
     * Desired duration of the bench run activity, in seconds.
     *
     * NOTE: Only used by the javascript benchRun() and benchRunSync() functions.
     */
    double seconds;

    /**
     * Whether the individual benchRun thread connections should be creating and using sessions.
     */
    bool useSessions{false};

    /**
     * Whether write commands should be sent with a txnNumber to ensure they are idempotent. This
     * setting doesn't actually cause the workload generator to perform any retries.
     */
    bool useIdempotentWrites{false};

    /**
     * Whether read commands should be sent with a txnNumber and read concern level snapshot.
     */
    bool useSnapshotReads{false};

    /**
     * How many milliseconds to sleep for if an operation fails, before continuing to the next op.
     */
    Milliseconds delayMillisOnFailedOperation{0};

    /// Base random seed for threads
    int64_t randomSeed;

    bool handleErrors;
    bool hideErrors;

    std::shared_ptr<pcre::Regex> trapPattern;
    std::shared_ptr<pcre::Regex> noTrapPattern;
    std::shared_ptr<pcre::Regex> watchPattern;
    std::shared_ptr<pcre::Regex> noWatchPattern;

    /**
     * Operation description. A list of BenchRunOps, each describing a single
     * operation.
     *
     * Every thread in a benchRun job will perform these operations in sequence, restarting at
     * the beginning when the end is reached, until the job is stopped.
     *
     * TODO: Introduce support for performing each operation exactly N times.
     */
    std::vector<BenchRunOp> ops;

    bool throwGLE;
    bool breakOnTrap;

private:
    static std::function<std::unique_ptr<DBClientBase>(const BenchRunConfig&)> _factory;

    /// Initialize a config object to its default values.
    void initializeToDefaults();
};

/**
 * An event counter for events that have an associated duration.
 *
 * Not thread safe. Expected use is one instance per thread during parallel execution.
 */
class BenchRunEventCounter {
public:
    BenchRunEventCounter();

    /**
     * Conceptually the equivalent of "+=". Adds "other" into this.
     */
    void updateFrom(const BenchRunEventCounter& other);

    /**
     * Count one instance of the event, which took "timeMicros" microseconds.
     */
    void countOne(long long timeMicros) {
        if (_numEvents == std::numeric_limits<long long>::max()) {
            fassertFailedWithStatus(50874,
                                    {ErrorCodes::Overflow, "Overflowed the events counter."});
        }
        ++_numEvents;
        _totalTimeMicros += timeMicros;
    }

    /**
     * Get the total number of microseconds ellapsed during all observed events.
     */
    unsigned long long getTotalTimeMicros() const {
        return _totalTimeMicros;
    }

    /**
     * Get the number of observed events.
     */
    long long getNumEvents() const {
        return _numEvents;
    }

private:
    long long _totalTimeMicros{0};
    long long _numEvents{0};
};

/**
 * RAII object for tracing an event.
 *
 * Construct an instance of this at the beginning of an event, and have it go out of scope at
 * the end, to facilitate tracking events.
 *
 * This type can be used to separately count failures and successes by passing two event
 * counters to the BenchRunEventCounter constructor, and calling "succeed()" on the object at
 * the end of a successful event. If an exception is thrown, the fail counter will receive the
 * event, and otherwise, the succes counter will.
 *
 * In all cases, the counter objects must outlive the trace object.
 */
class BenchRunEventTrace {
    BenchRunEventTrace(const BenchRunEventTrace&) = delete;
    BenchRunEventTrace& operator=(const BenchRunEventTrace&) = delete;

public:
    explicit BenchRunEventTrace(BenchRunEventCounter* eventCounter) {
        initialize(eventCounter, eventCounter, false);
    }

    BenchRunEventTrace(BenchRunEventCounter* successCounter,
                       BenchRunEventCounter* failCounter,
                       bool defaultToFailure = true) {
        initialize(successCounter, failCounter, defaultToFailure);
    }

    ~BenchRunEventTrace() {
        (_succeeded ? _successCounter : _failCounter)->countOne(_timer.micros());
    }

    void succeed() {
        _succeeded = true;
    }
    void fail() {
        _succeeded = false;
    }

private:
    void initialize(BenchRunEventCounter* successCounter,
                    BenchRunEventCounter* failCounter,
                    bool defaultToFailure) {
        _successCounter = successCounter;
        _failCounter = failCounter;
        _succeeded = !defaultToFailure;
    }

    Timer _timer;
    BenchRunEventCounter* _successCounter;
    BenchRunEventCounter* _failCounter;
    bool _succeeded;
};

/**
 * Statistics object representing the result of a bench run activity.
 */
struct BenchRunStats {
    void updateFrom(const BenchRunStats& other);

    bool error{false};

    unsigned long long errCount{0};
    unsigned long long opCount{0};

    BenchRunEventCounter findOneCounter;
    BenchRunEventCounter updateCounter;
    BenchRunEventCounter insertCounter;
    BenchRunEventCounter deleteCounter;
    BenchRunEventCounter queryCounter;
    BenchRunEventCounter commandCounter;

    std::map<std::string, long long> opcounters;
    std::vector<BSONObj> trappedErrors;
};

/**
 * State of a BenchRun activity.
 *
 * Logically, the states are "starting up", "running" and "finished."
 */
class BenchRunState {
    BenchRunState(const BenchRunState&) = delete;
    BenchRunState& operator=(const BenchRunState&) = delete;

public:
    enum State { BRS_STARTING_UP, BRS_RUNNING, BRS_FINISHED };

    explicit BenchRunState(unsigned numWorkers);
    ~BenchRunState();

    //
    // Functions called by the job-controlling thread, through an instance of BenchRunner.
    //

    /**
     * Block until the current state is "awaitedState."
     *
     * massert() (uassert()?) if "awaitedState" is unreachable from
     * the current state.
     */
    void waitForState(State awaitedState);

    /**
     * Notify the worker threads to wrap up. Does not block.
     */
    void tellWorkersToFinish();

    /**
     * Notify the worker threads to collect statistics. Does not block.
     */
    void tellWorkersToCollectStats();

    /**
     * Check that the current state is BRS_FINISHED.
     */
    void assertFinished() const;

    //
    // Functions called by the worker threads, through instances of BenchRunWorker.
    //

    /**
     * Predicate that workers call to see if they should finish (as a result of a call
     * to tellWorkersToFinish()).
     */
    bool shouldWorkerFinish() const;

    /**
     * Predicate that workers call to see if they should start collecting stats (as a result
     * of a call to tellWorkersToCollectStats()).
     */
    bool shouldWorkerCollectStats() const;

    /**
     * Called by each BenchRunWorker from within its thread context, immediately before it
     * starts sending requests to the configured mongo instance.
     */
    void onWorkerStarted();

    /**
     * Called by each BenchRunWorker from within its thread context, shortly after it finishes
     * sending requests to the configured mongo instance.
     */
    void onWorkerFinished();

private:
    mutable Mutex _mutex = MONGO_MAKE_LATCH("BenchRunState::_mutex");

    stdx::condition_variable _stateChangeCondition;

    unsigned _numUnstartedWorkers;
    unsigned _numActiveWorkers;

    AtomicWord<unsigned> _isShuttingDown;
    AtomicWord<unsigned> _isCollectingStats;
};

/**
 * A single worker in the bench run activity.
 *
 * Represents the behavior of one thread working in a bench run activity.
 */
class BenchRunWorker {
    BenchRunWorker(const BenchRunWorker&) = delete;
    BenchRunWorker& operator=(const BenchRunWorker&) = delete;

public:
    /**
     * Create a new worker, performing one thread's worth of the activity described in
     * "config", and part of the larger activity with state "brState". Both "config"
     * and "brState" must exist for the life of this object.
     *
     * "id" is a positive integer which should uniquely identify the worker.
     */
    BenchRunWorker(size_t id,
                   const BenchRunConfig* config,
                   BenchRunState& brState,
                   int64_t randomSeed);
    ~BenchRunWorker();

    /**
     * Start performing the "work" behavior in a new thread.
     */
    void start();

    /**
     * Get the run statistics for a worker.
     *
     * Should only be observed _after_ the worker has signaled its completion by calling
     * onWorkerFinished() on the BenchRunState passed into its constructor.
     */
    const BenchRunStats& stats() const {
        return _stats;
    }

private:
    /// The main method of the worker, executed inside the thread launched by start().
    void run();

    /// The function that actually sets about generating the load described in "_config".
    void generateLoadOnConnection(DBClientBase* conn);

    /// Predicate, used to decide whether or not it's time to terminate the worker.
    bool shouldStop() const;
    /// Predicate, used to decide whether or not it's time to collect statistics
    bool shouldCollectStats() const;

    stdx::thread _thread;

    const size_t _id;

    const BenchRunConfig* _config;

    BenchRunState& _brState;

    PseudoRandom _rng;

    // Dummy stats to use before observation period.
    BenchRunStats _statsBlackHole;

    // Actual stats collected during the run
    BenchRunStats _stats;
};

/**
 * Object representing a "bench run" activity.
 */
class BenchRunner {
    BenchRunner(const BenchRunner&) = delete;
    BenchRunner& operator=(const BenchRunner&) = delete;

public:
    /**
     * Utility method to create a new bench runner from a BSONObj representation
     * of a configuration.
     *
     * TODO: This is only really for the use of the javascript benchRun() methods,
     * and should probably move out of the BenchRunner class.
     */
    static BenchRunner* createWithConfig(const BSONObj& configArgs);

    /**
     * Look up a bench runner object by OID.
     *
     * TODO: Same todo as for "createWithConfig".
     */
    static BenchRunner* get(OID oid);

    /**
     * Stop a running "runner", and return a BSON representation of its resultant
     * BenchRunStats.
     *
     * TODO: Same as for "createWithConfig".
     */
    static BSONObj finish(BenchRunner* runner);

    /**
     * Create a new bench runner, to perform the activity described by "*config."
     *
     * Takes ownership of "config", and will delete it.
     */
    explicit BenchRunner(BenchRunConfig* config);
    ~BenchRunner();

    /**
     * Start the activity. Only call once per instance of BenchRunner.
     */
    void start();

    /**
     * Stop the activity. Block until the activitiy has stopped.
     */
    void stop();

    /**
     * Store the collected event data from a completed bench run activity into "stats."
     *
     * Illegal to call until after stop() returns.
     */
    BenchRunStats gatherStats() const;

    OID oid() const {
        return _oid;
    }

    const BenchRunConfig& config() const {
        return *_config;
    }  // TODO: Remove this function.

    // JS bindings
    static BSONObj benchFinish(const BSONObj& argsFake, void* data);
    static BSONObj benchStart(const BSONObj& argsFake, void* data);
    static BSONObj benchRunSync(const BSONObj& argsFake, void* data);

private:
    // TODO: Same as for createWithConfig.
    static Mutex _staticMutex;
    static std::map<OID, BenchRunner*> _activeRuns;

    OID _oid;
    BenchRunState _brState;

    boost::optional<Timer> _brTimer;
    unsigned long long _microsElapsed;

    std::unique_ptr<BenchRunConfig> _config;
    std::vector<std::unique_ptr<BenchRunWorker>> _workers;
};

}  // namespace mongo
