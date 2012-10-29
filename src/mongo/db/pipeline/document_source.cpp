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
        pExpCtx(pCtx),
        nRowsOut(0) {
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

    bool DocumentSource::advance() {
        pExpCtx->checkForInterrupt(); // might not return
        return false;
    }

    void DocumentSource::dispose() {
        if ( pSource ) {
            // This is required for the DocumentSourceCursor to release its read lock, see
            // SERVER-6123.
            pSource->dispose();
        }
    }

    void DocumentSource::addToBsonArray(
        BSONArrayBuilder *pBuilder, bool explain) const {
        BSONObjBuilder insides;
        sourceToBson(&insides, explain);

/* No statistics at this time
        if (explain) {
            insides.append("nOut", nOut);
        }
*/

        pBuilder->append(insides.done());
    }

    void DocumentSource::writeString(stringstream &ss) const {
        BSONArrayBuilder bab;
        addToBsonArray(&bab);
        BSONArray ba(bab.arr());
        ss << ba.toString(/* isArray */true); 
            // our toString should use standard string types.....
    }

    BSONObj DocumentSource::depsToProjection(const set<string>& deps) {
        BSONObjBuilder bb;

        bool needId = false;

        string last;
        for (set<string>::const_iterator it(deps.begin()), end(deps.end()); it!=end; ++it) {
            if (str::startsWith(*it, "_id") && (it->size() == 3 || (*it)[3] == '.')) {
                // _id and subfields are handled specially due in part to SERVER-7502
                needId = true;
                continue;
            }
            if (!last.empty() && str::startsWith(*it, last)) {
                // we are including a parent of *it so we don't need to
                // include this field explicitly. In fact, due to
                // SERVER-6527 if we included this field, the parent
                // wouldn't be fully included.
                continue;
            }
            last = *it + '.';
            bb.append(*it, 1);
        }

        if (needId) // we are explicit either way
            bb.append("_id", 1);
        else
            bb.append("_id", 0);

        return bb.obj();
    }
}
