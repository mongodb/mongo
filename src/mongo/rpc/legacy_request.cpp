// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/rpc/legacy_request.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/rpc/metadata.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {
namespace rpc {

OpMsgRequest opMsgRequestFromLegacyRequest(const Message& message) {
    DbMessage dbm(message);
    QueryMessage qm(dbm);
    const auto ns =
        NamespaceStringUtil::deserialize(boost::none, qm.ns, SerializationContext::stateDefault());

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
