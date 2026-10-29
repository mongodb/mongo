// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/change_stream_options_manager.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/change_stream_options_parameter_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {
using namespace std::literals::string_view_literals;
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
    std::lock_guard<std::mutex> L(_mutex);
    return _changeStreamOptions;
}

StatusWith<ChangeStreamOptions> ChangeStreamOptionsManager::setOptions(
    OperationContext* opCtx, ChangeStreamOptions optionsToSet) {
    std::lock_guard<std::mutex> L(_mutex);
    _changeStreamOptions = std::move(optionsToSet);
    return _changeStreamOptions;
}

const LogicalTime& ChangeStreamOptionsManager::getClusterParameterTime() const {
    std::lock_guard<std::mutex> L(_mutex);
    return _changeStreamOptions.getClusterParameterTime();
}

// The following methods are for the 'ChangeStreamOptionsParameter' server parameter type which is
// defined via IDL ('in file src/mongo/db/change_stream_options_parameter.idl'). The 'TenantId'
// parameters in the following methods do nothing, but they are required because the IDL generator
// generates method signatures including 'TenantId' parameters.
// TODO SERVER-103953: remove TenantId parameters from the signatures below.

void ChangeStreamOptionsParameter::append(OperationContext* opCtx,
                                          BSONObjBuilder* bob,
                                          std::string_view name,
                                          const boost::optional<TenantId>&) {
    ChangeStreamOptionsManager& changeStreamOptionsManager =
        ChangeStreamOptionsManager::get(getGlobalServiceContext());
    bob->append("_id"sv, name);
    bob->appendElementsUnique(changeStreamOptionsManager.getOptions(opCtx).toBSON());
}

Status ChangeStreamOptionsParameter::set(const BSONElement& newValueElement,
                                         const boost::optional<TenantId>&) {
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
                                              const boost::optional<TenantId>&) const {
    try {
        BSONObj changeStreamOptionsObj = newValueElement.Obj();
        Status validateStatus = Status::OK();

        // PreAndPostImages currently contains a single field, `expireAfterSeconds`, that is
        // default- initialized to 'off'. This is useful for parameter initialization at startup but
        // causes the IDL parser to not enforce the presence of `expireAfterSeconds` in BSON
        // representations. We assert that and the existence of PreAndPostImages here.
        IDLParserContext ctxt = IDLParserContext("changeStreamOptions"sv);
        if (auto preAndPostImagesObj = changeStreamOptionsObj["preAndPostImages"sv];
            !preAndPostImagesObj.eoo()) {
            if (preAndPostImagesObj["expireAfterSeconds"sv].eoo()) {
                ctxt.throwMissingField("expireAfterSeconds"sv);
            }
        } else {
            ctxt.throwMissingField("preAndPostImages"sv);
        }

        ChangeStreamOptions newOptions = ChangeStreamOptions::parse(changeStreamOptionsObj, ctxt);
        auto preAndPostImages = newOptions.getPreAndPostImages();
        visit(OverloadedVisitor{
                  [&](const std::string& expireAfterSeconds) {
                      if (expireAfterSeconds != "off"sv) {
                          validateStatus = {
                              ErrorCodes::BadValue,
                              "Non-numeric value of 'expireAfterSeconds' should be 'off'"};
                      }
                  },
                  [&](const std::int64_t& expireAfterSeconds) {
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

Status ChangeStreamOptionsParameter::reset(const boost::optional<TenantId>&) {
    // Replace the current changeStreamOptions with a default-constructed one, which should
    // automatically set preAndPostImages.expirationSeconds to 'off' by default.
    ChangeStreamOptionsManager& changeStreamOptionsManager =
        ChangeStreamOptionsManager::get(getGlobalServiceContext());
    return changeStreamOptionsManager
        .setOptions(Client::getCurrent()->getOperationContext(), ChangeStreamOptions())
        .getStatus();
}

LogicalTime ChangeStreamOptionsParameter::getClusterParameterTime(
    const boost::optional<TenantId>&) const {
    ChangeStreamOptionsManager& changeStreamOptionsManager =
        ChangeStreamOptionsManager::get(getGlobalServiceContext());
    return changeStreamOptionsManager.getClusterParameterTime();
}

}  // namespace mongo
