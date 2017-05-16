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

#include "mongo/platform/basic.h"

#include "mongo/db/logical_time_validator.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {

namespace {
const auto getLogicalClockValidator =
    ServiceContext::declareDecoration<std::unique_ptr<LogicalTimeValidator>>();
stdx::mutex validatorMutex;

// TODO: SERVER-28127 Implement KeysCollectionManager
// Remove _tempKey and its uses from logical clock, and pass actual key from key manager.
TimeProofService::Key tempKey = {};
}

LogicalTimeValidator* LogicalTimeValidator::get(ServiceContext* service) {
    stdx::lock_guard<stdx::mutex> lk(validatorMutex);
    return getLogicalClockValidator(service).get();
}

LogicalTimeValidator* LogicalTimeValidator::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void LogicalTimeValidator::set(ServiceContext* service,
                               std::unique_ptr<LogicalTimeValidator> newValidator) {
    stdx::lock_guard<stdx::mutex> lk(validatorMutex);
    auto& validator = getLogicalClockValidator(service);
    validator = std::move(newValidator);
}

SignedLogicalTime LogicalTimeValidator::signLogicalTime(const LogicalTime& newTime) {
    // Compare and calculate HMAC inside mutex to prevent multiple threads computing HMAC for the
    // same logical time.
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    // Note: _lastSeenValidTime will initially not have a proof set.
    if (newTime == _lastSeenValidTime.getTime() && _lastSeenValidTime.getProof()) {
        return _lastSeenValidTime;
    }

    auto signature = _timeProofService.getProof(newTime, tempKey);
    SignedLogicalTime newSignedTime(newTime, std::move(signature), 0);

    if (newTime > _lastSeenValidTime.getTime() || !_lastSeenValidTime.getProof()) {
        _lastSeenValidTime = newSignedTime;
    }

    return newSignedTime;
}

Status LogicalTimeValidator::validate(const SignedLogicalTime& newTime) {
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        if (newTime.getTime() == _lastSeenValidTime.getTime()) {
            return Status::OK();
        }
    }

    const auto newProof = newTime.getProof();
    // Logical time is only sent if a server's clock can verify and sign logical times, so any
    // received logical times should have proofs.
    invariant(newProof);

    auto res = _timeProofService.checkProof(newTime.getTime(), newProof.get(), tempKey);
    if (res != Status::OK()) {
        return res;
    }

    return Status::OK();
}

void LogicalTimeValidator::updateCacheTrustedSource(const SignedLogicalTime& newTime) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (newTime.getTime() > _lastSeenValidTime.getTime()) {
        _lastSeenValidTime = newTime;
    }
}

}  // namespace mongo
