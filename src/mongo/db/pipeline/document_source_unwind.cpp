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

    /** Helper class to unwind arrays within a series of documents. */
    class DocumentSourceUnwind::Unwinder {
    public:
        /** @param unwindPath is the field path to the array to unwind. */
        Unwinder(const FieldPath& unwindPath);
        /** Reset the unwinder to unwind a new document. */
        void resetDocument(const intrusive_ptr<Document>& document);
        /** @return true if done unwinding the last document passed to resetDocument(). */
        bool eof() const;
        /**
         * Try to advance to the next document unwound from the document passed to resetDocument().
         * @return true if advanced to a new unwound document, but false if done advancing.
         */
        void advance();
        /**
         * @return the current document unwound from the document provided to resetDocuemnt(), using
         * the current value in the array located at the provided unwindPath.  But @return
         * intrusive_ptr<Document>() if resetDocument() has not been called or the results to unwind
         * have been exhausted.
         */
        intrusive_ptr<Document> getCurrent() const;
    private:
        /**
         * @return the value at the unwind path, otherwise an empty pointer if no such value
         * exists.  The _unwindPathFieldIndexes attribute will be set as the field path is traversed
         * to find the value to unwind.
         */
        intrusive_ptr<const Value> extractUnwindValue();
        // Path to the array to unwind.
        FieldPath _unwindPath;
        // The souce document to unwind.
        intrusive_ptr<Document> _document;
        // Document indexes of the field path components.
        vector<int> _unwindPathFieldIndexes;
        // Iterator over the array within _document to unwind.
        intrusive_ptr<ValueIterator> _unwindArrayIterator;
        // The last value returned from _unwindArrayIterator.
        intrusive_ptr<const Value> _unwindArrayIteratorCurrent;
    };

    DocumentSourceUnwind::Unwinder::Unwinder(const FieldPath& unwindPath):
        _unwindPath(unwindPath) {
    }

    void DocumentSourceUnwind::Unwinder::resetDocument(const intrusive_ptr<Document>& document) {
        verify( document );

        // Reset document specific attributes.
        _document = document;
        _unwindPathFieldIndexes.clear();
        _unwindArrayIterator.reset();
        _unwindArrayIteratorCurrent.reset();

        intrusive_ptr<const Value> pathValue = extractUnwindValue(); // sets _unwindPathFieldIndexes
        if (!pathValue) {
            // The path does not exist.
            return;
        }

        bool nothingToEmit =
            (pathValue->getType() == jstNULL) ||
            (pathValue->getType() == Undefined) ||
            ((pathValue->getType() == Array) && (pathValue->getArrayLength() == 0));

        if (nothingToEmit) {
            // The target field exists, but there are no values to unwind.
            return;
        }

        // The target field must be an array to unwind.
        uassert(15978, str::stream() << (string)DocumentSourceUnwind::unwindName
                << ":  value at end of field path must be an array",
                pathValue->getType() == Array);

        // Start the iterator used to unwind the array.
        _unwindArrayIterator = pathValue->getArray();
        verify(_unwindArrayIterator->more()); // Checked above that the array is nonempty.
        // Pull the first value out of the iterator.
        _unwindArrayIteratorCurrent = _unwindArrayIterator->next();
    }

    bool DocumentSourceUnwind::Unwinder::eof() const {
        return !_unwindArrayIteratorCurrent;
    }

    void DocumentSourceUnwind::Unwinder::advance() {
        if (!_unwindArrayIterator) {
            // resetDocument() has not been called or the supplied document had no results to
            // unwind.
            _unwindArrayIteratorCurrent = NULL;
        }
        else if (!_unwindArrayIterator->more()) {
            // There are no more results to unwind.
            _unwindArrayIteratorCurrent = NULL;
        }
        else {
            _unwindArrayIteratorCurrent = _unwindArrayIterator->next();
        }
    }

    intrusive_ptr<Document> DocumentSourceUnwind::Unwinder::getCurrent() const {
        if (!_unwindArrayIteratorCurrent) {
            return NULL;
        }

        // Clone all the documents along the field path so that the end values are not shared across
        // documents that have come out of this pipeline operator.  This is a partial deep clone.
        // Because the value at the end will be replaced, everything along the path leading to that
        // will be replaced in order not to share that change with any other clones (or the
        // original).

        intrusive_ptr<Document> clone(_document->clone());
        intrusive_ptr<Document> current(clone);
        const size_t n = _unwindPathFieldIndexes.size();
        verify(n);
        for(size_t i = 0; i < n; ++i) {
            const size_t fi = _unwindPathFieldIndexes[i];
            Document::FieldPair fp(current->getField(fi));
            if (i + 1 < n) {
                // For every object in the path but the last, clone it and continue on down.
                intrusive_ptr<Document> next = fp.second->getDocument()->clone();
                current->setField(fi, fp.first, Value::createDocument(next));
                current = next;
            }
            else {
                // In the last nested document, subsitute the current unwound value.
                current->setField(fi, fp.first, _unwindArrayIteratorCurrent);
            }
        }

        return clone;
    }

    intrusive_ptr<const Value> DocumentSourceUnwind::Unwinder::extractUnwindValue() {

        intrusive_ptr<Document> current = _document;
        intrusive_ptr<const Value> pathValue;
        const size_t pathLength = _unwindPath.getPathLength();
        verify(pathLength>0);
        for(size_t i = 0; i < pathLength; ++i) {

            size_t idx = current->getFieldIndex(_unwindPath.getFieldName(i));

            if (idx == current->getFieldCount()) {
                // The target field is missing.
                return NULL;
            }

            // Record the indexes of the fields down the field path in order to quickly replace them
            // as the documents along the field path are cloned.
            _unwindPathFieldIndexes.push_back(idx);

            pathValue = current->getField(idx).second;

            if (i < pathLength - 1) {

                if (pathValue->getType() != Object) {
                    // The next field in the path cannot exist (inside a non object).
                    return NULL;
                }

                // Move down the object tree.
                current = pathValue->getDocument();
            }
        }

        return pathValue;
    }

    const char DocumentSourceUnwind::unwindName[] = "$unwind";

    DocumentSourceUnwind::~DocumentSourceUnwind() {
    }

    DocumentSourceUnwind::DocumentSourceUnwind(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx),
        _unwindPath() {
    }

    void DocumentSourceUnwind::lazyInit() {
        if (!_unwinder) {
            verify(_unwindPath.getPathLength());
            _unwinder.reset(new Unwinder(_unwindPath));
            if (!pSource->eof()) {
                // Set up the first source document for unwinding.
                _unwinder->resetDocument(pSource->getCurrent());
            }
            mayAdvanceSource();
        }
    }

    void DocumentSourceUnwind::mayAdvanceSource() {
        while(_unwinder->eof()) {
            // The _unwinder is exhausted.

            if (pSource->eof()) {
                // The source is exhausted.
                return;
            }
            if (!pSource->advance()) {
                // The source is exhausted.
                return;
            }
            // Reset the _unwinder with pSource's next document.
            _unwinder->resetDocument(pSource->getCurrent());
        }
    }

    const char *DocumentSourceUnwind::getSourceName() const {
        return unwindName;
    }

    bool DocumentSourceUnwind::eof() {
        lazyInit();
        return _unwinder->eof();
    }

    bool DocumentSourceUnwind::advance() {
        DocumentSource::advance(); // check for interrupts
        lazyInit();
        _unwinder->advance();
        mayAdvanceSource();
        return !_unwinder->eof();
    }

    intrusive_ptr<Document> DocumentSourceUnwind::getCurrent() {
        lazyInit();
        return _unwinder->getCurrent();
    }

    void DocumentSourceUnwind::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {
        pBuilder->append(unwindName, _unwindPath.getPath(true));
    }

    DocumentSource::GetDepsReturn DocumentSourceUnwind::getDependencies(set<string>& deps) const {
        deps.insert(_unwindPath.getPath(false));
        return SEE_NEXT;
    }

    void DocumentSourceUnwind::unwindPath(const FieldPath &fieldPath) {
        // Can't set more than one unwind path.
        uassert(15979, str::stream() << unwindName <<
                "can't unwind more than one path at once",
                !_unwindPath.getPathLength());
        uassert(15980, "the path of the field to unwind cannot be empty",
                fieldPath.getPathLength());
        // Record the unwind path.
        _unwindPath = fieldPath;
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
        intrusive_ptr<DocumentSourceUnwind> pUnwind(new DocumentSourceUnwind(pExpCtx));
        pUnwind->unwindPath(FieldPath(pathString));

        return pUnwind;
    }
}
