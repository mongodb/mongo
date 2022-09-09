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

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_coll_stats.h"
#include "mongo/db/pipeline/document_source_internal_all_collection_stats_gen.h"

namespace mongo {

/**
 * This aggregation stage is the ‘$_internalAllCollectionStats´. It takes no arguments. Its
 * response will be a cursor, each document of which represents the collection statistics for a
 * single collection for all the existing collections.
 *
 * When executing the '$_internalAllCollectionStats' aggregation stage, we will need to obtain the
 * catalog containing all collections namespaces.
 *
 * Then, for each collection, we will call `makeStatsForNs` method from DocumentSourceCollStats that
 * will retrieve all storage stats for that particular collection.
 */
class DocumentSourceInternalAllCollectionStats final : public DocumentSource {
public:
    static constexpr StringData kStageNameInternal = "$_internalAllCollectionStats"_sd;

    DocumentSourceInternalAllCollectionStats(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                             DocumentSourceInternalAllCollectionStatsSpec spec);

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& spec) {
            return std::make_unique<LiteParsed>(spec.fieldName());
        }

        explicit LiteParsed(std::string parseTimeName)
            : LiteParsedDocumentSource(std::move(parseTimeName)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final;

        bool isInitialSource() const final {
            return true;
        }
    };

    const char* getSourceName() const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final{};

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

    StageConstraints constraints(Pipeline::SplitState pipeState) const final {
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kAnyShard,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.isIndependentOfAnyCollection = true;
        constraints.requiresInputDocSource = false;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    static boost::intrusive_ptr<DocumentSource> createFromBsonInternal(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    GetNextResult doGetNext() final;

    // The specification object given to $_internalAllCollectionStats containing user specified
    // options.
    DocumentSourceInternalAllCollectionStatsSpec _internalAllCollectionStatsSpec;
    boost::optional<std::deque<BSONObj>> _catalogDocs;
};
}  // namespace mongo
