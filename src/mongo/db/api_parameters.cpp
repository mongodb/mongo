// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/api_parameters.h"

#include "mongo/idl/idl_parser.h"

#include <utility>

#include <boost/functional/hash.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand


namespace mongo {

const OperationContext::Decoration<APIParameters> APIParameters::get =
    OperationContext::declareDecoration<APIParameters>();

APIParameters APIParameters::fromClient(const APIParametersFromClient& apiParamsFromClient) {
    APIParameters apiParameters = APIParameters();
    auto apiVersion = apiParamsFromClient.getApiVersion();
    auto apiStrict = apiParamsFromClient.getApiStrict();
    auto apiDeprecationErrors = apiParamsFromClient.getApiDeprecationErrors();

    if (apiVersion) {
        apiParameters.setAPIVersion(apiVersion.value());
    }

    if (apiStrict) {
        apiParameters.setAPIStrict(apiStrict.value());
    }

    if (apiDeprecationErrors) {
        apiParameters.setAPIDeprecationErrors(apiDeprecationErrors.value());
    }

    return apiParameters;
}

APIParameters APIParameters::fromBSON(const BSONObj& cmdObj) {
    return APIParameters::fromClient(
        APIParametersFromClient::parse(cmdObj, IDLParserContext{"APIParametersFromClient"}));
}

void APIParameters::appendInfo(BSONObjBuilder* builder) const {
    if (_apiVersion) {
        builder->append(kAPIVersionFieldName, *_apiVersion);
    }
    if (_apiStrict) {
        builder->append(kAPIStrictFieldName, *_apiStrict);
    }
    if (_apiDeprecationErrors) {
        builder->append(kAPIDeprecationErrorsFieldName, *_apiDeprecationErrors);
    }
}

BSONObj APIParameters::toBSON() const {
    BSONObjBuilder bob;
    appendInfo(&bob);
    return bob.obj();
}

std::size_t APIParameters::Hash::operator()(const APIParameters& params) const {
    size_t seed = 0;
    if (params.getAPIVersion()) {
        boost::hash_combine(seed, *params.getAPIVersion());
    }
    if (params.getAPIStrict()) {
        boost::hash_combine(seed, *params.getAPIStrict());
    }
    if (params.getAPIDeprecationErrors()) {
        boost::hash_combine(seed, *params.getAPIDeprecationErrors());
    }
    return seed;
}

}  // namespace mongo
