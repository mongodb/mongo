
/**
 * Copyright (C) 2016 MongoDB Inc.
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
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"

namespace mongo {

class DocumentSourceRedact final : public DocumentSource {
public:
    GetNextResult getNext() final;
    const char* getSourceName() const final;
    boost::intrusive_ptr<DocumentSource> optimize() final;

    /**
     * Attempts to duplicate the redact-safe portion of a subsequent $match before the $redact
     * stage.
     */
    Pipeline::SourceContainer::iterator doOptimizeAt(Pipeline::SourceContainer::iterator itr,
                                                     Pipeline::SourceContainer* container) final;

    static boost::intrusive_ptr<DocumentSource> createFromBson(
        BSONElement elem, const boost::intrusive_ptr<ExpressionContext>& expCtx);

    Value serialize(boost::optional<ExplainOptions::Verbosity> explain = boost::none) const final;

private:
    DocumentSourceRedact(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                         const boost::intrusive_ptr<Expression>& previsit);

    // These both work over pExpCtx->variables.
    boost::optional<Document> redactObject(const Document& root);  // redacts CURRENT
    Value redactValue(const Value& in, const Document& root);

    Variables::Id _currentId;
    boost::intrusive_ptr<Expression> _expression;
};

}  // namespace mongo
