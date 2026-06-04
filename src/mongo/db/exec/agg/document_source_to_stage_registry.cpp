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

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"


namespace mongo {
namespace exec {
namespace agg {

namespace {

stdx::unordered_map<DocumentSource::Id, DocumentSourceToStagesFn> stageBuildersMap;

}  // namespace

void registerDocumentSourceToStageFn(DocumentSource::Id dsid, DocumentSourceToStageFn fn) {
    registerDocumentSourceToStagesFn(
        dsid,
        [fn = std::move(fn)](const boost::intrusive_ptr<DocumentSource>& ds) -> StageExpansion {
            return {fn(ds)};
        });
}

void registerDocumentSourceToStagesFn(DocumentSource::Id dsid, DocumentSourceToStagesFn fn) {
    auto [_, inserted] = stageBuildersMap.insert({dsid, std::move(fn)});
    tassert(10395400, "duplicate DocumentSource to Stage mapping", inserted);
}

// Populate 'DocumentSource' to 'agg::Stage' mapping function registry after every 'DocumentSource'
// subclass got its unique 'Id' assigned.
MONGO_INITIALIZER_GROUP(BeginDocumentSourceStageRegistration, ("EndDocumentSourceIdAllocation"), ())
MONGO_INITIALIZER_GROUP(EndDocumentSourceStageRegistration,
                        ("BeginDocumentSourceStageRegistration"),
                        ())

StageExpansion buildStages(const boost::intrusive_ptr<DocumentSource>& ds) {
    auto it = stageBuildersMap.find(ds->getId());
    tassert(10395401,
            str::stream() << "missing 'DocumentSource' to 'agg::Stage' mapping function for "
                          << ds->getSourceName(),
            it != stageBuildersMap.end());
    auto expansion = (it->second)(ds);
    tassert(12634301,
            str::stream() << "stage expansion for " << ds->getSourceName()
                          << " must contain at least one stage",
            !expansion.empty());
    return expansion;
}

StagePtr buildStage(const boost::intrusive_ptr<DocumentSource>& ds) {
    auto expansion = buildStages(ds);
    tassert(10395402, "expected exactly one stage from 1:1 mapping", expansion.size() == 1);
    return std::move(expansion.front());
}

StagePtr buildStageAndStitch(const boost::intrusive_ptr<DocumentSource>& ds,
                             const StagePtr& sourceStage) {
    auto&& newStage = buildStage(ds);
    newStage->setSource(sourceStage.get());
    return newStage;
}

void stitchStage(Stage& stage, Stage* prior) {
    stage.setSource(prior);
}

}  // namespace agg
}  // namespace exec
}  // namespace mongo
