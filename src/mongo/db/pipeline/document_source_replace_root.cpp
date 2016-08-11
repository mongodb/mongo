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

#include "mongo/db/pipeline/document_source.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

using boost::intrusive_ptr;

/**
 * This class implements the transformation logic for the $replaceRoot stage.
 */
class ReplaceRootTransformation final : public DocumentSourceSingleDocumentTransformation::TransformerInterface {

public:
    virtual Document applyTransformation(Document input) {
        // Extract subdocument in the form of a Value.
        _variables->setRoot(*input);
        Value newRoot = _newRoot->evaluate(_variables.get());

        // The newRoot expression must evaluate to a valid Value.
        uassert(40232,
                str::stream()
                    << " 'newRoot' argument to $replaceRoot stage must evaluate to a valid Value, "
                    << "try ensuring that your field path(s) exist by prepending a "
                    << "$match: {<path>: $exists} aggregation stage.",
                !newRoot.missing());

        // The newRoot expression, if it exists, must evaluate to an object.
        uassert(40228,
                str::stream()
                    << " 'newRoot' argument to $replaceRoot stage must evaluate to an object, but got "
                    << typeName(newRoot.getType())
                    << " try ensuring that it evaluates to an object by prepending a "
                    << "$match: {<path>: {$type: 'object'}} aggregation stage.",
                newRoot.getType() == Object);

        // Turn the value into a document.
        return newRoot.getDocument();
    }

    // Optimize the newRoot expression.
    virtual void optimize() {
        _newRoot = _newRoot->optimize();
    }

    virtual Document serialize(bool explain) {
        return Document{{getSourceName(), Document{{"newRoot", _newRoot->serialize(explain)}}}};
    }
    
    virtual DocumentSource::GetDepsReturn addDependencies(DepsTracker* deps) {
        _newRoot->addDependencies(deps);
        // This stage will replace the entire document with a new document, so any existing fields will
        // be replaced and cannot be required as dependencies.
        return DocumentSource::EXHAUSTIVE_FIELDS;
    }
    
    virtual void injectExpressionContext(
        const boost::intrusive_ptr<ExpressionContext>& pExpCtx) {}

private:
    std::unique_ptr<Variables> _variables;
    boost::intrusive_ptr<Expression> _newRoot;

};

DocumentSourceReplaceRoot::DocumentSourceReplaceRoot(
    const intrusive_ptr<ExpressionContext>& pExpCtx, const intrusive_ptr<Expression> expr)
    : DocumentSource(pExpCtx), _newRoot(expr) {}

REGISTER_DOCUMENT_SOURCE(replaceRoot, DocumentSourceReplaceRoot::createFromBson);

const char* DocumentSourceReplaceRoot::getSourceName() const {
    return "$replaceRoot";
}

Pipeline::SourceContainer::iterator DocumentSourceReplaceRoot::optimizeAt(
    Pipeline::SourceContainer::iterator itr, Pipeline::SourceContainer* container) {
    invariant(*itr == this);

    auto nextSkip = dynamic_cast<DocumentSourceSkip*>((*std::next(itr)).get());
    auto nextLimit = dynamic_cast<DocumentSourceLimit*>((*std::next(itr)).get());

    if (nextSkip || nextLimit) {
        std::swap(*itr, *std::next(itr));
        return itr == container->begin() ? itr : std::prev(itr);
    }
    return std::next(itr);
}

intrusive_ptr<DocumentSource> DocumentSourceReplaceRoot::createFromBson(
    BSONElement elem, const intrusive_ptr<ExpressionContext>& pExpCtx) {

    // We need a VariablesParseState in order to parse the 'newRoot' expression.
    intrusive_ptr<Expression> expr;
    VariablesIdGenerator idGenerator;
    VariablesParseState vps(&idGenerator);

    // Confirm that the stage was called with an object.
    uassert(40229,
            str::stream() << "expected an object as specification for $replaceRoot stage, got "
                          << typeName(elem.type()),
            elem.type() == Object);

    // Get the options from this stage. Currently the only option is newRoot.
    for (auto&& argument : elem.Obj()) {
        const StringData argName = argument.fieldNameStringData();

        if (argName == "newRoot") {
            // Allows for field path, object, and other expressions.
            expr = Expression::parseOperand(argument, vps);
        } else {
            uasserted(40230,
                      str::stream() << "unrecognized option to $replaceRoot stage: " << argName
                                    << ", only valid option is 'newRoot'.");
        }
    }

    // Check that there was a new root specified.
    uassert(40231, "no newRoot specified for the $replaceRoot stage", expr);

    // Create the ReplaceRoot aggregation stage.
    intrusive_ptr<DocumentSourceReplaceRoot> source = new DocumentSourceReplaceRoot(pExpCtx, expr);
    source->_variables.reset(new Variables(idGenerator.getIdCount()));
    return source;
}

}  // namespace mongo
