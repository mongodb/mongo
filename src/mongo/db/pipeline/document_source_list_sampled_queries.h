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

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/multitenancy_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_sampled_queries_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/s/analyze_shard_key_documents_gen.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"


namespace mongo {
namespace analyze_shard_key {

class DocumentSourceListSampledQueries final : public DocumentSource {
public:
    static constexpr StringData kStageName = "$listSampledQueries"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem);

        explicit LiteParsed(std::string parseTimeName,
                            NamespaceString nss,
                            DocumentSourceListSampledQueriesSpec spec)
            : LiteParsedDocumentSource(std::move(parseTimeName)),
              _nss(std::move(nss)),
              _privileges({Privilege(ResourcePattern::forClusterResource(_nss.tenantId()),
                                     ActionType::listSampledQueries)}) {}

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const override {
            return _privileges;
        }

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const override {
            return stdx::unordered_set<NamespaceString>();
        }

        bool isInitialSource() const final {
            return true;
        }

        void assertSupportsMultiDocumentTransaction() const override {
            transactionNotSupported(kStageName);
        }

    private:
        const NamespaceString _nss;
        const PrivilegeVector _privileges;
    };

    DocumentSourceListSampledQueries(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                                     DocumentSourceListSampledQueriesSpec spec)
        : DocumentSource(kStageName, pExpCtx), _spec(std::move(spec)) {}

    ~DocumentSourceListSampledQueries() override = default;

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

    static const Id& id;

    Id getId() const override {
        return id;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

    void detachFromOperationContext() final;
    void reattachToOperationContext(OperationContext* opCtx) final;

private:
    DocumentSourceListSampledQueries(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : DocumentSource(kStageName, expCtx) {}

    GetNextResult doGetNext() final;

    DocumentSourceListSampledQueriesSpec _spec;
    std::unique_ptr<Pipeline, PipelineDeleter> _pipeline;
};

}  // namespace analyze_shard_key
}  // namespace mongo
