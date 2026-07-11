// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/global_settings.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/client.h"
#include "mongo/db/mongod_options_general_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/decorable.h"

#include <mutex>
#include <string_view>
#include <utility>

#include <boost/optional/optional.hpp>

namespace mongo {

MongodGlobalParams mongodGlobalParams;

namespace {
repl::ReplSettings globalReplSettings;

const auto getClusterNetworkRestrictionManager =
    ServiceContext::declareDecoration<std::unique_ptr<ClusterNetworkRestrictionManager>>();

std::mutex mtxSetAllowListedCluster;

}  // namespace

void setGlobalReplSettings(const repl::ReplSettings& settings) {
    globalReplSettings = settings;
}

const repl::ReplSettings& getGlobalReplSettings() {
    return globalReplSettings;
}

void ClusterNetworkRestrictionManager::set(
    ServiceContext* service, std::unique_ptr<ClusterNetworkRestrictionManager> manager) {
    getClusterNetworkRestrictionManager(service) = std::move(manager);
}

void AllowListedClusterNetworkSetting::append(OperationContext*,
                                              BSONObjBuilder* b,
                                              std::string_view name,
                                              const boost::optional<TenantId>&) {
    auto allowlistedClusterNetwork =
        std::atomic_load(&mongodGlobalParams.allowlistedClusterNetwork);  // NOLINT
    if (allowlistedClusterNetwork) {
        BSONArrayBuilder bb(b->subarrayStart(name));
        for (const auto& acn : *allowlistedClusterNetwork) {
            bb << acn;
        }
        bb.doneFast();
    } else {
        *b << name << BSONNULL;
    }
}

Status AllowListedClusterNetworkSetting::set(const mongo::BSONElement& e,
                                             const boost::optional<TenantId>&) {
    std::shared_ptr<std::vector<std::string>> allowlistedClusterNetwork;
    if (e.isNull()) {
        // noop
    } else if (e.type() == BSONType::array) {
        allowlistedClusterNetwork = std::make_shared<std::vector<std::string>>();
        for (const auto& sub : e.Array()) {
            if (sub.type() != BSONType::string) {
                return {ErrorCodes::BadValue, "Expected array of strings"};
            }
            allowlistedClusterNetwork->push_back(sub.str());
        }
    } else {
        return {ErrorCodes::BadValue, "Expected array or null"};
    }

    const auto service = Client::getCurrent()->getServiceContext();
    const auto updater = getClusterNetworkRestrictionManager(service).get();
    if (updater) {
        std::lock_guard<std::mutex> guard(mtxSetAllowListedCluster);
        std::atomic_store(&mongodGlobalParams.allowlistedClusterNetwork,
                          std::move(allowlistedClusterNetwork));
        updater->updateClusterNetworkRestrictions();
    }

    return Status::OK();
}

Status AllowListedClusterNetworkSetting::setFromString(std::string_view s,
                                                       const boost::optional<TenantId>&) {
    return {ErrorCodes::InternalError, "Cannot invoke this method"};
}

}  // namespace mongo
