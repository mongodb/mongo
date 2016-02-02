/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/db/s/migration_session_id.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/timer.h"

namespace mongo {

class OperationContext;
class Status;
struct WriteConcernOptions;

namespace repl {
class OpTime;
}

/**
 * Drives the receiving side of the MongoD migration process. One instance exists per shard.
 */
class MigrationDestinationManager {
    MONGO_DISALLOW_COPYING(MigrationDestinationManager);

public:
    enum State { READY, CLONE, CATCHUP, STEADY, COMMIT_START, DONE, FAIL, ABORT };

    MigrationDestinationManager();
    ~MigrationDestinationManager();

    State getState() const;
    void setState(State newState);

    bool isActive() const;

    /**
     * Reports the state of the migration manager as a BSON document.
     */
    void report(BSONObjBuilder& b);

    /**
     * Returns OK if migration started successfully.
     */
    Status start(const std::string& ns,
                 const MigrationSessionId& sessionId,
                 const std::string& fromShard,
                 const BSONObj& min,
                 const BSONObj& max,
                 const BSONObj& shardKeyPattern,
                 const OID& epoch,
                 const WriteConcernOptions& writeConcern);

    void abort();

    bool startCommit(const MigrationSessionId& sessionId);

private:
    /**
     * Thread which drives the migration apply process on the recipient side.
     */
    void _migrateThread(std::string ns,
                        MigrationSessionId sessionId,
                        BSONObj min,
                        BSONObj max,
                        BSONObj shardKeyPattern,
                        std::string fromShard,
                        OID epoch,
                        WriteConcernOptions writeConcern);

    void _migrateDriver(OperationContext* txn,
                        const std::string& ns,
                        const MigrationSessionId& sessionId,
                        const BSONObj& min,
                        const BSONObj& max,
                        const BSONObj& shardKeyPattern,
                        const std::string& fromShard,
                        const OID& epoch,
                        const WriteConcernOptions& writeConcern);

    bool _applyMigrateOp(OperationContext* txn,
                         const std::string& ns,
                         const BSONObj& min,
                         const BSONObj& max,
                         const BSONObj& shardKeyPattern,
                         const BSONObj& xfer,
                         repl::OpTime* lastOpApplied);

    bool _flushPendingWrites(OperationContext* txn,
                             const std::string& ns,
                             BSONObj min,
                             BSONObj max,
                             const repl::OpTime& lastOpApplied,
                             const WriteConcernOptions& writeConcern);

    // Mutex to guard all fields
    mutable stdx::mutex _mutex;

    // Migration session ID uniquely identifies the migration and indicates whether the prepare
    // method has been called.
    boost::optional<MigrationSessionId> _sessionId{boost::none};
    // A condition variable on which to wait for the prepare method to be called.
    stdx::condition_variable _isActiveCV;

    stdx::thread _migrateThreadHandle;

    std::string _ns;
    std::string _from;

    BSONObj _min;
    BSONObj _max;
    BSONObj _shardKeyPattern;

    long long _numCloned{0};
    long long _clonedBytes{0};
    long long _numCatchup{0};
    long long _numSteady{0};

    State _state{READY};
    std::string _errmsg;
};

class MoveTimingHelper {
public:
    MoveTimingHelper(OperationContext* txn,
                     const std::string& where,
                     const std::string& ns,
                     const BSONObj& min,
                     const BSONObj& max,
                     int totalNumSteps,
                     std::string* cmdErrmsg,
                     const std::string& toShard,
                     const std::string& fromShard);
    ~MoveTimingHelper();

    void done(int step);

private:
    // Measures how long the receiving of a chunk takes
    Timer _t;

    OperationContext* const _txn;
    const std::string _where;
    const std::string _ns;
    const std::string _to;
    const std::string _from;
    const int _totalNumSteps;
    const std::string* _cmdErrmsg;

    int _nextStep;
    BSONObjBuilder _b;
};

}  // namespace mongo
