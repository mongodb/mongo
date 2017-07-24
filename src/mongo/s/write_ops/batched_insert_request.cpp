/**
 *    Copyright (C) 2013 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/write_ops/batched_insert_request.h"

#include "mongo/db/ops/write_ops.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const BSONField<std::string> BatchedInsertRequest::collName("insert");
const BSONField<std::vector<BSONObj>> BatchedInsertRequest::documents("documents");

BatchedInsertRequest::BatchedInsertRequest() {
    clear();
}

bool BatchedInsertRequest::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isNSSet) {
        *errMsg = str::stream() << "missing " << collName.name() << " field";
        return false;
    }

    if (!_isDocumentsSet) {
        *errMsg = str::stream() << "missing " << documents.name() << " field";
        return false;
    }

    return true;
}

BSONObj BatchedInsertRequest::toBSON() const {
    BSONObjBuilder builder;

    if (_isNSSet)
        builder.append(collName(), _ns.coll());

    if (_isDocumentsSet) {
        BSONArrayBuilder documentsBuilder(builder.subarrayStart(documents()));
        for (std::vector<BSONObj>::const_iterator it = _documents.begin(); it != _documents.end();
             ++it) {
            documentsBuilder.append(*it);
        }
        documentsBuilder.done();
    }

    return builder.obj();
}

void BatchedInsertRequest::parseRequest(const OpMsgRequest& request) {
    clear();

    auto insertOp = InsertOp::parse(request);

    _ns = std::move(insertOp.getNamespace());
    _isNSSet = true;

    for (auto&& documentEntry : insertOp.getDocuments()) {
        _documents.push_back(documentEntry.getOwned());
    }

    _isDocumentsSet = true;
}

void BatchedInsertRequest::clear() {
    _ns = NamespaceString();
    _isNSSet = false;

    _documents.clear();
    _isDocumentsSet = false;
}

void BatchedInsertRequest::cloneTo(BatchedInsertRequest* other) const {
    other->clear();

    other->_ns = _ns;
    other->_isNSSet = _isNSSet;

    for (std::vector<BSONObj>::const_iterator it = _documents.begin(); it != _documents.end();
         ++it) {
        other->addToDocuments(*it);
    }
    other->_isDocumentsSet = _isDocumentsSet;
}

std::string BatchedInsertRequest::toString() const {
    return toBSON().toString();
}

void BatchedInsertRequest::setNS(NamespaceString ns) {
    _ns = std::move(ns);
    _isNSSet = true;
}

const NamespaceString& BatchedInsertRequest::getNS() const {
    dassert(_isNSSet);
    return _ns;
}

void BatchedInsertRequest::addToDocuments(const BSONObj& documents) {
    _documents.push_back(documents);
    _isDocumentsSet = true;
}

size_t BatchedInsertRequest::sizeDocuments() const {
    return _documents.size();
}

const std::vector<BSONObj>& BatchedInsertRequest::getDocuments() const {
    dassert(_isDocumentsSet);
    return _documents;
}

const BSONObj& BatchedInsertRequest::getDocumentsAt(size_t pos) const {
    dassert(_isDocumentsSet);
    dassert(_documents.size() > pos);
    return _documents.at(pos);
}

void BatchedInsertRequest::setDocumentAt(size_t pos, const BSONObj& doc) {
    dassert(_isDocumentsSet);
    dassert(_documents.size() > pos);
    _documents[pos] = doc;
}

}  // namespace mongo
