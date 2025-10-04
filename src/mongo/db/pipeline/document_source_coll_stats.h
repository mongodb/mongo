/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/auth/resource_pattern.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_coll_stats_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Provides a document source interface to retrieve collection-level statistics for a given
 * collection.
 */
class DocumentSourceCollStats : public DocumentSource {
public:
    static constexpr StringData kStageName = "$collStats"_sd;

    class LiteParsed final : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options) {
            uassert(5447000,
                    str::stream() << "$collStats must take a nested object but found: " << specElem,
                    specElem.type() == BSONType::object);
            auto spec = DocumentSourceCollStatsSpec::parse(specElem.embeddedObject(),
                                                           IDLParserContext(kStageName));
            return std::make_unique<LiteParsed>(specElem.fieldName(), nss, std::move(spec));
        }

        explicit LiteParsed(std::string parseTimeName,
                            NamespaceString nss,
                            DocumentSourceCollStatsSpec spec)
            : LiteParsedDocumentSource(std::move(parseTimeName)),
              _nss(std::move(nss)),
              _spec(std::move(spec)) {}

        bool isCollStats() const final {
            return true;
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {Privilege(ResourcePattern::forExactNamespace(_nss), ActionType::collStats)};
        }

        void assertPermittedInAPIVersion(const APIParameters&) const override;

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        bool isInitialSource() const final {
            return true;
        }

    private:
        const NamespaceString _nss;
        const DocumentSourceCollStatsSpec _spec;
    };

    DocumentSourceCollStats(const boost::intrusive_ptr<ExpressionContext>& pExpCtx,
                            DocumentSourceCollStatsSpec spec)
        : DocumentSource(kStageName, pExpCtx),
          _collStatsSpec(std::move(spec)),
          _targetAllNodes(_collStatsSpec.getTargetAllNodes().value_or(false)) {}

    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    StageConstraints constraints(PipelineSplitState pipeState) const final {
        HostTypeRequirement hostTypeRequirement =
            _targetAllNodes ? HostTypeRequirement::kAllShardHosts : HostTypeRequirement::kAnyShard;
        StageConstraints constraints(StreamType::kStreaming,
                                     PositionRequirement::kFirst,
                                     hostTypeRequirement,
                                     DiskUseRequirement::kNoDiskUse,
                                     FacetRequirement::kNotAllowed,
                                     TransactionRequirement::kNotAllowed,
                                     LookupRequirement::kAllowed,
                                     UnionRequirement::kAllowed);

        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    boost::optional<DistributedPlanLogic> distributedPlanLogic() final {
        return boost::none;
    }

    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;

    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);

private:
    friend boost::intrusive_ptr<exec::agg::Stage> documentSourceCollStatsToStageFn(
        const boost::intrusive_ptr<DocumentSource>& documentSource);

    // The raw object given to $collStats containing user specified options.
    DocumentSourceCollStatsSpec _collStatsSpec;
    bool _targetAllNodes;
};

}  // namespace mongo
