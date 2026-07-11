// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(ListSearchIndexes);

class DocumentSourceListSearchIndexesSpec;

class DocumentSourceListSearchIndexes final : public DocumentSource {
public:
    static constexpr std::string_view kStageName = "$listSearchIndexes"sv;
    static constexpr std::string_view kCursorFieldName = "cursor"sv;
    static constexpr std::string_view kFirstBatchFieldName = "firstBatch"sv;

    /**
     * A 'LiteParsed' representation of the $listSearchIndexes stage.
     */
    class LiteParsedListSearchIndexes final
        : public LiteParsedDocumentSourceDefault<LiteParsedListSearchIndexes> {
    public:
        static std::unique_ptr<LiteParsedListSearchIndexes> parse(
            const NamespaceString& nss, const BSONElement& spec, const LiteParserOptions& options) {
            return std::make_unique<LiteParsedListSearchIndexes>(spec, nss);
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            // There are no foreign namespaces.
            return stdx::unordered_set<NamespaceString>{};
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {Privilege(ResourcePattern::forDatabaseName(_nss.dbName()),
                              ActionType::listSearchIndexes)};
        }

        bool isInitialSource() const final {
            return true;
        }

        ReadConcernSupportResult supportsReadConcern(repl::ReadConcernLevel level,
                                                     bool isImplicitDefault) const override {
            return onlyReadConcernLocalSupported(getParseTimeName(), level, isImplicitDefault);
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(getParseTimeName());
        }

        bool isSearchStage() const final {
            return true;
        }

        std::unique_ptr<StageParams> getStageParams() const final {
            return std::make_unique<ListSearchIndexesStageParams>(_originalBson);
        }

        LiteParsedListSearchIndexes(const BSONElement& spec, NamespaceString nss)
            : LiteParsedDocumentSourceDefault(spec), _nss(std::move(nss)) {}

    private:
        const NamespaceString _nss;
    };

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    DocumentSourceListSearchIndexes(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                    BSONObj cmdObj)
        : DocumentSource(kStageName, pExpCtx), _cmdObj(cmdObj.getOwned()) {}

    std::string_view getSourceName() const override {
        return kStageName;
    }

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const query_shape::SerializationOptions& opts =
                        query_shape::SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    boost::optional<DistributedPlanLogic> distributedPlanLogic(
        const DistributedPlanContext* ctx) final {
        return boost::none;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final;

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceListSearchIndexesToStageFn(
        const boost::intrusive_ptr<DocumentSource>&);

    BSONObj _cmdObj;
};

}  // namespace mongo
