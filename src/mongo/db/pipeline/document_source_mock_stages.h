/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo {
// Like a DocumentSourceMock, but has a deferrable merge sort.
class DocumentSourceDeferredMergeSort : public DocumentSourceMock {
public:
    DocumentSourceDeferredMergeSort(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    static boost::intrusive_ptr<DocumentSourceDeferredMergeSort> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceDeferredMergeSort(expCtx);
    }

    static bool canMovePastDuringSplit(const DocumentSource& ds) {
        return ds.constraints().preservesOrderAndMetadata;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        DistributedPlanLogic logic;

        logic.shardsStage = this;
        logic.mergingStages = {};
        logic.mergeSortPattern = BSON("a" << 1);
        logic.needsSplit = false;
        logic.canMovePast = canMovePastDuringSplit;

        return logic;
    }
};

// Like a DocumentSourceMock, but must run on router and can be used anywhere in the pipeline.
class DocumentSourceMustRunOnRouter : public DocumentSourceMock {
public:
    DocumentSourceMustRunOnRouter(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        // Overrides DocumentSourceMock's required position.
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kRouter,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kNotAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kNotAllowed,
                UnionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceMustRunOnRouter> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceMustRunOnRouter(expCtx);
    }
};

class DocumentSourceCanSwapWithSort : public DocumentSourceMock {
public:
    DocumentSourceCanSwapWithSort(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    static boost::intrusive_ptr<DocumentSourceCanSwapWithSort> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceCanSwapWithSort(expCtx);
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() override {
        return boost::none;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const override {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kNone,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed,
                                     ChangeStreamRequirement::kDenylist);
        constraints.preservesOrderAndMetadata = true;

        return constraints;
    }
};

class DocumentSourceCollectionlessMock : public DocumentSourceMock {
public:
    DocumentSourceCollectionlessMock(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);
        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    static boost::intrusive_ptr<DocumentSourceCollectionlessMock> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceCollectionlessMock(expCtx);
    }
};

class DocumentSourceDisallowedInTransactions : public DocumentSourceMock {
public:
    DocumentSourceDisallowedInTransactions(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        return StageConstraints{StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kAllowed,
                                UnionRequirement::kAllowed};
    }

    static boost::intrusive_ptr<DocumentSourceDisallowedInTransactions> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceDisallowedInTransactions(expCtx);
    }
};

//
// Some dummy DocumentSources with different dependencies.
//

// Like a DocumentSourceMock, but can be used anywhere in the pipeline.
class DocumentSourceDependencyDummy : public DocumentSourceMock {
public:
    DocumentSourceDependencyDummy(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceMock({}, expCtx) {}

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        // Overrides DocumentSourceMock's required position.
        return {StreamType::kStreaming,
                PositionRequirement::kNone,
                HostTypeRequirement::kNone,
                DiskUseRequirement::kNoDiskUse,
                FacetRequirement::kAllowed,
                TransactionRequirement::kAllowed,
                LookupRequirement::kAllowed,
                UnionRequirement::kAllowed};
    }
};

class DocumentSourceDependenciesNotSupported : public DocumentSourceDependencyDummy {
public:
    DocumentSourceDependenciesNotSupported(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::NOT_SUPPORTED;
    }

    static boost::intrusive_ptr<DocumentSourceDependenciesNotSupported> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceDependenciesNotSupported(expCtx);
    }
};

class DocumentSourceNeedsASeeNext : public DocumentSourceDependencyDummy {
public:
    DocumentSourceNeedsASeeNext(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("a");
        return DepsTracker::State::SEE_NEXT;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsASeeNext> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsASeeNext(expCtx);
    }
};

class DocumentSourceNeedsOnlyB : public DocumentSourceDependencyDummy {
public:
    DocumentSourceNeedsOnlyB(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->fields.insert("b");
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsOnlyB> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceNeedsOnlyB(expCtx);
    }
};

class DocumentSourceNeedsMetaField : public DocumentSourceDependencyDummy {
public:
    DocumentSourceNeedsMetaField(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                 const DocumentMetadataFields::MetaType type)
        : DocumentSourceDependencyDummy(expCtx), _type(type) {}

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->setNeedsMetadata(_type);
        return DepsTracker::State::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceNeedsMetaField> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentMetadataFields::MetaType type) {
        return new DocumentSourceNeedsMetaField(expCtx, type);
    }

private:
    const DocumentMetadataFields::MetaType _type;
};

class DocumentSourceGeneratesMetaField : public DocumentSourceDependencyDummy {
public:
    DocumentSourceGeneratesMetaField(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                     const DocumentMetadataFields::MetaType type)
        : DocumentSourceDependencyDummy(expCtx), _type(type) {}

    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        deps->setMetadataAvailable(_type);
        return DepsTracker::State::SEE_NEXT;
    }

    static boost::intrusive_ptr<DocumentSourceGeneratesMetaField> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const DocumentMetadataFields::MetaType type) {
        return new DocumentSourceGeneratesMetaField(expCtx, type);
    }

private:
    const DocumentMetadataFields::MetaType _type;
};

class DocumentSourceStripsMetadata : public DocumentSourceDependencyDummy {
public:
    DocumentSourceStripsMetadata(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSourceDependencyDummy(expCtx) {}
    DepsTracker::State getDependencies(DepsTracker* deps) const final {
        return DepsTracker::State::EXHAUSTIVE_META;
    }

    static boost::intrusive_ptr<DocumentSourceStripsMetadata> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx) {
        return new DocumentSourceStripsMetadata(expCtx);
    }
};

}  // namespace mongo
