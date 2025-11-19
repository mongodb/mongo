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

#include "mongo/db/extension/shared/extension_status.h"

namespace mongo::extension {

ExtensionGetNextResult ExecAggStageHandle::getNext(MongoExtensionQueryExecutionContext* execCtxPtr,
                                                   ::MongoExtensionGetNextRequestType requestType) {
    ::MongoExtensionGetNextResult result{};
    invokeCAndConvertStatusToException(
        [&]() { return vtable().get_next(get(), execCtxPtr, &result); });

    return convertCRepresentationToGetNextResult(&result);
}

namespace {
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

std::string_view ExecAggStageHandle::getName() const {
    return _internalGetName(vtable(), get());
}

host_connector::HostOperationMetricsHandle ExecAggStageHandle::createMetrics() const {
    return _internalCreateMetrics(vtable(), get());
}

std::string_view UnownedExecAggStageHandle::getName() const {
    return _internalGetName(vtable(), get());
}

host_connector::HostOperationMetricsHandle UnownedExecAggStageHandle::createMetrics() const {
    return _internalCreateMetrics(vtable(), get());
}

void ExecAggStageHandle::open() {
    invokeCAndConvertStatusToException([&]() { return vtable().open(get()); });
}

void ExecAggStageHandle::reopen() {
    invokeCAndConvertStatusToException([&]() { return vtable().reopen(get()); });
}

void ExecAggStageHandle::close() {
    invokeCAndConvertStatusToException([&]() { return vtable().close(get()); });
}

}  // namespace mongo::extension
