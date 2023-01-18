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

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/document_source_single_document_transformation.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

/**
 * This class implements the transformation logic for the $replaceRoot and $replaceWith stages.
 */
class ReplaceRootTransformation final : public TransformerInterface {
public:
    enum class UserSpecifiedName { kReplaceRoot, kReplaceWith };

    ReplaceRootTransformation(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                              boost::intrusive_ptr<Expression> newRootExpression,
                              std::string errMsgContextForNonObject)
        : _expCtx(expCtx),
          _newRoot(std::move(newRootExpression)),
          _errMsgContextForNonObject(std::move(errMsgContextForNonObject)) {}

    TransformerType getType() const final {
        return TransformerType::kReplaceRoot;
    }

    Document applyTransformation(const Document& input) final;

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
        return {DocumentSource::GetModPathsReturn::Type::kAllPaths, OrderedPathSet{}, {}};
    }

    const boost::intrusive_ptr<Expression>& getExpression() const {
        return _newRoot;
    }

private:
    const boost::intrusive_ptr<ExpressionContext> _expCtx;
    boost::intrusive_ptr<Expression> _newRoot;

    // A string for additional context for the user about where/why we were expecting an object.
    // This can be helpful if you are using $replaceRoot as part of an alias expansion as we do in
    // $documents for example. Goes first in the template error message below.
    std::string _errMsgContextForNonObject;
    static constexpr StringData kErrorTemplate =
        "{} must evaluate to an object, but resulting value was: {}. Type of resulting value: "
        "'{}'. Input document: {}"_sd;
};

/*
 * $replaceRoot takes an object containing only an expression in the newRoot field, and replaces
 * each incoming document with the result of evaluating that expression. Throws an error if the
 * given expression is not an object or if the expression evaluates to the "missing" Value. This
 * is implemented as an extension of DocumentSourceSingleDocumentTransformation.
 *
 * There is a shorthand $replaceWith alias which takes a direct single argument containing the
 * expression which will become the new root: {$replaceWith: <expression>} aliases to {$replaceRoot:
 * {newRoot: <expression>}}.
 */
class DocumentSourceReplaceRoot final {
public:
    static constexpr StringData kStageName = "$replaceRoot"_sd;
    static constexpr StringData kAliasNameReplaceWith = "$replaceWith"_sd;
    /**
     * Creates a new replaceRoot DocumentSource from the BSON specification of the $replaceRoot
     * stage.
     */
    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    static boost::intrusive_ptr<DocumentSource> create(
        const boost::intrusive_ptr<ExpressionContext>& expCtx,
        const boost::intrusive_ptr<Expression>& newRootExpression,
        std::string errMsgContextForNonObjects);

private:
    // It is illegal to construct a DocumentSourceReplaceRoot directly, use createFromBson()
    // instead.
    DocumentSourceReplaceRoot() = default;
};

}  // namespace mongo
