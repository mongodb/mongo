/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/operation_key_manager.h"

#include <fmt/format.h>

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {
auto getOpKeyManager = ServiceContext::declareDecoration<OperationKeyManager>();
}

OperationKeyManager::~OperationKeyManager() {
    invariant(_idByOperationKey.empty(),
              "Every associated OperationContext with an OperationKey must be destroyed before an "
              "OperationKeyManager can be destroyed.");
}

OperationKeyManager& OperationKeyManager::get(ServiceContext* serviceContext) {
    return getOpKeyManager(serviceContext);
}

void OperationKeyManager::add(const OperationKey& key, OperationId id) {
    using namespace fmt::literals;

    LOGV2_DEBUG(4615636,
                2,
                "Mapping OperationKey {operationKey} to OperationId {operationId}",
                "operationKey"_attr = key.toString(),
                "operationId"_attr = id);

    stdx::lock_guard lk(_mutex);
    auto result = _idByOperationKey.emplace(key, id).second;

    uassert(
        ErrorCodes::BadValue, "OperationKey currently '{}' in use"_format(key.toString()), result);
}

bool OperationKeyManager::remove(const OperationKey& key) {
    stdx::lock_guard lk(_mutex);
    return _idByOperationKey.erase(key);
}

boost::optional<OperationId> OperationKeyManager::at(const OperationKey& key) const {
    stdx::lock_guard lk(_mutex);
    auto it = _idByOperationKey.find(key);
    if (it == _idByOperationKey.end()) {
        return boost::none;
    }

    return it->second;
}

size_t OperationKeyManager::size() const {
    stdx::lock_guard lk(_mutex);
    return _idByOperationKey.size();
}

}  // namespace mongo
