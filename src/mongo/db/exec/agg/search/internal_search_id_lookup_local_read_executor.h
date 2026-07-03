/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/single_doc_lookup/single_document_lookup_executor.h"
#include "mongo/db/pipeline/search/document_source_internal_search_id_lookup.h"
#include "mongo/db/query/plan_summary_stats.h"

#include <vector>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::exec::agg {

/**
 * idLookup's original lookup strategy, now behind SingleDocumentLookupExecutor: resolves an _id via
 * a `$match`-on-_id sub-pipeline (optionally + the view pipeline) against the stage's stashed
 * acquisition, whose shard filter drops orphans on sharded collections. Installed when the feature
 * flag is off or a view is present; always handles the lookup (never kNotHandled).
 */
class InternalSearchIdLookUpLocalReadExecutor final : public SingleDocumentLookupExecutor {
public:
    InternalSearchIdLookUpLocalReadExecutor(
        boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle> catalogResourceHandle,
        boost::optional<std::vector<BSONObj>> viewPipeline)
        : _catalogResourceHandle(std::move(catalogResourceHandle)),
          _viewPipeline(std::move(viewPipeline)) {}

    LookupResult performLookup(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               boost::optional<UUID> collectionUUID,
                               const Document& documentKey,
                               boost::optional<Timestamp> afterClusterTime) override;

    void setPlanSummaryStatsSink(PlanSummaryStats* sink) override {
        _planSummaryStatsSink = sink;
    }

private:
    boost::intrusive_ptr<DSInternalSearchIdLookUpCatalogResourceHandle> _catalogResourceHandle;
    boost::optional<std::vector<BSONObj>> _viewPipeline;

    // Non-owning; owned by the stage. Stats are accumulated here when set.
    PlanSummaryStats* _planSummaryStatsSink = nullptr;
};

}  // namespace mongo::exec::agg
