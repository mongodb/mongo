// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/allowed_contexts.h"

#include "mongo/base/error_codes.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/str.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {

void assertLanguageFeatureIsAllowed(
    const OperationContext* opCtx,
    std::string_view operatorName,
    AllowedWithApiStrict allowedWithApiStrict,
    AllowedWithClientType allowedWithClientType,
    boost::optional<std::function<void(const APIParameters&)>> conditionalCallback) {

    assertAllowedInternalIfRequired(opCtx, operatorName, allowedWithClientType);

    const auto apiParameters = APIParameters::get(opCtx);
    const auto isInternal = isInternalClient(opCtx->getClient());
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
                    isInternal);
            break;
        }
        case AllowedWithApiStrict::kConditionally: {
            if (conditionalCallback) {
                (*conditionalCallback)(apiParameters);
            }
            break;
        }
        case AllowedWithApiStrict::kAlways: {
            break;
        }
    }
}

/**
 * An internal client could be one of the following :
 *     - Does not have any transport session
 *     - The transport session tag is internal
 */
bool isInternalClient(Client* client) {
    return client && (!client->session() || client->isInternalClient());
}

/**
 * If the AllowedWithClientType requires that the session be internal assert that it is.
 */
void assertAllowedInternalIfRequired(const OperationContext* opCtx,
                                     std::string_view operatorName,
                                     AllowedWithClientType allowedWithClientType) {
    const auto isInternal = isInternalClient(opCtx->getClient());

    uassert(5491300,
            str::stream() << operatorName << " is not allowed in user requests",
            !(allowedWithClientType == AllowedWithClientType::kInternal && !isInternal));
}
}  // namespace mongo
