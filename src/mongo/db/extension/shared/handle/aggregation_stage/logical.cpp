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

#include "mongo/db/extension/shared/handle/aggregation_stage/logical.h"

#include "mongo/db/extension/shared/explain_utils.h"
#include "mongo/db/extension/shared/extension_status.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/distributed_plan_logic.h"
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"
#include "mongo/db/extension/shared/handle/byte_buf_handle.h"

namespace mongo::extension {

StringData LogicalAggStageAPI::getName() const {
    auto stringView = byteViewAsStringView(vtable().get_name(get()));
    return StringData{stringView.data(), stringView.size()};
}

BSONObj LogicalAggStageAPI::serialize() const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() { return vtable().serialize(get(), &buf); });

    tassert(11173700,
            "Extension implementation of `serialize` encountered nullptr inside the output buffer.",
            buf != nullptr);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the caller.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

BSONObj LogicalAggStageAPI::explain(mongo::ExplainOptions::Verbosity verbosity) const {
    ::MongoExtensionByteBuf* buf{nullptr};
    invokeCAndConvertStatusToException([&]() {
        return vtable().explain(get(), convertHostVerbosityToExtVerbosity(verbosity), &buf);
    });

    tassert(11239400, "buffer returned from explain must not be null", buf);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf->getByteView()).getOwned();
}

ExecAggStageHandle LogicalAggStageAPI::compile() const {
    ::MongoExtensionExecAggStage* execAggStage{nullptr};
    invokeCAndConvertStatusToException([&]() { return vtable().compile(get(), &execAggStage); });

    return ExecAggStageHandle(execAggStage);
}

DistributedPlanLogicHandle LogicalAggStageAPI::getDistributedPlanLogic() const {
    ::MongoExtensionDistributedPlanLogic* dpl{nullptr};
    invokeCAndConvertStatusToException(
        [&]() { return vtable().get_distributed_plan_logic(get(), &dpl); });

    return DistributedPlanLogicHandle(dpl);
}

}  // namespace mongo::extension
