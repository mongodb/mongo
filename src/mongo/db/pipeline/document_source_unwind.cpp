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

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

    using boost::intrusive_ptr;
    using std::string;
    using std::vector;

    /** Helper class to unwind array from a single document. */
    class DocumentSourceUnwind::Unwinder {
    public:
        /** @param unwindPath is the field path to the array to unwind. */
        Unwinder(const FieldPath& unwindPath);
        /** Reset the unwinder to unwind a new document. */
        void resetDocument(const Document& document);

        /**
         * @return the next document unwound from the document provided to resetDocument(), using
         * the current value in the array located at the provided unwindPath.
         *
         * Returns boost::none if the array is exhausted.
         */
        boost::optional<Document> getNext();

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

        _inputArray = pathValue;
    }

    boost::optional<Document> DocumentSourceUnwind::Unwinder::getNext() {
        if (_inputArray.missing())
            return boost::none;

        // If needed, this will automatically clone all the documents along the
        // field path so that the end values are not shared across documents
        // that have come out of this pipeline operator.  This is a partial deep
        // clone. Because the value at the end will be replaced, everything
        // along the path leading to that will be replaced in order not to share
        // that change with any other clones (or the original).

        if (_inputArray.getType() == Array) {
            if (_index == _inputArray.getArrayLength())
                return boost::none;
            _output.setNestedField(_unwindPathFieldIndexes, _inputArray[_index]);
        }
        else if (_index > 0) {
            return boost::none;
        }
        else {
            _output.setNestedField(_unwindPathFieldIndexes, _inputArray);
        }
        _index++;
        return _output.peek();
    }

    const char DocumentSourceUnwind::unwindName[] = "$unwind";

    DocumentSourceUnwind::DocumentSourceUnwind(
        const intrusive_ptr<ExpressionContext> &pExpCtx):
        DocumentSource(pExpCtx) {
    }

    const char *DocumentSourceUnwind::getSourceName() const {
        return unwindName;
    }

    boost::optional<Document> DocumentSourceUnwind::getNext() {
        pExpCtx->checkForInterrupt();

        boost::optional<Document> out = _unwinder->getNext();
        while (!out) {
            // No more elements in array currently being unwound. This will loop if the input
            // document is missing the unwind field or has an empty array.
            boost::optional<Document> input = pSource->getNext();
            if (!input)
                return boost::none; // input exhausted

            // Try to extract an output document from the new input document.
            _unwinder->resetDocument(*input);
            out = _unwinder->getNext();
        }

        return out;
    }

    Value DocumentSourceUnwind::serialize(bool explain) const {
        verify(_unwindPath);
        return Value(DOC(getSourceName() << _unwindPath->getPath(true)));
    }

    DocumentSource::GetDepsReturn DocumentSourceUnwind::getDependencies(DepsTracker* deps) const {
        deps->fields.insert(_unwindPath->getPath(false));
        return SEE_NEXT;
    }

    void DocumentSourceUnwind::unwindPath(const FieldPath &fieldPath) {
        // Can't set more than one unwind path.
        uassert(15979, str::stream() << unwindName << "can't unwind more than one path",
                !_unwindPath);
        // Record the unwind path.
        _unwindPath.reset(new FieldPath(fieldPath));
        _unwinder.reset(new Unwinder(fieldPath));
    }

    intrusive_ptr<DocumentSource> DocumentSourceUnwind::createFromBson(
            BSONElement elem,
            const intrusive_ptr<ExpressionContext> &pExpCtx) {
        /*
          The value of $unwind should just be a field path.
         */
        uassert(15981, str::stream() << "the " << unwindName <<
                " field path must be specified as a string",
                elem.type() == String);

        string prefixedPathString(elem.str());
        string pathString(Expression::removeFieldPrefix(prefixedPathString));
        intrusive_ptr<DocumentSourceUnwind> pUnwind(new DocumentSourceUnwind(pExpCtx));
        pUnwind->unwindPath(FieldPath(pathString));

        return pUnwind;
    }
}
