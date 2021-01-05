/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_operation_metrics.h"

#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/server_options.h"
#include "mongo/db/stats/operation_resource_consumption_gen.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

using boost::intrusive_ptr;

REGISTER_DOCUMENT_SOURCE(operationMetrics,
                         DocumentSourceOperationMetrics::LiteParsed::parse,
                         DocumentSourceOperationMetrics::createFromBson);

const char* DocumentSourceOperationMetrics::getSourceName() const {
    return kStageName.rawData();
}

namespace {
static constexpr StringData kClearMetrics = "clearMetrics"_sd;
static constexpr StringData kDatabaseName = "db"_sd;
}  // namespace

DocumentSource::GetNextResult DocumentSourceOperationMetrics::doGetNext() {
    if (_operationMetrics.empty()) {
        auto dbMetrics = [&]() {
            if (_clearMetrics) {
                return ResourceConsumption::get(pExpCtx->opCtx).getAndClearDbMetrics();
            }
            return ResourceConsumption::get(pExpCtx->opCtx).getDbMetrics();
        }();
        for (auto& [dbName, metrics] : dbMetrics) {
            BSONObjBuilder builder;
            builder.append(kDatabaseName, dbName);
            metrics.toBson(&builder);
            _operationMetrics.push_back(builder.obj());
        }

        _operationMetricsIter = _operationMetrics.begin();
    }

    if (_operationMetricsIter != _operationMetrics.end()) {
        auto doc = Document(std::move(*_operationMetricsIter));
        _operationMetricsIter++;
        return doc;
    }

    return GetNextResult::makeEOF();
}

intrusive_ptr<DocumentSource> DocumentSourceOperationMetrics::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {
    if (!ResourceConsumption::isMetricsAggregationEnabled()) {
        uasserted(ErrorCodes::CommandNotSupported,
                  "The aggregateOperationResourceConsumption server parameter is not set");
    }

    const NamespaceString& nss = pExpCtx->ns;
    uassert(ErrorCodes::InvalidNamespace,
            "$operationMetrics must be run against the 'admin' database with {aggregate: 1}",
            nss.db() == NamespaceString::kAdminDb && nss.isCollectionlessAggregateNS());

    uassert(ErrorCodes::BadValue,
            "The $operationMetrics stage specification must be an object",
            elem.type() == Object);

    auto stageObj = elem.Obj();
    bool clearMetrics = false;
    if (auto clearElem = stageObj.getField(kClearMetrics); !clearElem.eoo()) {
        clearMetrics = clearElem.trueValue();
    } else if (!stageObj.isEmpty()) {
        uasserted(
            ErrorCodes::BadValue,
            "The $operationMetrics stage specification must be empty or contain valid options");
    }
    return new DocumentSourceOperationMetrics(pExpCtx, clearMetrics);
}

Value DocumentSourceOperationMetrics::serialize(
    boost::optional<ExplainOptions::Verbosity> explain) const {
    return Value(DOC(getSourceName() << Document()));
}
}  // namespace mongo
