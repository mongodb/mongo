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

#include "mongo/db/global_settings.h"

#include "mongo/db/client.h"
#include "mongo/db/mongod_options_general_gen.h"
#include "mongo/db/service_context.h"

namespace mongo {

MongodGlobalParams mongodGlobalParams;

namespace {
repl::ReplSettings globalReplSettings;

const auto getClusterNetworkRestrictionManager =
    ServiceContext::declareDecoration<std::unique_ptr<ClusterNetworkRestrictionManager>>();

Mutex mtxSetAllowListedCluster = MONGO_MAKE_LATCH("AllowListedClusterNetworkSetting::mutex");

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
                                              StringData name,
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
    } else if (e.type() == mongo::Array) {
        allowlistedClusterNetwork = std::make_shared<std::vector<std::string>>();
        for (const auto& sub : e.Array()) {
            if (sub.type() != mongo::String) {
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
        stdx::lock_guard<Mutex> guard(mtxSetAllowListedCluster);
        mongodGlobalParams.allowlistedClusterNetwork = allowlistedClusterNetwork;
        updater->updateClusterNetworkRestrictions();
    }

    return Status::OK();
}

Status AllowListedClusterNetworkSetting::setFromString(StringData s,
                                                       const boost::optional<TenantId>&) {
    return {ErrorCodes::InternalError, "Cannot invoke this method"};
}

}  // namespace mongo
