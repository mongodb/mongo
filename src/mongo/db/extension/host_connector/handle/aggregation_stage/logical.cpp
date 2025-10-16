/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/extension/host_connector/handle/aggregation_stage/logical.h"

#include "mongo/db/extension/shared/byte_buf.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

namespace {
::MongoExtensionExplainVerbosity convertHostVerbosityToExtVerbosity(
    mongo::ExplainOptions::Verbosity hostVerbosity) {
    switch (hostVerbosity) {
        case mongo::ExplainOptions::Verbosity::kQueryPlanner:
            return ::MongoExtensionExplainVerbosity::kQueryPlanner;
        case mongo::ExplainOptions::Verbosity::kExecStats:
            return ::MongoExtensionExplainVerbosity::kExecStats;
        case mongo::ExplainOptions::Verbosity::kExecAllPlans:
            return ::MongoExtensionExplainVerbosity::kExecAllPlans;
        default:
            MONGO_UNREACHABLE_TASSERT(11239404);
    }
}
}  // namespace

namespace mongo::extension::host_connector {

BSONObj LogicalAggStageHandle::serialize() const {
    ::MongoExtensionByteBuf* buf;
    invokeCAndConvertStatusToException([&]() { return vtable().serialize(get(), &buf); });

    tassert(11173700,
            "Extension implementation of `serialize` encountered nullptr inside the output buffer.",
            buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the caller.
    // TODO: SERVER-112442 Avoid the BSON copy in getOwned() once the work is completed.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf.getByteView()).getOwned();
}

BSONObj LogicalAggStageHandle::explain(mongo::ExplainOptions::Verbosity verbosity) const {
    ::MongoExtensionByteBuf* buf;
    auto extVerbosity = convertHostVerbosityToExtVerbosity(verbosity);
    invokeCAndConvertStatusToException(
        [&]() { return vtable().explain(get(), extVerbosity, &buf); });

    tassert(11239400, "buffer returned from explain must not be null", buf);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    // TODO: SERVER-112442 Avoid the BSON copy in getOwned() once the work is completed.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf.getByteView()).getOwned();
}

}  // namespace mongo::extension::host_connector
