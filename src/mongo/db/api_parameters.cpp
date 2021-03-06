/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/api_parameters.h"

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
        APIParametersFromClient::parse("APIParametersFromClient"_sd, cmdObj));
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
