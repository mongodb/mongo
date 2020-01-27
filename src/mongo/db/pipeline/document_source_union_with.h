/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include <boost/optional.hpp>

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

namespace mongo {

class DocumentSourceUnionWith final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$unionWith"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec);

        LiteParsed(NamespaceString withNss,
                   stdx::unordered_set<NamespaceString> foreignNssSet,
                   boost::optional<LiteParsedPipeline> liteParsedPipeline)
            : _withNss{std::move(withNss)},
              _foreignNssSet(std::move(foreignNssSet)),
              _liteParsedPipeline(std::move(liteParsedPipeline)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return {_foreignNssSet};
        }

        PrivilegeVector requiredPrivileges(bool, bool) const final {
            return {};
        }

        bool allowShardedForeignCollection(NamespaceString) const final {
            return true;
        }

        bool allowedToPassthroughFromMongos() const final {
            return true;
        }

    private:
        const NamespaceString _withNss;
        const stdx::unordered_set<NamespaceString> _foreignNssSet;
        const boost::optional<LiteParsedPipeline> _liteParsedPipeline;
    };

    DocumentSourceUnionWith(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                            std::unique_ptr<Pipeline, PipelineDeleter> pipeline)
        : DocumentSource(kStageName, expCtx), _pipeline(std::move(pipeline)) {}

    const char* getSourceName() const final {
        return kStageName.rawData();
    }

    void serializeToArray(
        std::vector<Value>& array,
        boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    GetModPathsReturn getModifiedPaths() const final;

    StageConstraints constraints(Pipeline::SplitState) const final {
        return StageConstraints(StreamType::kStreaming,
                                PositionRequirement::kNone,
                                HostTypeRequirement::kNone,
                                DiskUseRequirement::kNoDiskUse,
                                FacetRequirement::kAllowed,
                                TransactionRequirement::kNotAllowed,
                                LookupRequirement::kNotAllowed);
    }

    DepsTracker::State getDependencies(DepsTracker* deps) const final;

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        // {shardsStage, mergingStage, sortPattern}
        return DistributedPlanLogic{nullptr, this, boost::none};
    }

    void addInvolvedCollections(stdx::unordered_set<NamespaceString>* collectionNames) const final;

    void detachFromOperationContext() final;

    void reattachToOperationContext(OperationContext* opCtx) final;

protected:
    GetNextResult doGetNext() final;
    void doDispose() final;

private:
    /**
     * Should not be called; use serializeToArray instead.
     */
    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
    bool _sourceExhausted = false;
    bool _cursorAttached = false;
};

}  // namespace mongo
