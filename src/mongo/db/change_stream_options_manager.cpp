/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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


#include "mongo/db/change_stream_options_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/change_stream_options_parameter_gen.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <variant>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
namespace {
const auto getChangeStreamOptionsManager =
    ServiceContext::declareDecoration<boost::optional<ChangeStreamOptionsManager>>();

}  // namespace

ChangeStreamOptionsManager& ChangeStreamOptionsManager::get(ServiceContext* service) {
    return *getChangeStreamOptionsManager(service);
}

ChangeStreamOptionsManager& ChangeStreamOptionsManager::get(OperationContext* opCtx) {
    return *getChangeStreamOptionsManager(opCtx->getServiceContext());
}

void ChangeStreamOptionsManager::create(ServiceContext* service) {
    getChangeStreamOptionsManager(service).emplace(service);
}

ChangeStreamOptions ChangeStreamOptionsManager::getOptions(OperationContext* opCtx) const {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    return _changeStreamOptions;
}

StatusWith<ChangeStreamOptions> ChangeStreamOptionsManager::setOptions(
    OperationContext* opCtx, ChangeStreamOptions optionsToSet) {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    _changeStreamOptions = std::move(optionsToSet);
    return _changeStreamOptions;
}

const LogicalTime& ChangeStreamOptionsManager::getClusterParameterTime() const {
    stdx::lock_guard<stdx::mutex> L(_mutex);
    return _changeStreamOptions.getClusterParameterTime();
}

void ChangeStreamOptionsParameter::append(OperationContext* opCtx,
                                          BSONObjBuilder* bob,
                                          StringData name,
                                          const boost::optional<TenantId>& tenantId) {
    ChangeStreamOptionsManager& changeStreamOptionsManager =
        ChangeStreamOptionsManager::get(getGlobalServiceContext());
    bob->append("_id"_sd, name);
    bob->appendElementsUnique(changeStreamOptionsManager.getOptions(opCtx).toBSON());
}

Status ChangeStreamOptionsParameter::set(const BSONElement& newValueElement,
                                         const boost::optional<TenantId>& tenantId) {
    try {
        ChangeStreamOptionsManager& changeStreamOptionsManager =
            ChangeStreamOptionsManager::get(getGlobalServiceContext());
        ChangeStreamOptions newOptions = ChangeStreamOptions::parse(
            newValueElement.Obj(), IDLParserContext("changeStreamOptions"));

        return changeStreamOptionsManager
            .setOptions(Client::getCurrent()->getOperationContext(), newOptions)
            .getStatus();
    } catch (const AssertionException&) {
        return {ErrorCodes::BadValue, "Could not parse changeStreamOptions parameter"};
    }
}

Status ChangeStreamOptionsParameter::validate(const BSONElement& newValueElement,
                                              const boost::optional<TenantId>& tenantId) const {
    try {
        BSONObj changeStreamOptionsObj = newValueElement.Obj();
        Status validateStatus = Status::OK();

        // PreAndPostImages currently contains a single field, `expireAfterSeconds`, that is
        // default- initialized to 'off'. This is useful for parameter initialization at startup but
        // causes the IDL parser to not enforce the presence of `expireAfterSeconds` in BSON
        // representations. We assert that and the existence of PreAndPostImages here.
        IDLParserContext ctxt = IDLParserContext("changeStreamOptions"_sd);
        if (auto preAndPostImagesObj = changeStreamOptionsObj["preAndPostImages"_sd];
            !preAndPostImagesObj.eoo()) {
            if (preAndPostImagesObj["expireAfterSeconds"_sd].eoo()) {
                ctxt.throwMissingField("expireAfterSeconds"_sd);
            }
        } else {
            ctxt.throwMissingField("preAndPostImages"_sd);
        }

        ChangeStreamOptions newOptions = ChangeStreamOptions::parse(changeStreamOptionsObj, ctxt);
        auto preAndPostImages = newOptions.getPreAndPostImages();
        visit(OverloadedVisitor{
                  [&](const std::string& expireAfterSeconds) {
                      if (expireAfterSeconds != "off"_sd) {
                          validateStatus = {
                              ErrorCodes::BadValue,
                              "Non-numeric value of 'expireAfterSeconds' should be 'off'"};
                      }
                  },
                  [&](const std::int64_t& expireAfterSeconds) {
                      if (change_stream_serverless_helpers::isServerlessEnvironment()) {
                          validateStatus = {
                              ErrorCodes::CommandNotSupported,
                              "The 'changeStreamOptions.preAndPostImages.expireAfterSeconds' is "
                              "unsupported in serverless, consider using "
                              "'changeStreams.expireAfterSeconds' instead."};
                          return;
                      }
                      if (expireAfterSeconds <= 0) {
                          validateStatus = {
                              ErrorCodes::BadValue,
                              "Numeric value of 'expireAfterSeconds' should be positive"};
                      }
                  },
              },
              preAndPostImages.getExpireAfterSeconds());

        return validateStatus;
    } catch (const AssertionException& ex) {
        return {ErrorCodes::BadValue,
                str::stream() << "Failed parsing new changeStreamOptions value" << ex.reason()};
    }
}

Status ChangeStreamOptionsParameter::reset(const boost::optional<TenantId>& tenantId) {
    // Replace the current changeStreamOptions with a default-constructed one, which should
    // automatically set preAndPostImages.expirationSeconds to 'off' by default.
    ChangeStreamOptionsManager& changeStreamOptionsManager =
        ChangeStreamOptionsManager::get(getGlobalServiceContext());
    return changeStreamOptionsManager
        .setOptions(Client::getCurrent()->getOperationContext(), ChangeStreamOptions())
        .getStatus();
}

LogicalTime ChangeStreamOptionsParameter::getClusterParameterTime(
    const boost::optional<TenantId>& tenantId) const {
    ChangeStreamOptionsManager& changeStreamOptionsManager =
        ChangeStreamOptionsManager::get(getGlobalServiceContext());
    return changeStreamOptionsManager.getClusterParameterTime();
}

}  // namespace mongo
