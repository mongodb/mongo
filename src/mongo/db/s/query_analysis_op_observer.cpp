/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/s/query_analysis_op_observer.h"

#include "mongo/bson/timestamp.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/s/query_analysis_coordinator.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/util/future.h"

#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace analyze_shard_key {

void QueryAnalysisOpObserver::insertInConfigQueryAnalyzersNamespaceImpl(
    OperationContext* opCtx,
    std::vector<InsertStatement>::const_iterator begin,
    std::vector<InsertStatement>::const_iterator end) {
    for (auto it = begin; it != end; ++it) {
        auto parsedDoc = QueryAnalyzerDocument::parse(
            it->doc, IDLParserContext("QueryAnalysisOpObserver::onInserts"));
        shard_role_details::getRecoveryUnit(opCtx)->onCommit(
            [parsedDoc = std::move(parsedDoc)](OperationContext* opCtx,
                                               boost::optional<Timestamp>) {
                analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationInsert(
                    parsedDoc);
            });
    }
}

void QueryAnalysisOpObserver::updateToConfigQueryAnalyzersNamespaceImpl(
    OperationContext* opCtx, const OplogUpdateEntryArgs& args) {
    auto parsedDoc = QueryAnalyzerDocument::parse(
        args.updateArgs->updatedDoc, IDLParserContext("QueryAnalysisOpObserver::onUpdate"));
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [parsedDoc = std::move(parsedDoc)](OperationContext* opCtx, boost::optional<Timestamp>) {
            analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationUpdate(
                parsedDoc);
        });
}

void QueryAnalysisOpObserver::updateWithSampleIdImpl(OperationContext* opCtx,
                                                     const OplogUpdateEntryArgs& args) {
    analyze_shard_key::QueryAnalysisWriter::get(opCtx)
        ->addDiff(*args.updateArgs->sampleId,
                  args.coll->ns(),
                  args.coll->uuid(),
                  args.updateArgs->preImageDoc,
                  args.updateArgs->updatedDoc)
        .getAsync([](auto) {});
}

void QueryAnalysisOpObserver::deleteFromConfigQueryAnalyzersNamespaceImpl(
    OperationContext* opCtx, const OplogDeleteEntryArgs& args, const BSONObj& doc) {
    auto parsedDoc =
        QueryAnalyzerDocument::parse(doc, IDLParserContext("QueryAnalysisOpObserver::onDelete"));
    shard_role_details::getRecoveryUnit(opCtx)->onCommit(
        [parsedDoc = std::move(parsedDoc)](OperationContext* opCtx, boost::optional<Timestamp>) {
            analyze_shard_key::QueryAnalysisCoordinator::get(opCtx)->onConfigurationDelete(
                parsedDoc);
        });
}

}  // namespace analyze_shard_key
}  // namespace mongo
