/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/operation_context_impl.h"

#include <memory>

#include "mongo/db/client.h"
#include "mongo/db/concurrency/lock_state.h"
#include "mongo/db/curop.h"
#include "mongo/db/service_context.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
    std::unique_ptr<Locker> newLocker() {
        if (isMMAPV1()) return stdx::make_unique<MMAPV1LockerImpl>();
        return stdx::make_unique<DefaultLockerImpl>();
    }

    class ClientOperationInfo {
    public:
        Locker* getLocker() {
            if (!_locker) {
                _locker = newLocker();
            }
            return _locker.get();
        }

    private:
        std::unique_ptr<Locker> _locker;
    };

    const auto clientOperationInfoDecoration = Client::declareDecoration<ClientOperationInfo>();

}  // namespace

    using std::string;

    OperationContextImpl::OperationContextImpl()
        : _client(&cc()),
          _locker(clientOperationInfoDecoration(_client).getLocker()),
          _writesAreReplicated(true) {

        invariant(_locker);

        StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
        _recovery.reset(storageEngine->newRecoveryUnit());

        _client->setOperationContext(this);
    }

    OperationContextImpl::~OperationContextImpl() {
        _locker->assertEmptyAndReset();
        _client->resetOperationContext();
    }

    RecoveryUnit* OperationContextImpl::recoveryUnit() const {
        return _recovery.get();
    }

    RecoveryUnit* OperationContextImpl::releaseRecoveryUnit() {
        if ( _recovery.get() )
            _recovery->beingReleasedFromOperationContext();
        return _recovery.release();
    }

    void OperationContextImpl::setRecoveryUnit(RecoveryUnit* unit) {
        _recovery.reset(unit);
        if ( unit )
            unit->beingSetOnOperationContext();
    }

    Locker* OperationContextImpl::lockState() const {
        return _locker;
    }

    ProgressMeter* OperationContextImpl::setMessage(const char * msg,
                                                    const std::string &name,
                                                    unsigned long long progressMeterTotal,
                                                    int secondsBetween) {
        return &getCurOp()->setMessage(msg, name, progressMeterTotal, secondsBetween);
    }

    string OperationContextImpl::getNS() const {
        return getCurOp()->getNS();
    }

    Client* OperationContextImpl::getClient() const {
        return _client;
    }

    CurOp* OperationContextImpl::getCurOp() const {
        return CurOp::get(getClient());
    }

    unsigned int OperationContextImpl::getOpID() const {
        return getCurOp()->opNum();
    }

    uint64_t OperationContextImpl::getRemainingMaxTimeMicros() const {
        return getCurOp()->getRemainingMaxTimeMicros();
    }

    // Enabling the checkForInterruptFail fail point will start a game of random chance on the
    // connection specified in the fail point data, generating an interrupt with a given fixed
    // probability.  Example invocation:
    //
    // {configureFailPoint: "checkForInterruptFail",
    //  mode: "alwaysOn",
    //  data: {conn: 17, chance: .01, allowNested: true}}
    //
    // All three data fields must be specified.  In the above example, all interrupt points on
    // connection 17 will generate a kill on the current operation with probability p(.01),
    // including interrupt points of nested operations.  If "allowNested" is false, nested
    // operations are not targeted.  "chance" must be a double between 0 and 1, inclusive.
    MONGO_FP_DECLARE(checkForInterruptFail);

    namespace {

        // Global state for checkForInterrupt fail point.
        PseudoRandom checkForInterruptPRNG(static_cast<int64_t>(time(NULL)));

        // Helper function for checkForInterrupt fail point.  Decides whether the operation currently
        // being run by the given Client meet the (probabilistic) conditions for interruption as
        // specified in the fail point info.
        bool opShouldFail(const Client& c, const BSONObj& failPointInfo) {
            // Only target the client with the specified connection number.
            if (c.getConnectionId() != failPointInfo["conn"].safeNumberLong()) {
                return false;
            }

            // Only target nested operations if requested.
            if (!failPointInfo["allowNested"].trueValue() && CurOp::get(c)->parent() != NULL) {
                return false;
            }

            // Return true with (approx) probability p = "chance".  Recall: 0 <= chance <= 1.
            double next = static_cast<double>(std::abs(checkForInterruptPRNG.nextInt64()));
            double upperBound =
                std::numeric_limits<int64_t>::max() * failPointInfo["chance"].numberDouble();
            if (next > upperBound) {
                return false;
            }
            return true;
        }

    } // namespace

    void OperationContextImpl::checkForInterrupt() const {
        // We cannot interrupt operation, while it's inside of a write unit of work, because logOp
        // cannot handle being iterrupted.
        if (lockState()->inAWriteUnitOfWork()) return;

        uassertStatusOK(checkForInterruptNoAssert());
    }

    Status OperationContextImpl::checkForInterruptNoAssert() const {
        if (getGlobalServiceContext()->getKillAllOperations()) {
            return Status(ErrorCodes::InterruptedAtShutdown, "interrupted at shutdown");
        }

        Client* c = getClient();
        if (CurOp::get(c)->maxTimeHasExpired()) {
            CurOp::get(c)->kill();
            return Status(ErrorCodes::ExceededTimeLimit, "operation exceeded time limit");
        }

        MONGO_FAIL_POINT_BLOCK(checkForInterruptFail, scopedFailPoint) {
            if (opShouldFail(*c, scopedFailPoint.getData())) {
                log() << "set pending kill on "
                      << (CurOp::get(c)->parent() ? "nested" : "top-level")
                      << " op " << CurOp::get(c)->opNum() << ", for checkForInterruptFail";
                CurOp::get(c)->kill();
            }
        }

        if (CurOp::get(c)->killPending()) {
            return Status(ErrorCodes::Interrupted, "operation was interrupted");
        }

        return Status::OK();
    }

    bool OperationContextImpl::isPrimaryFor( StringData ns ) {
        return repl::getGlobalReplicationCoordinator()->canAcceptWritesForDatabase(
                NamespaceString(ns).db());
    }

    void OperationContextImpl::setReplicatedWrites(bool writesAreReplicated) {
        _writesAreReplicated = writesAreReplicated;
    }

    bool OperationContextImpl::writesAreReplicated() const {
        return _writesAreReplicated;
    }
}  // namespace mongo
