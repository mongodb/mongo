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

#pragma once

#include <boost/optional.hpp>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/auth/security_token_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"

namespace mongo {
namespace auth {

class SecurityTokenAuthenticationGuard {
public:
    SecurityTokenAuthenticationGuard() = delete;
    SecurityTokenAuthenticationGuard(OperationContext* opCtx);
    ~SecurityTokenAuthenticationGuard();

private:
    Client* _client;
};

/**
 * Takes an unsigned security token as input and applies
 * the temporary signature algorithm to extend it into a full SecurityToken.
 */
BSONObj signSecurityToken(BSONObj obj);

/**
 * Verify the contents of the provided security token
 * using the temporary signing algorithm,
 */
SecurityToken verifySecurityToken(BSONObj obj);

/**
 * Parse any SecurityToken from the OpMsg and place it as a decoration
 * on OperationContext
 */
void readSecurityTokenMetadata(OperationContext* opCtx, BSONObj securityToken);

/**
 * Retrieve the Security Token associated with this operation context
 */
using MaybeSecurityToken = boost::optional<SecurityToken>;
MaybeSecurityToken getSecurityToken(OperationContext* opCtx);

}  // namespace auth
}  // namespace mongo
