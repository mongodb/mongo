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

#include "mongo/platform/basic.h"

#include <utility>

#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/rpc/legacy_request.h"
#include "mongo/rpc/metadata.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace rpc {

OpMsgRequest opMsgRequestFromLegacyRequest(const Message& message) {
    DbMessage dbm(message);
    QueryMessage qm(dbm);
    NamespaceString ns(qm.ns);

    if (qm.queryOptions & QueryOption_Exhaust) {
        uasserted(18527,
                  str::stream() << "The 'exhaust' OP_QUERY flag is invalid for commands: "
                                << ns.toStringForErrorMsg() << " " << qm.query.toString());
    }

    uassert(40473,
            str::stream() << "Trying to handle namespace " << qm.ns << " as a command",
            ns.isCommand());

    uassert(16979,
            str::stream() << "Bad numberToReturn (" << qm.ntoreturn
                          << ") for $cmd type ns - can only be 1 or -1",
            qm.ntoreturn == 1 || qm.ntoreturn == -1);

    return upconvertRequest(
        ns.dbName(), qm.query.shareOwnershipWith(message.sharedBuffer()), qm.queryOptions);
}

}  // namespace rpc
}  // namespace mongo
