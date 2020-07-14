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

#pragma once

#include "mongo/db/initialize_api_parameters_gen.h"
#include "mongo/db/operation_context.h"

namespace mongo {

/**
 * Parses a command's API Version parameters from a request and stores the apiVersion, apiStrict,
 * and apiDeprecationErrors fields.
 */
const APIParametersFromClient initializeAPIParameters(const BSONObj& requestBody);

/**
 * Decorates operation context with methods to retrieve apiVersion, apiStrict, and
 * apiDeprecationErrors.
 */
class APIParameters {

public:
    APIParameters();
    static APIParameters& get(OperationContext* opCtx);
    static APIParameters fromClient(const APIParametersFromClient& apiParamsFromClient);

    const StringData getAPIVersion() const {
        return _apiVersion;
    }

    void setAPIVersion(StringData apiVersion) {
        _apiVersion = apiVersion;
    }

    const bool getAPIStrict() const {
        return _apiStrict;
    }

    void setAPIStrict(bool apiStrict) {
        _apiStrict = apiStrict;
    }

    const bool getAPIDeprecationErrors() const {
        return _apiDeprecationErrors;
    }

    void setAPIDeprecationErrors(bool apiDeprecationErrors) {
        _apiDeprecationErrors = apiDeprecationErrors;
    }

private:
    StringData _apiVersion;
    bool _apiStrict;
    bool _apiDeprecationErrors;
};

}  // namespace mongo