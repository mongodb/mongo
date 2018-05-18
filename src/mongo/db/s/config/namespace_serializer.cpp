/**
 *    Copyright (C) 2018 MongoDB Inc.
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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault
#include "mongo/platform/basic.h"

#include "mongo/db/s/config/namespace_serializer.h"

#include <map>
#include <memory>
#include <string>

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/config/sharding_catalog_manager.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

NamespaceSerializer::NamespaceSerializer() {}

NamespaceSerializer::ScopedLock::ScopedLock(StringData ns, NamespaceSerializer& nsSerializer)
    : _ns(ns.toString()), _nsSerializer(nsSerializer) {}

NamespaceSerializer::ScopedLock::~ScopedLock() {
    stdx::unique_lock<stdx::mutex> lock(_nsSerializer._mutex);
    auto iter = _nsSerializer._inProgressMap.find(_ns);

    iter->second->numWaiting--;
    iter->second->isInProgress = false;
    iter->second->cvLocked.notify_one();

    if (iter->second->numWaiting == 0) {
        _nsSerializer._inProgressMap.erase(_ns);
    }
}

NamespaceSerializer::ScopedLock NamespaceSerializer::lock(OperationContext* opCtx, StringData nss) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto iter = _inProgressMap.find(nss);

    if (iter == _inProgressMap.end()) {
        _inProgressMap.try_emplace(nss, std::make_shared<NSLock>());
    } else {
        auto& nsLock = iter->second;
        nsLock->numWaiting++;
        opCtx->waitForConditionOrInterrupt(
            nsLock->cvLocked, lock, [&nsLock]() { return !nsLock->isInProgress; });
        nsLock->isInProgress = true;
    }

    return ScopedLock(nss, *this);
}

}  // namespace mongo
