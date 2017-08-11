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

#include "mongo/db/jsobj.h"
#include "mongo/db/pipeline/value.h"

namespace mongo {

DocumentStructureEnumeratorConfig::DocumentStructureEnumeratorConfig(std::set<StringData> fields_,
                                                                     std::size_t depth_,
                                                                     std::size_t length_,
                                                                     bool skipSubDocs_,
                                                                     bool skipSubArrs_)
    : fields(std::move(fields_)),
      depth(depth_),
      length(length_),
      skipSubDocs(skipSubDocs_),
      skipSubArrs(skipSubArrs_) {}

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

void DocumentStructureEnumerator::_enumerateFixedLenArrs(
    const DocumentStructureEnumeratorConfig& config,
    const ScalarGenerator* scalarGenerator,
    BSONArray arr,
    std::vector<BSONArray>* arrs) {
    if (!config.length) {
        // Base case: no more room for any other elements.
        arrs->push_back(arr);
        return;
    }

    // Otherwise, go through our choices, similar to the approach to documents.
    //
    DocumentStructureEnumeratorConfig nextElementConfig(config);
    nextElementConfig.length--;

    // Scalar.
    BSONArrayBuilder scalarArr = _getArrayBuilderFromArr(arr);
    scalarGenerator->generateScalar().addToBsonArray(&scalarArr);
    _enumerateFixedLenArrs(nextElementConfig, scalarGenerator, scalarArr.arr(), arrs);

    if (config.depth <= 0) {
        return;
    }

    DocumentStructureEnumeratorConfig nextLayerConfig(config);
    nextLayerConfig.depth--;
    nextLayerConfig.length += arr.nFields();
    // Subarray.
    std::vector<BSONArray> subArrs = _enumerateArrs(nextLayerConfig, scalarGenerator);
    for (auto subArr : subArrs) {
        BSONArrayBuilder arrayArr = _getArrayBuilderFromArr(arr);
        arrayArr.append(subArr);
        _enumerateFixedLenArrs(nextElementConfig, scalarGenerator, arrayArr.arr(), arrs);
    }

    // Document.
    if (!config.skipSubDocs) {
        BSONObj blankDoc;
        std::vector<BSONObj> subDocs;
        _enumerateDocs(nextLayerConfig, scalarGenerator, blankDoc, &subDocs);
        for (auto subDoc : subDocs) {
            BSONArrayBuilder docArr = _getArrayBuilderFromArr(arr);
            docArr.append(subDoc);
            _enumerateFixedLenArrs(nextElementConfig, scalarGenerator, docArr.arr(), arrs);
        }
    }

    return;
}

void DocumentStructureEnumerator::_enumerateDocs(const DocumentStructureEnumeratorConfig& config,
                                                 const ScalarGenerator* scalarGenerator,
                                                 BSONObj doc,
                                                 std::vector<BSONObj>* docs) {
    if (config.fields.empty()) {
        // Base case: when we have run out of fields to use.
        docs->push_back(doc);
        return;
    }

    // Create a copy of the fields we have.
    std::set<StringData> remainingFields(config.fields);
    // Pop the first field arbitrarily.
    StringData field = *remainingFields.begin();
    remainingFields.erase(remainingFields.begin());

    DocumentStructureEnumeratorConfig nextFieldConfig(config);
    nextFieldConfig.fields = remainingFields;

    // Branch off depending on the choice.
    //

    // Scalar.
    BSONObjBuilder scalarDoc(doc);
    scalarGenerator->generateScalar().addToBsonObj(&scalarDoc, field);
    _enumerateDocs(nextFieldConfig, scalarGenerator, scalarDoc.obj(), docs);

    // Omit the field.
    BSONObjBuilder vanishDoc(doc);
    _enumerateDocs(nextFieldConfig, scalarGenerator, vanishDoc.obj(), docs);

    if (config.depth <= 0) {
        // If we are ever at the deepest level possible, we have no more choices after this.
        return;
    }

    DocumentStructureEnumeratorConfig nextLayerConfig(nextFieldConfig);
    nextLayerConfig.depth--;

    if (!config.skipSubArrs) {
        // Array.
        for (auto subArr : _enumerateArrs(nextLayerConfig, scalarGenerator)) {
            BSONObjBuilder arrayDoc(doc);
            arrayDoc.append(field, subArr);
            _enumerateDocs(nextFieldConfig, scalarGenerator, arrayDoc.obj(), docs);
        }
    }

    // Subdocument.
    if (!config.skipSubDocs) {
        BSONObj blankDoc;
        std::vector<BSONObj> subDocs;
        _enumerateDocs(nextLayerConfig, scalarGenerator, blankDoc, &subDocs);
        for (auto subDoc : subDocs) {
            BSONObjBuilder docDoc(doc);
            docDoc.append(field, subDoc);
            _enumerateDocs(nextFieldConfig, scalarGenerator, docDoc.obj(), docs);
        }
    }
}

std::vector<BSONArray> DocumentStructureEnumerator::_enumerateArrs(
    const DocumentStructureEnumeratorConfig& config, const ScalarGenerator* scalarGenerator) {
    std::vector<BSONArray> arrs;
    // We enumerate arrays of each possible length independently of each other to avoid having to
    // account for how different omissions of elements in an array are equivalent to each other.
    // For example, we'd otherwise need to treat omitting the first element and adding x as distinct
    // from adding x and omitting the second element since both yield an array containing only the
    // element x. Without this, we will enumerate duplicate arrays.
    for (std::size_t i = 0; i <= config.length; i++) {
        BSONArray emptyArr;
        DocumentStructureEnumeratorConfig nextConfig(config);
        nextConfig.length = i;
        _enumerateFixedLenArrs(nextConfig, scalarGenerator, emptyArr, &arrs);
    }

    return arrs;
}

std::vector<BSONObj> DocumentStructureEnumerator::enumerateDocs() const {
    BSONObj startDoc;
    std::vector<BSONObj> docs;
    _enumerateDocs(this->_config, this->_scalarGenerator, startDoc, &docs);
    return docs;
}

std::vector<BSONArray> DocumentStructureEnumerator::enumerateArrs() const {
    return _enumerateArrs(this->_config, this->_scalarGenerator);
}

DocumentStructureEnumerator::DocumentStructureEnumerator(DocumentStructureEnumeratorConfig config,
                                                         const ScalarGenerator* scalarGenerator)
    : _config(std::move(config)), _scalarGenerator(scalarGenerator) {
    this->_docs = enumerateDocs();
}
}  // namespace mongo
