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
