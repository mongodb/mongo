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

#include "pch.h"

#include "db/pipeline/document_source.h"
#include "db/pipeline/expression_context.h"

namespace mongo {

    DocumentSource::DocumentSource(
        const intrusive_ptr<ExpressionContext> &pCtx):
        pSource(NULL),
        step(-1),
        pExpCtx(pCtx) {
    }

    DocumentSource::~DocumentSource() {
    }

    const char *DocumentSource::getSourceName() const {
        static const char unknown[] = "[UNKNOWN]";
        return unknown;
    }

    void DocumentSource::setSource(DocumentSource *pTheSource) {
        verify(!pSource);
        pSource = pTheSource;
    }

    bool DocumentSource::coalesce(
        const intrusive_ptr<DocumentSource> &pNextSource) {
        return false;
    }

    void DocumentSource::optimize() {
    }

    void DocumentSource::manageDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker) {
#ifdef MONGO_LATER_SERVER_4644
        verify(false); // identify any sources that need this but don't have it
#endif /* MONGO_LATER_SERVER_4644 */
    }

    bool DocumentSource::advance() {
        pExpCtx->checkForInterrupt(); // might not return
        return false;
    }

    void DocumentSource::addToBsonArray(BSONArrayBuilder *pBuilder) const {
        BSONObjBuilder insides;
        sourceToBson(&insides);
        pBuilder->append(insides.done());
    }

    void DocumentSource::writeString(stringstream &ss) const {
        BSONArrayBuilder bab;
        addToBsonArray(&bab);
        BSONArray ba(bab.arr());
        ss << ba.toString(/* isArray */true); 
            // our toString should use standard string types.....
    }
}
