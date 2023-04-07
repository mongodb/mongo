/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/dbdirectclient.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_sampled_queries_gen.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/s/analyze_shard_key_util.h"
#include "mongo/s/is_mongos.h"


namespace mongo {
namespace analyze_shard_key {

class DocumentSourceListSampledQueries final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$listSampledQueries"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem) {
            uassert(6876000,
                    str::stream() << kStageName
                                  << " must take a nested object but found: " << specElem,
                    specElem.type() == BSONType::Object);
            uassert(
                ErrorCodes::IllegalOperation,
                str::stream() << kStageName << " is not supported on a standalone mongod",
                isMongos() ||
                    repl::ReplicationCoordinator::get(getGlobalServiceContext())->isReplEnabled());
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << kStageName << " is not supported on a multitenant replica set",
                    !gMultitenancySupport);
            uassert(ErrorCodes::IllegalOperation,
                    str::stream() << kStageName << " is not supported on a configsvr mongod",
                    !serverGlobalParams.clusterRole.exclusivelyHasConfigRole());

            auto spec = DocumentSourceListSampledQueriesSpec::parse(IDLParserContext(kStageName),
                                                                    specElem.embeddedObject());
            if (spec.getNamespace()) {
                uassertStatusOK(validateNamespace(*spec.getNamespace()));
            }
            return std::make_unique<LiteParsed>(specElem.fieldName(), nss, std::move(spec));
        }

        explicit LiteParsed(std::string parseTimeName,
                            NamespaceString nss,
                            DocumentSourceListSampledQueriesSpec spec)
            : LiteParsedDocumentSource(std::move(parseTimeName)), _nss(std::move(nss)) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return {
                Privilege(ResourcePattern::forClusterResource(), ActionType::listSampledQueries)};
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        bool isInitialSource() const final {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const {
            transactionNotSupported(kStageName);
        }

    private:
        const NamespaceString _nss;
    };

    DocumentSourceListSampledQueries(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     DocumentSourceListSampledQueriesSpec spec)
        : DocumentSource(kStageName, pExpCtx), _spec(std::move(spec)) {}

    virtual ~DocumentSourceListSampledQueries() = default;

    StageConstraints constraints(
        Pipeline::SplitState = Pipeline::SplitState::kUnsplit) const override {
        StageConstraints constraints{StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     HostTypeRequirement::kNone,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed};

        constraints.requiresInputDocSource = false;
        constraints.isIndependentOfAnyCollection = true;
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    const char* getSourceName() const override {
        return kStageName.rawData();
    }

    Value serialize(SerializationOptions opts = SerializationOptions()) const final override;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    DocumentSourceListSampledQueries(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    GetNextResult doGetNext() final;

    DocumentSourceListSampledQueriesSpec _spec;
    bool _finished = false;
    std::unique_ptr<DBClientCursor> _cursor;
};

}  // namespace analyze_shard_key
}  // namespace mongo
