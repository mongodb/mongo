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

#include "mongo/db/initialize_api_parameters.h"

namespace mongo {

const APIParametersFromClient initializeAPIParameters(OperationContext* opCtx,
                                                      const BSONObj& requestBody,
                                                      Command* command) {
    auto apiParamsFromClient =
        APIParametersFromClient::parse("APIParametersFromClient"_sd, requestBody);

    if (gRequireApiVersion.load() && !opCtx->getClient()->isInDirectClient()) {
        uassert(
            498870,
            "The apiVersion parameter is required, please configure your MongoClient's API version",
            apiParamsFromClient.getApiVersion());
    }

    if (apiParamsFromClient.getApiDeprecationErrors() || apiParamsFromClient.getApiStrict()) {
        uassert(4886600,
                "Provided apiStrict and/or apiDeprecationErrors without passing apiVersion",
                apiParamsFromClient.getApiVersion());
    }

    if (apiParamsFromClient.getApiVersion()) {
        auto apiVersionFromClient = apiParamsFromClient.getApiVersion().value();
        if (apiVersionFromClient == "2") {
            uassert(ErrorCodes::APIVersionError, "Cannot accept API version 2", acceptAPIVersion2);
        } else {
            uassert(ErrorCodes::APIVersionError,
                    "API version must be \"1\"",
                    "1" == apiVersionFromClient);
        }
    }

    if (apiParamsFromClient.getApiStrict().get_value_or(false)) {
        auto cmdApiVersions = command->apiVersions();
        auto apiVersionFromClient = apiParamsFromClient.getApiVersion().value().toString();

        bool strictAssert = (cmdApiVersions.find(apiVersionFromClient) != cmdApiVersions.end());
        uassert(ErrorCodes::APIStrictError,
                str::stream() << "Provided apiStrict:true, but the command " << command->getName()
                              << " is not in API Version " << apiVersionFromClient,
                strictAssert);
    }

    if (apiParamsFromClient.getApiDeprecationErrors().get_value_or(false)) {
        auto cmdDepApiVersions = command->deprecatedApiVersions();
        auto apiVersionFromClient = apiParamsFromClient.getApiVersion().value().toString();

        bool deprecationAssert =
            (cmdDepApiVersions.find(apiVersionFromClient) == cmdDepApiVersions.end());
        uassert(ErrorCodes::APIDeprecationError,
                str::stream() << "Provided apiDeprecationErrors:true, but the command "
                              << command->getName() << " is deprecated in API Version "
                              << apiVersionFromClient,
                deprecationAssert);
    }

    return apiParamsFromClient;
}

const OperationContext::Decoration<APIParameters> handle =
    OperationContext::declareDecoration<APIParameters>();

APIParameters& APIParameters::get(OperationContext* opCtx) {
    return handle(opCtx);
}

APIParameters::APIParameters()
    : _apiVersion("1"), _apiStrict(false), _apiDeprecationErrors(false), _paramsPassed(false) {}

APIParameters APIParameters::fromClient(const APIParametersFromClient& apiParamsFromClient) {
    APIParameters apiParameters = APIParameters();
    auto apiVersion = apiParamsFromClient.getApiVersion();
    auto apiStrict = apiParamsFromClient.getApiStrict();
    auto apiDeprecationErrors = apiParamsFromClient.getApiDeprecationErrors();

    if (apiVersion) {
        apiParameters.setAPIVersion(apiVersion.value());
        apiParameters.setParamsPassed(true);
    }

    if (apiStrict) {
        apiParameters.setAPIStrict(apiStrict.value());
    }

    if (apiDeprecationErrors) {
        apiParameters.setAPIDeprecationErrors(apiDeprecationErrors.value());
    }

    return apiParameters;
}

void APIParameters::appendInfo(BSONObjBuilder* builder) const {
    builder->append(kAPIVersionFieldName, _apiVersion);
    builder->append(kAPIStrictFieldName, _apiStrict);
    builder->append(kAPIDeprecationErrorsFieldName, _apiDeprecationErrors);
}

}  // namespace mongo
