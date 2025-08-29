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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_list_mql_entities_gen.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/pipeline_split_state.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/query_shape/serialization_options.h"
#include "mongo/stdx/unordered_set.h"

#include <memory>
#include <set>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Test-only aggregation stage which describes the set of MQL entities available in this binary,
 * which may vary depending on the binary (mongod/mongos, commmunity/enterprise). For the
 * DocumentSource MQL entity type, this stage returns only the set of stages which have a registered
 * parser and are thus user-facing via the 'aggregate' command; it does not include DocumentSources
 * which we create during desugaring or optimization. The order of results is guarenteed to be
 * sorted by name.
 */
class DocumentSourceListMqlEntities final : public DocumentSource {
public:
    class LiteParsed : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(specElem.fieldName());
        }

        LiteParsed(std::string parseTimeName)
            : LiteParsedDocumentSource(std::move(parseTimeName)) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        // No privileges required because this is a test-only stage.
        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return {};
        }

        bool requiresAuthzChecks() const override {
            return false;
        }

        bool isInitialSource() const final {
            return true;
        }
    };

    static constexpr StringData kStageName = "$listMqlEntities"_sd;
    static constexpr StringData kEntityTypeFieldName = "entityType"_sd;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    DocumentSourceListMqlEntities(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                  MqlEntityTypeEnum type);

    StageConstraints constraints(PipelineSplitState pipeState) const final;
    const char* getSourceName() const final;

    static const Id& id;

    Id getId() const override {
        return id;
    }

    MqlEntityTypeEnum getType() const {
        return _type;
    }

    DocumentSourceContainer::iterator doOptimizeAt(DocumentSourceContainer::iterator itr,
                                                   DocumentSourceContainer* container) final;
    Value serialize(const SerializationOptions& opts = SerializationOptions{}) const final;
    boost::optional<DistributedPlanLogic> distributedPlanLogic() final;
    void addVariableRefs(std::set<Variables::Id>* refs) const final {}

private:
    MqlEntityTypeEnum _type;
};

}  // namespace mongo
