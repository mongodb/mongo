/**
 *    Copyright (C) 2017 MongoDB, Inc.
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

#pragma once

#include <memory>

#include "mongo/db/signed_logical_time.h"
#include "mongo/db/time_proof_service.h"
#include "mongo/stdx/mutex.h"

namespace mongo {

class OperationContext;
class ServiceContext;
class KeysCollectionDocument;
class KeysCollectionManager;

/**
 * This is responsible for signing logical times that can be used to sent to other servers and
 * verifying signatures of signed logical times.
 */
class LogicalTimeValidator {
public:
    // Decorate ServiceContext with LogicalTimeValidator instance.
    static LogicalTimeValidator* get(ServiceContext* service);
    static LogicalTimeValidator* get(OperationContext* ctx);
    static void set(ServiceContext* service, std::unique_ptr<LogicalTimeValidator> validator);

    explicit LogicalTimeValidator(std::unique_ptr<KeysCollectionManager> keyManager);

    /**
     * Tries to sign the newTime with a valid signature. Can return an empty signature and keyId
     * of 0 if it cannot find valid key for newTime.
     */
    SignedLogicalTime trySignLogicalTime(const LogicalTime& newTime);

    /**
     * Returns the newTime with a valid signature.
     */
    SignedLogicalTime signLogicalTime(OperationContext* opCtx, const LogicalTime& newTime);

    /**
     * Returns true if the signature of newTime is valid.
     */
    Status validate(OperationContext* opCtx, const SignedLogicalTime& newTime);

    /**
     * Initializes this validator. This should be called first before the other methods can be used.
     */
    void init(ServiceContext* service);

    /**
     * Cleans up this validator. This will no longer be usable after this is called.
     */
    void shutDown();

    /**
     * Enable writing new keys to the config server primary. Should only be called if current node
     * is the config primary.
     */
    void enableKeyGenerator(OperationContext* opCtx, bool doEnable);

    /**
     * Returns true if client has sufficient privilege to advance clock.
     */
    static bool isAuthorizedToAdvanceClock(OperationContext* opCtx);

private:
    SignedLogicalTime _getProof(const KeysCollectionDocument& keyDoc, LogicalTime newTime);

    stdx::mutex _mutex;
    SignedLogicalTime _lastSeenValidTime;
    TimeProofService _timeProofService;
    std::unique_ptr<KeysCollectionManager> _keyManager;
};

}  // namespace mongo
