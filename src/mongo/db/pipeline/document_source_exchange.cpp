// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/pipeline/document_source_exchange.h"

#include <string_view>
#include <utility>
namespace mongo {

ALLOCATE_DOCUMENT_SOURCE_ID(exchange, DocumentSourceExchange::id)

std::string_view DocumentSourceExchange::getSourceName() const {
    return kStageName;
}

Value DocumentSourceExchange::serialize(const query_shape::SerializationOptions& opts) const {
    return Value(DOC(getSourceName() << _exchange->getSpec().toBSON(opts)));
}

DocumentSourceExchange::DocumentSourceExchange(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const boost::intrusive_ptr<exec::agg::Exchange> exchange,
    size_t consumerId,
    const std::shared_ptr<ResourceYielder>& yielder)
    : DocumentSource(kStageName, expCtx),
      _exchange(exchange),
      _consumerId(consumerId),
      _resourceYielder(yielder) {}

}  // namespace mongo
