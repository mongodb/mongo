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
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/pch.h"

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    DocumentSource::DocumentSource(const intrusive_ptr<ExpressionContext> &pCtx)
        : pSource(NULL)
        , pExpCtx(pCtx)
    {}

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

    void DocumentSource::dispose() {
        if ( pSource ) {
            pSource->dispose();
        }
    }

    void DocumentSource::kill() {
        if ( pSource ) {
            pSource->kill();
        }
    }

    void DocumentSource::serializeToArray(vector<Value>& array, bool explain) const {
        Value entry = serialize(explain);
        if (!entry.missing()) {
            array.push_back(entry);
        }
    }
}
