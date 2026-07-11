// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/change_stream_reader_builder.h"

#include "mongo/util/decorable.h"

namespace mongo {

namespace {
const auto getChangeStreamReaderBuilderFromServiceContext =
    ServiceContext::declareDecoration<std::unique_ptr<ChangeStreamReaderBuilder>>();

}  // namespace

ChangeStreamReaderBuilder* ChangeStreamReaderBuilder::get(ServiceContext* serviceContext) {
    return getChangeStreamReaderBuilderFromServiceContext(serviceContext).get();
}

void ChangeStreamReaderBuilder::set(
    ServiceContext* serviceContext,
    std::unique_ptr<ChangeStreamReaderBuilder> changeStreamReaderBuilder) {
    auto& holder = getChangeStreamReaderBuilderFromServiceContext(serviceContext);
    holder = std::move(changeStreamReaderBuilder);
}

std::unique_ptr<ChangeStreamReaderBuilder> ChangeStreamReaderBuilder::swap_forTest(
    ServiceContext* serviceContext, std::unique_ptr<ChangeStreamReaderBuilder> replacement) {
    auto& holder = getChangeStreamReaderBuilderFromServiceContext(serviceContext);
    auto previous = std::move(holder);
    holder = std::move(replacement);
    return previous;
}

}  // namespace mongo
