// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonelement.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

DECLARE_STAGE_PARAMS_DERIVED_DEFAULT(Documents);

namespace DocumentSourceDocuments {
using namespace std::literals::string_view_literals;
class LiteParsed : public LiteParsedDocumentSourceDefault<LiteParsed> {
public:
    static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                             const BSONElement& spec,
                                             const LiteParserOptions& options) {
        return std::make_unique<LiteParsed>(spec);
    }

    LiteParsed(const BSONElement& spec) : LiteParsedDocumentSourceDefault(spec) {}

    stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
        return stdx::unordered_set<NamespaceString>();
    }

    PrivilegeVector requiredPrivileges(bool isMongos, bool bypassDocumentValidation) const final {
        return {};
    }

    bool requiresAuthzChecks() const override {
        return false;
    }

    bool isInitialSource() const final {
        return true;
    }

    bool generatesOwnDataOnce() const final {
        return true;
    }

    std::unique_ptr<StageParams> getStageParams() const final {
        return std::make_unique<DocumentsStageParams>(_originalBson);
    }
};

const inline std::string kGenFieldName = "_tempDocumentsField";
constexpr inline std::string_view kStageName = "$documents"sv;

std::list<boost::intrusive_ptr<DocumentSource>> createFromBson(
    BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& pExpCtx);


/**
 * If the pipeline starts with a desugared $documents, returns stages representing the desugared
 * $documents.
 */
boost::optional<std::vector<BSONObj>> extractDesugaredStagesFromPipeline(
    const std::vector<BSONObj>& pipeline);

};  // namespace DocumentSourceDocuments

}  // namespace mongo
