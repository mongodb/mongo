/**
*    Copyright (C) 2011 10gen Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    DocumentSourceFilterBase::~DocumentSourceFilterBase() {
    }

    boost::optional<Document> DocumentSourceFilterBase::getNext() {
        pExpCtx->checkForInterrupt();

        while (boost::optional<Document> next = pSource->getNext()) {
            if (accept(*next))
                return next;
        }

        // Nothing matched
        return boost::none;
    }

    DocumentSourceFilterBase::DocumentSourceFilterBase(
            const intrusive_ptr<ExpressionContext> &pExpCtx)
        : DocumentSource(pExpCtx)
    {}
}
