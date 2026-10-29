// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/write_ops/parsed_delete.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/matcher/extensions_callback.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/delete_request_gen.h"
#include "mongo/db/query/write_ops/parsed_writes_common.h"
#include "mongo/util/assert_util.h"

#include <memory>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kWrite


namespace mongo {

PlanYieldPolicy::YieldPolicy getDeleteYieldPolicy(const DeleteRequest* request) {
    return request->getGod() ? PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY
                             : request->getYieldPolicy();
}

const DeleteRequest* ParsedDelete::getRequest() const {
    return request;
}

bool ParsedDelete::hasParsedFindCommand() const {
    return parsedFind.get() != nullptr;
}

namespace parsed_delete_command {

StatusWith<ParsedDelete> parse(boost::intrusive_ptr<ExpressionContext> expCtx,
                               const DeleteRequest* request,
                               std::unique_ptr<const ExtensionsCallback> extensionsCallback) {
    ParsedDelete out;
    out.request = request;
    out.extensionsCallback = std::move(extensionsCallback);

    tassert(11052001,
            "Cannot request DeleteStage to return the deleted document during a multi-delete",
            !(request->getReturnDeleted() && request->getMulti()));

    tassert(11052002,
            "Cannot apply projection to DeleteStage if the DeleteStage would not return the "
            "deleted document",
            request->getProj().isEmpty() || request->getReturnDeleted());

    expCtx->startExpressionCounters();


    if (isSimpleIdQuery(request->getQuery())) {
        return out;
    }

    auto swParsedFind =
        impl::parseWriteQueryToParsedFindCommand(expCtx.get(), *out.extensionsCallback, *request);
    if (!swParsedFind.isOK()) {
        return swParsedFind.getStatus();
    }
    out.parsedFind = std::move(swParsedFind.getValue());

    return out;
}
}  // namespace parsed_delete_command

}  // namespace mongo
