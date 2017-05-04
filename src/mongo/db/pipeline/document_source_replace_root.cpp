/**
 * Copyright 2016 (c) 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects for
 * all of the code used other than as permitted herein. If you modify file(s)
 * with this exception, you may extend this exception to your version of the
 * file(s), but you are not obligated to do so. If you do not wish to do so,
 * delete this exception statement from your version. If you delete this
 * exception statement from all source files in the program, then also delete
 * it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/document_source_replace_root.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;

/**
 * This class implements the transformation logic for the $replaceRoot stage.
 */
class ReplaceRootTransformation final
    : public DocumentSourceSingleDocumentTransformation::TransformerInterface {

public:
    ReplaceRootTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx)
        : _expCtx(expCtx) {}

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
                newRoot.getType() == Object);

        // Turn the value into a document.
        return newRoot.getDocument();
    }

    // Optimize the newRoot expression.
    void optimize() final {
        _newRoot->optimize();
    }

    Document serialize(boost::optional<ExplainOptions::Verbosity> explain) const final {
        return Document{{"newRoot", _newRoot->serialize(static_cast<bool>(explain))}};
    }

    DocumentSource::GetDepsReturn addDependencies(DepsTracker* deps) const final {
        _newRoot->addDependencies(deps);
        // This stage will replace the entire document with a new document, so any existing fields
        // will be replaced and cannot be required as dependencies.
        return DocumentSource::EXHAUSTIVE_FIELDS;
    }

    DocumentSource::GetModPathsReturn getModifiedPaths() const final {
        // Replaces the entire root, so all paths are modified.
        return {DocumentSource::GetModPathsReturn::Type::kAllPaths, std::set<std::string>{}, {}};
    }

    // Create the replaceRoot transformer. Uasserts on invalid input.
    static std::unique_ptr<ReplaceRootTransformation> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONElement& spec) {

        // Confirm that the stage was called with an object.
        uassert(40229,
                str::stream() << "expected an object as specification for $replaceRoot stage, got "
                              << typeName(spec.type()),
                spec.type() == Object);

        // Create the pointer, parse the stage, and return.
        std::unique_ptr<ReplaceRootTransformation> parsedReplaceRoot =
            stdx::make_unique<ReplaceRootTransformation>(expCtx);
        parsedReplaceRoot->parse(expCtx, spec);
        return parsedReplaceRoot;
    }

    // Check for valid replaceRoot options, and populate internal state variables.
    void parse(const boost::intrusive_ptr<ExpressionContext>& expCtx, const BSONElement& spec) {
        // We need a VariablesParseState in order to parse the 'newRoot' expression.
        VariablesParseState vps = expCtx->variablesParseState;

        // Get the options from this stage. Currently the only option is newRoot.
        for (auto&& argument : spec.Obj()) {
            const StringData argName = argument.fieldNameStringData();

            if (argName == "newRoot") {
                // Allows for field path, object, and other expressions.
                _newRoot = Expression::parseOperand(expCtx, argument, vps);
            } else {
                uasserted(40230,
                          str::stream() << "unrecognized option to $replaceRoot stage: " << argName
                                        << ", only valid option is 'newRoot'.");
            }
        }

        // Check that there was a new root specified.
        uassert(40231, "no newRoot specified for the $replaceRoot stage", _newRoot);
    }

private:
    boost::intrusive_ptr<Expression> _newRoot;
    const boost::intrusive_ptr<ExpressionContext> _expCtx;
};

REGISTER_DOCUMENT_SOURCE(replaceRoot,
                         LiteParsedDocumentSourceDefault::parse,
                         DocumentSourceReplaceRoot::createFromBson);

intrusive_ptr<DocumentSource> DocumentSourceReplaceRoot::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {

    return new DocumentSourceSingleDocumentTransformation(
        pExpCtx, ReplaceRootTransformation::create(pExpCtx, elem), "$replaceRoot");
}

}  // namespace mongo
