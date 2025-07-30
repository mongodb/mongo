/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/stdx/unordered_set.h"

#include <list>
#include <memory>
#include <string>
#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace DocumentSourceDocuments {
class LiteParsed : public LiteParsedDocumentSource {
public:
    static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                             const BSONElement& spec,
                                             const LiteParserOptions& options) {
        return std::make_unique<LiteParsed>(spec.fieldName());
    }

    LiteParsed(std::string parseTimeName) : LiteParsedDocumentSource(std::move(parseTimeName)) {}

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
};

const inline std::string kGenFieldName = "_tempDocumentsField";
constexpr inline StringData kStageName = "$documents"_sd;

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
