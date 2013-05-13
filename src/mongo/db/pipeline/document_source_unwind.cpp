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

#include "mongo/pch.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    /** Helper class to unwind arrays within a series of documents. */
    class DocumentSourceUnwind::Unwinder {
    public:
        /** @param unwindPath is the field path to the array to unwind. */
        Unwinder(const FieldPath& unwindPath);
        /** Reset the unwinder to unwind a new document. */
        void resetDocument(const Document& document);
        /** @return true if done unwinding the last document passed to resetDocument(). */
        bool eof() const;
        /** Try to advance to the next document unwound from the document passed to resetDocument().  */
        void advance();
        /**
         * @return the current document unwound from the document provided to resetDocument(), using
         * the current value in the array located at the provided unwindPath.  But @return
         * Document() if resetDocument() has not been called or the results to unwind
         * have been exhausted.
         */
        Document getCurrent();
    private:
        // Path to the array to unwind.
        const FieldPath _unwindPath;

        Value _inputArray;
        MutableDocument _output;

        // Document indexes of the field path components.
        vector<Position> _unwindPathFieldIndexes;
        // Index into the _inputArray to return next.
        size_t _index;
    };

    DocumentSourceUnwind::Unwinder::Unwinder(const FieldPath& unwindPath):
        _unwindPath(unwindPath) {
    }

    void DocumentSourceUnwind::Unwinder::resetDocument(const Document& document) {

        // Reset document specific attributes.
        _inputArray = Value();
        _output.reset(document);
        _unwindPathFieldIndexes.clear();
        _index = 0;

        Value pathValue = document.getNestedField(_unwindPath, &_unwindPathFieldIndexes);
        if (pathValue.nullish()) {
            // The path does not exist or is null.
            return;
        }

        // The target field must be an array to unwind.
        uassert(15978, str::stream() << (string)DocumentSourceUnwind::unwindName
                << ":  value at end of field path must be an array",
                pathValue.getType() == Array);

        if (pathValue.getArray().empty()) {
            // there are no values to unwind.
            return;
        }

        _inputArray = pathValue;
        verify(!eof()); // Checked above that the array is nonempty.
    }

    bool DocumentSourceUnwind::Unwinder::eof() const {
        return (_inputArray.getType() != Array)
            || (_index == _inputArray.getArrayLength());
    }

    void DocumentSourceUnwind::Unwinder::advance() {
        if (!eof()) { // don't advance past end()
            _index++;
        }
    }

    Document DocumentSourceUnwind::Unwinder::getCurrent() {
        if (eof()) {
            return Document();
        }

        // If needed, this will automatically clone all the documents along the
        // field path so that the end values are not shared across documents
        // that have come out of this pipeline operator.  This is a partial deep
        // clone. Because the value at the end will be replaced, everything
        // along the path leading to that will be replaced in order not to share
        // that change with any other clones (or the original).

        _output.setNestedField(_unwindPathFieldIndexes, _inputArray[_index]);

        return _output.peek();
    }

    const char DocumentSourceUnwind::unwindName[] = "$unwind";

    DocumentSourceUnwind::~DocumentSourceUnwind() {
    }

    DocumentSourceUnwind::DocumentSourceUnwind(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx) {
    }

    void DocumentSourceUnwind::lazyInit() {
        if (!_unwinder) {
            verify(_unwindPath);
            _unwinder.reset(new Unwinder(*_unwindPath));
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

    Document DocumentSourceUnwind::getCurrent() {
        verify(!eof());
        return _unwinder->getCurrent();
    }

    void DocumentSourceUnwind::sourceToBson(
        BSONObjBuilder *pBuilder, bool explain) const {
        verify(_unwindPath);
        pBuilder->append(unwindName, _unwindPath->getPath(true));
    }

    DocumentSource::GetDepsReturn DocumentSourceUnwind::getDependencies(set<string>& deps) const {
        verify(_unwindPath);
        deps.insert(_unwindPath->getPath(false));
        return SEE_NEXT;
    }

    void DocumentSourceUnwind::unwindPath(const FieldPath &fieldPath) {
        // Can't set more than one unwind path.
        uassert(15979, str::stream() << unwindName << "can't unwind more than one path",
                !_unwindPath);
        // Record the unwind path.
        _unwindPath.reset(new FieldPath(fieldPath));
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

        string prefixedPathString(pBsonElement->str());
        string pathString(Expression::removeFieldPrefix(prefixedPathString));
        intrusive_ptr<DocumentSourceUnwind> pUnwind(new DocumentSourceUnwind(pExpCtx));
        pUnwind->unwindPath(FieldPath(pathString));

        return pUnwind;
    }
}
