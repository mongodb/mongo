/**
 * Copyright 2011 (c) 10gen Inc.
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
 */

#include "pch.h"
#include "db/pipeline/document_source.h"

#include "db/jsobj.h"
#include "db/pipeline/document.h"
#include "db/pipeline/expression.h"
#include "db/pipeline/value.h"

namespace mongo {

    const char DocumentSourceUnwind::unwindName[] = "$unwind";

    DocumentSourceUnwind::~DocumentSourceUnwind() {
    }

    DocumentSourceUnwind::DocumentSourceUnwind(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx),
        unwindPath(),
        pNoUnwindDocument(),
        pUnwindArray(),
        pUnwinder(),
        pUnwindValue() {
    }

    const char *DocumentSourceUnwind::getSourceName() const {
        return unwindName;
    }

    bool DocumentSourceUnwind::eof() {
        /*
          If we're unwinding an array, and there are more elements, then we
          can return more documents.
        */
        if (pUnwinder.get() && pUnwinder->more())
            return false;

        return pSource->eof();
    }

    bool DocumentSourceUnwind::advance() {
        DocumentSource::advance(); // check for interrupts

        if (pUnwinder.get() && pUnwinder->more()) {
            pUnwindValue = pUnwinder->next();
            return true;
        }

        /* release the last document and advance */
        resetArray();
        return pSource->advance();
    }

    intrusive_ptr<Document> DocumentSourceUnwind::getCurrent() {
        if (!pNoUnwindDocument.get()) {
            intrusive_ptr<Document> pInDocument(pSource->getCurrent());

            /* create the result document */
            pNoUnwindDocument = pInDocument;
            fieldIndex.clear();

            /*
              First we'll look to see if the path is there.  If it isn't,
              we'll pass this document through.  If it is, we record the
              indexes of the fields down the field path so that we can
              quickly replace them as we clone the documents along the
              field path.

              We have to clone all the documents along the field path so
              that we don't share the end value across documents that have
              come out of this pipeline operator.
             */
            intrusive_ptr<Document> pCurrent(pInDocument);
            const size_t pathLength = unwindPath.getPathLength();
            for(size_t i = 0; i < pathLength; ++i) {
                size_t idx = pCurrent->getFieldIndex(
                    unwindPath.getFieldName(i));
                if (idx == pCurrent->getFieldCount() ) {
                    /* this document doesn't contain the target field */
                    resetArray();
                    return pInDocument;
                    break;
                }

                fieldIndex.push_back(idx);
                Document::FieldPair fp(pCurrent->getField(idx));
                intrusive_ptr<const Value> pPathValue(fp.second);
                if (i < pathLength - 1) {
                    if (pPathValue->getType() != Object) {
                        /* can't walk down the field path */
                        resetArray();
                        uassert(15977, str::stream() << unwindName <<
                                ":  cannot traverse field path past scalar value for \"" <<
                                fp.first << "\"", false);
                        break;
                    }

                    /* move down the object tree */
                    pCurrent = pPathValue->getDocument();
                }
                else /* (i == pathLength - 1) */ {
                    if (pPathValue->getType() != Array) {
                        /* last item on path must be an array to unwind */
                        resetArray();
                        uassert(15978, str::stream() << unwindName <<
                                ":  value at end of field path must be an array",
                                false);
                        break;
                    }

                    /* keep track of the array we're unwinding */
                    pUnwindArray = pPathValue;
                    if (pUnwindArray->getArrayLength() == 0) {
                        /*
                          The $unwind of an empty array is a NULL value.  If we
                          encounter this, use the non-unwind path, but replace
                          pOutField with a null.

                          Make sure unwind value is clear so the array is
                          removed.
                        */
                        pUnwindValue.reset();
                        intrusive_ptr<Document> pClone(clonePath());
                        resetArray();
                        return pClone;
                    }

                    /* get the iterator we'll use to unwind the array */
                    pUnwinder = pUnwindArray->getArray();
                    verify(pUnwinder->more()); // we just checked above...
                    pUnwindValue = pUnwinder->next();
                }
            }
        }

        /*
          If we're unwinding a field, create an alternate document.  In the
          alternate (clone), replace the unwound array field with the element
          at the appropriate index.
         */
        if (pUnwindArray.get()) {
            /* clone the document with an array we're unwinding */
            intrusive_ptr<Document> pUnwindDocument(clonePath());

            return pUnwindDocument;
        }

        return pNoUnwindDocument;
    }

    intrusive_ptr<Document> DocumentSourceUnwind::clonePath() const {
        /*
          For this to be valid, we must already have pNoUnwindDocument set,
          and have set up the vector of indices for that document in fieldIndex.
         */
        verify(pNoUnwindDocument.get());

        intrusive_ptr<Document> pClone(pNoUnwindDocument->clone());
        intrusive_ptr<Document> pCurrent(pClone);
        const size_t n = fieldIndex.size();
        verify(n);
        for(size_t i = 0; i < n; ++i) {
            const size_t fi = fieldIndex[i];
            Document::FieldPair fp(pCurrent->getField(fi));
            if (i + 1 < n) {
                /*
                  For every object in the path but the last, clone it and
                  continue on down.
                */
                intrusive_ptr<Document> pNext(
                    fp.second->getDocument()->clone());
                pCurrent->setField(fi, fp.first, Value::createDocument(pNext));
                pCurrent = pNext;
            }
            else {
                /* for the last, subsitute the next unwound value */
                pCurrent->setField(fi, fp.first, pUnwindValue);
            }
        }

        return pClone;
    }

    void DocumentSourceUnwind::sourceToBson(BSONObjBuilder *pBuilder) const {
        pBuilder->append(unwindName, unwindPath.getPath(true));
    }

    intrusive_ptr<DocumentSourceUnwind> DocumentSourceUnwind::create(
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        intrusive_ptr<DocumentSourceUnwind> pSource(
            new DocumentSourceUnwind(pExpCtx));
        return pSource;
    }

    void DocumentSourceUnwind::unwindField(const FieldPath &rFieldPath) {
        /* can't set more than one unwind field */
        uassert(15979, str::stream() << unwindName <<
                "can't unwind more than one path at once",
                !unwindPath.getPathLength());

        uassert(15980, "the path of the field to unwind cannot be empty",
                false);

        /* record the field path */
        unwindPath = rFieldPath;
    }

    intrusive_ptr<DocumentSource> DocumentSourceUnwind::createFromBson(
        BSONElement *pBsonElement,
        const intrusive_ptr<ExpressionContext> &pExpCtx) {
        /*
          The value of $unwind should just be a field path.
         */
        uassert(15981, str::stream() << "the " << unwindName <<
                " field path must be specified as a string",
                pBsonElement->type() == String);

        string prefixedPathString(pBsonElement->String());
        string pathString(Expression::removeFieldPrefix(prefixedPathString));
        intrusive_ptr<DocumentSourceUnwind> pUnwind(
            DocumentSourceUnwind::create(pExpCtx));
        pUnwind->unwindPath = FieldPath(pathString);

        return pUnwind;
    }

    void DocumentSourceUnwind::manageDependencies(
        const intrusive_ptr<DependencyTracker> &pTracker) {
        pTracker->addDependency(unwindPath.getPath(false), this);
    }
    
}
