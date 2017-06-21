/**
 * Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/repl/idempotency_document_structure.h"

namespace mongo {

DocumentStructureEnumerator::iterator DocumentStructureEnumerator::begin() const {
    return this->_docs.cbegin();
}

DocumentStructureEnumerator::iterator DocumentStructureEnumerator::end() const {
    return this->_docs.cend();
}

std::vector<BSONObj> DocumentStructureEnumerator::getDocs() const {
    return this->_docs;
}

BSONArrayBuilder DocumentStructureEnumerator::_getArrayBuilderFromArr(BSONArray arr) {
    BSONArrayBuilder arrBuilder;
    for (auto elem : arr) {
        arrBuilder.append(elem);
    }

    return arrBuilder;
}

void DocumentStructureEnumerator::_enumerateFixedLenArrs(const std::set<StringData>& fields,
                                                         const size_t depthRemaining,
                                                         const size_t length,
                                                         BSONArray arr,
                                                         std::vector<BSONArray>* arrs) {
    if (!length) {
        // Base case: no more room for any other elements.
        arrs->push_back(arr);
        return;
    }

    // Otherwise, go through our choices, similar to the approach to documents.
    //

    // Scalar.
    BSONArrayBuilder scalarArr = _getArrayBuilderFromArr(arr);
    scalarArr.append(0);
    _enumerateFixedLenArrs(fields, depthRemaining, length - 1, scalarArr.arr(), arrs);

    if (depthRemaining <= 0) {
        return;
    }

    // Subarray.
    std::vector<BSONArray> subArrs =
        _enumerateArrs(fields, depthRemaining - 1, length + arr.nFields());
    for (auto subArr : subArrs) {
        BSONArrayBuilder arrayArr = _getArrayBuilderFromArr(arr);
        arrayArr.append(subArr);
        _enumerateFixedLenArrs(fields, depthRemaining, length - 1, arrayArr.arr(), arrs);
    }

    // Document.
    BSONObj blankDoc;
    std::vector<BSONObj> subDocs;
    _enumerateDocs(fields, depthRemaining - 1, length, blankDoc, &subDocs);
    for (auto subDoc : subDocs) {
        BSONArrayBuilder docArr = _getArrayBuilderFromArr(arr);
        docArr.append(subDoc);
        _enumerateFixedLenArrs(fields, depthRemaining, length - 1, docArr.arr(), arrs);
    }

    return;
}

void DocumentStructureEnumerator::_enumerateDocs(const std::set<StringData>& fields,
                                                 const size_t depthRemaining,
                                                 const size_t length,
                                                 BSONObj doc,
                                                 std::vector<BSONObj>* docs) {
    if (fields.empty()) {
        // Base case: when we have run out of fields to use
        docs->push_back(doc);
        return;
    }

    // Create a copy of the fields we have.
    std::set<StringData> remainingFields(fields);
    // Pop the first field arbitrarily.
    StringData field = *remainingFields.begin();
    remainingFields.erase(remainingFields.begin());

    // Branch off depending on the choice.
    //

    // Scalar.
    BSONObjBuilder scalarDoc(doc);
    scalarDoc.append(field, 0);
    _enumerateDocs(remainingFields, depthRemaining, length, scalarDoc.obj(), docs);

    // Omit the field.
    BSONObjBuilder vanishDoc(doc);
    _enumerateDocs(remainingFields, depthRemaining, length, vanishDoc.obj(), docs);

    if (depthRemaining <= 0) {
        // If we are ever at the deepest level possible, we have no more choices after this.
        return;
    }

    // Array.
    for (auto subArr : _enumerateArrs(remainingFields, depthRemaining - 1, length)) {
        BSONObjBuilder arrayDoc(doc);
        arrayDoc.append(field, subArr);
        _enumerateDocs(remainingFields, depthRemaining, length, arrayDoc.obj(), docs);
    }

    // Subdocument.
    BSONObj blankDoc;
    std::vector<BSONObj> subDocs;
    _enumerateDocs(remainingFields, depthRemaining - 1, length, blankDoc, &subDocs);
    for (auto subDoc : subDocs) {
        BSONObjBuilder docDoc(doc);
        docDoc.append(field, subDoc);
        _enumerateDocs(remainingFields, depthRemaining, length, docDoc.obj(), docs);
    }
}

std::vector<BSONArray> DocumentStructureEnumerator::_enumerateArrs(
    const std::set<StringData>& fields, const size_t depth, const size_t length) {
    std::vector<BSONArray> arrs;
    // We enumerate arrays of each possible length independently of each other to avoid having to
    // account for how different omissions of elements in an array are equivalent to each other.
    // For example, we'd otherwise need to treat omitting the first element and adding x as distinct
    // from adding x and omitting the second element since both yield an array containing only the
    // element x. Without this, we will enumerate duplicate arrays.
    for (size_t i = 0; i <= length; i++) {
        BSONArray emptyArr;
        _enumerateFixedLenArrs(fields, depth, i, emptyArr, &arrs);
    }

    return arrs;
}

std::vector<BSONObj> DocumentStructureEnumerator::enumerateDocs() const {
    BSONObj startDoc;
    std::vector<BSONObj> docs;
    _enumerateDocs(this->_fields, this->_depth, this->_length, startDoc, &docs);
    return docs;
}

std::vector<BSONArray> DocumentStructureEnumerator::enumerateArrs() const {
    return _enumerateArrs(this->_fields, this->_depth, this->_length);
}

DocumentStructureEnumerator::DocumentStructureEnumerator(std::set<StringData> fields,
                                                         size_t depth,
                                                         size_t length)
    : _fields(std::move(fields)), _depth(depth), _length(length) {
    this->_docs = enumerateDocs();
}
}  // namespace mongo
