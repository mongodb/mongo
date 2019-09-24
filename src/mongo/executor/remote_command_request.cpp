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

#include "mongo/platform/basic.h"

#include "mongo/executor/remote_command_request.h"

#include <fmt/format.h>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/str.h"

using namespace fmt::literals;

namespace mongo {
namespace executor {
namespace {

// Used to generate unique identifiers for requests so they can be traced throughout the
// asynchronous networking logs
AtomicWord<unsigned long long> requestIdCounter(0);

}  // namespace

constexpr Milliseconds RemoteCommandRequestBase::kNoTimeout;

constexpr Date_t RemoteCommandRequestBase::kNoExpirationDate;

RemoteCommandRequestBase::RemoteCommandRequestBase(RequestId requestId,
                                                   const std::string& theDbName,
                                                   const BSONObj& theCmdObj,
                                                   const BSONObj& metadataObj,
                                                   OperationContext* opCtx,
                                                   Milliseconds timeoutMillis)
    : id(requestId), dbname(theDbName), metadata(metadataObj), opCtx(opCtx) {
    // If there is a comment associated with the current operation, append it to the command that we
    // are about to dispatch to the shards.
    cmdObj = opCtx && opCtx->getComment() && !theCmdObj["comment"]
        ? theCmdObj.addField(*opCtx->getComment())
        : theCmdObj;
    timeout = opCtx ? std::min<Milliseconds>(opCtx->getRemainingMaxTimeMillis(), timeoutMillis)
                    : timeoutMillis;
}

RemoteCommandRequestBase::RemoteCommandRequestBase() : id(requestIdCounter.addAndFetch(1)) {}

template <typename T>
RemoteCommandRequestImpl<T>::RemoteCommandRequestImpl() = default;

template <typename T>
RemoteCommandRequestImpl<T>::RemoteCommandRequestImpl(RequestId requestId,
                                                      const T& theTarget,
                                                      const std::string& theDbName,
                                                      const BSONObj& theCmdObj,
                                                      const BSONObj& metadataObj,
                                                      OperationContext* opCtx,
                                                      Milliseconds timeoutMillis)
    : RemoteCommandRequestBase(requestId, theDbName, theCmdObj, metadataObj, opCtx, timeoutMillis),
      target(theTarget) {
    if constexpr (std::is_same_v<T, std::vector<HostAndPort>>) {
        invariant(!theTarget.empty());
    }
}

template <typename T>
RemoteCommandRequestImpl<T>::RemoteCommandRequestImpl(const T& theTarget,
                                                      const std::string& theDbName,
                                                      const BSONObj& theCmdObj,
                                                      const BSONObj& metadataObj,
                                                      OperationContext* opCtx,
                                                      Milliseconds timeoutMillis)
    : RemoteCommandRequestImpl(requestIdCounter.addAndFetch(1),
                               theTarget,
                               theDbName,
                               theCmdObj,
                               metadataObj,
                               opCtx,
                               timeoutMillis) {}

template <typename T>
std::string RemoteCommandRequestImpl<T>::toString() const {
    str::stream out;
    out << "RemoteCommand " << id << " -- target:";
    if constexpr (std::is_same_v<HostAndPort, T>) {
        out << target.toString();
    } else {
        out << "[{}]"_format(fmt::join(target, ", "));
    }
    out << " db:" << dbname;

    if (expirationDate != kNoExpirationDate) {
        out << " expDate:" << expirationDate.toString();
    }

    out << " cmd:" << cmdObj.toString();
    return out;
}

template <typename T>
bool RemoteCommandRequestImpl<T>::operator==(const RemoteCommandRequestImpl& rhs) const {
    if (this == &rhs) {
        return true;
    }
    return target == rhs.target && dbname == rhs.dbname &&
        SimpleBSONObjComparator::kInstance.evaluate(cmdObj == rhs.cmdObj) &&
        SimpleBSONObjComparator::kInstance.evaluate(metadata == rhs.metadata) &&
        timeout == rhs.timeout;
}

template <typename T>
bool RemoteCommandRequestImpl<T>::operator!=(const RemoteCommandRequestImpl& rhs) const {
    return !(*this == rhs);
}

template struct RemoteCommandRequestImpl<HostAndPort>;
template struct RemoteCommandRequestImpl<std::vector<HostAndPort>>;

}  // namespace executor
}  // namespace mongo
