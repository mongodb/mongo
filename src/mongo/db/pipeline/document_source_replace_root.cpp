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

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_replace_root.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source_replace_root_gen.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;

/**
 * This class implements the transformation logic for the $replaceRoot and $replaceWith stages.
 */
class ReplaceRootTransformation final : public TransformerInterface {

public:
    ReplaceRootTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              boost::intrusive_ptr<Expression> newRootExpression)
        : _expCtx(expCtx), _newRoot(std::move(newRootExpression)) {}

    TransformerType getType() const final {
        return TransformerType::kReplaceRoot;
    }

    Document applyTransformation(const Document& input) final {
        // Extract subdocument in the form of a Value.
        Value newRoot = _newRoot->evaluate(input);

        // The newRoot expression, if it exists, must evaluate to an object.
        uassert(40228,
                str::stream()
                    << "'newRoot' expression must evaluate to an object, but resulting value was: "
                    << newRoot.toString()
                    << ". Type of resulting value: '"
                    << typeName(newRoot.getType())
                    << "'. Input document: "
                    << input.toString(),
                newRoot.getType() == BSONType::Object);

        // Turn the value into a document.
        MutableDocument newDoc(newRoot.getDocument());
        newDoc.copyMetaDataFrom(input);
        return newDoc.freeze();
    }

    // Optimize the newRoot expression.
    void optimize() final {
        _newRoot->optimize();
    }

    Document serializeTransformation(
        boost::optional<ExplainOptions::Verbosity> explain) const final {
        return Document{{"newRoot", _newRoot->serialize(static_cast<bool>(explain))}};
    }

    DepsTracker::State addDependencies(DepsTracker* deps) const final {
        _newRoot->addDependencies(deps);
        // This stage will replace the entire document with a new document, so any existing fields
        // will be replaced and cannot be required as dependencies.
        return DepsTracker::State::EXHAUSTIVE_FIELDS;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        // Replaces the entire root, so all paths are modified.
        return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<std::string>{}, {}};
    }

private:
    const boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::intrusive_ptr<Expression> _newRoot;
};

REGISTER_DOCUMENT_SOURCE(replaceRoot,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceReplaceRoot::createFromBson);
REGISTER_DOCUMENT_SOURCE(replaceWith,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceReplaceRoot::createFromBson);

intrusive_ptr<DocumentSource> DocumentSourceReplaceRoot::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& expCtx) {
    const auto stageName = elem.fieldNameStringData();
    auto newRootExpression = [&]() {
        if (stageName == kAliasNameReplaceWith) {
            return Expression::parseOperand(expCtx, elem, expCtx->variablesParseState);
        }

        invariant(
            stageName == kStageName,
            str::stream() << "Unexpected stage registered with DocumentSourceReplaceRoot parser: "
                          << stageName);
        uassert(40229,
                str::stream() << "expected an object as specification for " << kStageName
                              << " stage, got "
                              << typeName(elem.type()),
                elem.type() == Object);

        auto spec =
            ReplaceRootSpec::parse(IDLParserErrorContext(kStageName), elem.embeddedObject());

        // The IDL doesn't give us back the type we need to feed into the expression parser, and
        // the expression parser needs the extra state in 'vps' and 'expCtx', so for now we have
        // to adapt the two.
        BSONObj parsingBson = BSON("newRoot" << spec.getNewRoot());
        return Expression::parseOperand(
            expCtx, parsingBson.firstElement(), expCtx->variablesParseState);
    }();

    // Whether this was specified as $replaceWith or $replaceRoot, always use the name $replaceRoot
    // to simplify the serialization process.
    const bool isIndependentOfAnyCollection = false;
    return new DocumentSourceSingleDocumentTransformation(
        expCtx,
        std::make_unique<ReplaceRootTransformation>(expCtx, newRootExpression),
        kStageName.toString(),
        isIndependentOfAnyCollection);
}

}  // namespace mongo
