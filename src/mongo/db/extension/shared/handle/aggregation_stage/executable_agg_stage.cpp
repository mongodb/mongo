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
#include "mongo/db/extension/shared/handle/aggregation_stage/executable_agg_stage.h"

#include "mongo/db/extension/shared/explain_utils.h"
#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension {

void ExecAggStageHandle::setSource(const ExecAggStageHandle& input) {
    assertValid();
    invokeCAndConvertStatusToException([&]() { return vtable().set_source(get(), input.get()); });
}

namespace {
ExtensionGetNextResult _internalGetNext(const MongoExtensionExecAggStageVTable& vtable,
                                        MongoExtensionExecAggStage* stage,
                                        MongoExtensionQueryExecutionContext* execCtxPtr) {
    ::MongoExtensionGetNextResult result = createDefaultExtensionGetNext();
    invokeCAndConvertStatusToException(
        [&]() { return vtable.get_next(stage, execCtxPtr, &result); });

    return ExtensionGetNextResult::makeFromApiResult(result);
}

std::string_view _internalGetName(const MongoExtensionExecAggStageVTable& vtable,
                                  const MongoExtensionExecAggStage* stage) {
    return byteViewAsStringView(vtable.get_name(stage));
}

host_connector::HostOperationMetricsHandle _internalCreateMetrics(
    const MongoExtensionExecAggStageVTable& vtable, const MongoExtensionExecAggStage* stage) {
    MongoExtensionOperationMetrics* metrics = nullptr;
    invokeCAndConvertStatusToException([&]() { return vtable.create_metrics(stage, &metrics); });

    tassert(11213505, "Result of `create_metrics` was nullptr", metrics != nullptr);

    // Take ownership of the created metrics and return the result.
    return host_connector::HostOperationMetricsHandle(metrics);
}
}  // namespace

ExtensionGetNextResult ExecAggStageHandle::getNext(
    MongoExtensionQueryExecutionContext* execCtxPtr) {
    assertValid();
    return _internalGetNext(vtable(), get(), execCtxPtr);
}

std::string_view ExecAggStageHandle::getName() const {
    assertValid();
    return _internalGetName(vtable(), get());
}

host_connector::HostOperationMetricsHandle ExecAggStageHandle::createMetrics() const {
    assertValid();
    return _internalCreateMetrics(vtable(), get());
}

std::string_view UnownedExecAggStageHandle::getName() const {
    assertValid();
    return _internalGetName(vtable(), get());
}

host_connector::HostOperationMetricsHandle UnownedExecAggStageHandle::createMetrics() const {
    assertValid();
    return _internalCreateMetrics(vtable(), get());
}

ExtensionGetNextResult UnownedExecAggStageHandle::getNext(
    MongoExtensionQueryExecutionContext* execCtxPtr) {
    assertValid();
    return _internalGetNext(vtable(), get(), execCtxPtr);
}

void ExecAggStageHandle::open() {
    assertValid();
    invokeCAndConvertStatusToException([&]() { return vtable().open(get()); });
}

void ExecAggStageHandle::reopen() {
    assertValid();
    invokeCAndConvertStatusToException([&]() { return vtable().reopen(get()); });
}

void ExecAggStageHandle::close() {
    assertValid();
    invokeCAndConvertStatusToException([&]() { return vtable().close(get()); });
}

BSONObj ExecAggStageHandle::explain(mongo::ExplainOptions::Verbosity verbosity) const {
    assertValid();
    ::MongoExtensionByteBuf* buf;
    auto extVerbosity = convertHostVerbosityToExtVerbosity(verbosity);
    invokeCAndConvertStatusToException(
        [&]() { return vtable().explain(get(), extVerbosity, &buf); });

    tassert(11239500, "buffer returned from explain must not be null", buf);

    // Take ownership of the returned buffer so that it gets cleaned up, then retrieve an owned
    // BSONObj to return to the host.
    // TODO: SERVER-112442 Avoid the BSON copy in getOwned() once the work is completed.
    ExtensionByteBufHandle ownedBuf{buf};
    return bsonObjFromByteView(ownedBuf.getByteView()).getOwned();
}

}  // namespace mongo::extension
