/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/query/allowed_contexts.h"

#include "mongo/transport/session.h"

namespace mongo {

void assertLanguageFeatureIsAllowed(
    const OperationContext* opCtx,
    std::string operatorName,
    AllowedWithApiStrict allowedWithApiStrict,
    AllowedWithClientType allowedWithClientType,
    boost::optional<std::function<void(const APIParameters&)>> callbackForSometimesAllowed) {
    // An internal client could be one of the following :
    //     - Does not have any transport session
    //     - The transport session tag is internal
    auto client = opCtx->getClient();
    const auto isInternalClient = client &&
        (!client->session() ||
         (client->session()->getTags() & transport::Session::kInternalClient));

    const auto apiParameters = APIParameters::get(opCtx);

    uassert(5491300,
            str::stream() << operatorName << "' is not allowed in user requests",
            !(allowedWithClientType == AllowedWithClientType::kInternal && !isInternalClient));

    const auto apiVersion = apiParameters.getAPIVersion().value_or("");
    const auto apiStrict = apiParameters.getAPIStrict().value_or(false);
    if (!apiStrict) {
        return;
    }
    switch (allowedWithApiStrict) {
        case AllowedWithApiStrict::kNeverInVersion1: {
            uassert(ErrorCodes::APIStrictError,
                    str::stream() << operatorName
                                  << " is not allowed with 'apiStrict: true' in API Version "
                                  << apiVersion,
                    apiVersion != "1");
            break;
        }
        case AllowedWithApiStrict::kInternal: {
            uassert(ErrorCodes::APIStrictError,
                    str::stream() << operatorName
                                  << " cannot be specified with 'apiStrict: true' in API Version "
                                  << apiVersion,
                    isInternalClient);
            break;
        }
        case AllowedWithApiStrict::kSometimes: {
            if (auto callback = callbackForSometimesAllowed) {
                (*callback)(apiParameters);
            }
            break;
        }
        case AllowedWithApiStrict::kAlways: {
            break;
        }
    }
}
}  // namespace mongo
