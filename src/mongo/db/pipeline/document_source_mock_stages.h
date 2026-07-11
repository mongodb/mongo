// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/pipeline/document_source_mock.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/util/modules.h"

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

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
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

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) override {
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
