// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/operation_key_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"

#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


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

    LOGV2_DEBUG(4615636,
                2,
                "Mapping OperationKey {operationKey} to OperationId {operationId}",
                "operationKey"_attr = key.toString(),
                "operationId"_attr = id);

    std::lock_guard lk(_mutex);
    auto result = _idByOperationKey.emplace(key, id).second;

    uassert(ErrorCodes::BadValue,
            fmt::format("OperationKey currently '{}' in use", key.toString()),
            result);
}

bool OperationKeyManager::remove(const OperationKey& key) {
    std::lock_guard lk(_mutex);
    return _idByOperationKey.erase(key);
}

boost::optional<OperationId> OperationKeyManager::at(const OperationKey& key) const {
    std::lock_guard lk(_mutex);
    auto it = _idByOperationKey.find(key);
    if (it == _idByOperationKey.end()) {
        return boost::none;
    }

    return it->second;
}

size_t OperationKeyManager::size() const {
    std::lock_guard lk(_mutex);
    return _idByOperationKey.size();
}

}  // namespace mongo
