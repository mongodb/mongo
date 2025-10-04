/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/stage_constraints.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/stdx/unordered_set.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Document source for the $listExtensions stage, implemented as a wrapper of DocumentSourceQueue.
 */
class DocumentSourceListExtensions final {
public:
    static constexpr StringData kStageName = "$listExtensions"_sd;

    class LiteParsed : public LiteParsedDocumentSource {
    public:
        static std::unique_ptr<LiteParsed> parse(const NamespaceString& nss,
                                                 const BSONElement& specElem,
                                                 const LiteParserOptions& options) {
            return std::make_unique<LiteParsed>(specElem.fieldName());
        }

        LiteParsed(std::string parseTimeName)
            : LiteParsedDocumentSource(std::move(parseTimeName)),
              _privileges({Privilege(ResourcePattern::forClusterResource(boost::none),
                                     ActionType::listExtensions)}) {}

        stdx::unordered_set<NamespaceString> getInvolvedNamespaces() const final {
            return stdx::unordered_set<NamespaceString>();
        }

        PrivilegeVector requiredPrivileges(bool isMongos,
                                           bool bypassDocumentValidation) const final {
            return _privileges;
        }

        bool isInitialSource() const final {
            return true;
        }

    private:
        const PrivilegeVector _privileges;
    };

    /**
     * Returns the stage constraints used to override DocumentSourceQueue.
     */
    static StageConstraints constraints() {
        auto constraints = StageConstraints{DocumentSource::StreamType::kStreaming,
                                            DocumentSource::PositionRequirement::kFirst,
                                            DocumentSource::HostTypeRequirement::kLocalOnly,
                                            DocumentSource::DiskUseRequirement::kNoDiskUse,
                                            DocumentSource::FacetRequirement::kNotAllowed,
                                            DocumentSource::TransactionRequirement::kNotAllowed,
                                            DocumentSource::LookupRequirement::kNotAllowed,
                                            DocumentSource::UnionRequirement::kNotAllowed};
        constraints.isIndependentOfAnyCollection = true;
        constraints.setConstraintsForNoInputSources();
        return constraints;
    }

    /**
     * Creates a DocumentSourceQueue using a DeferredQueue which lazily computes the loaded
     * extensions during the first getNext() call.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);
};

}  // namespace mongo
