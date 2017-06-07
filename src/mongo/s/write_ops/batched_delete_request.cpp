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

#include "mongo/s/write_ops/batched_delete_request.h"

#include "mongo/db/ops/write_ops.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const BSONField<std::string> BatchedDeleteRequest::collName("delete");
const BSONField<std::vector<BatchedDeleteDocument*>> BatchedDeleteRequest::deletes("deletes");

BatchedDeleteRequest::BatchedDeleteRequest() {
    clear();
}

BatchedDeleteRequest::~BatchedDeleteRequest() {
    unsetDeletes();
}

bool BatchedDeleteRequest::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isNSSet) {
        *errMsg = str::stream() << "missing " << collName.name() << " field";
        return false;
    }

    if (!_isDeletesSet) {
        *errMsg = str::stream() << "missing " << deletes.name() << " field";
        return false;
    }

    return true;
}

BSONObj BatchedDeleteRequest::toBSON() const {
    BSONObjBuilder builder;

    if (_isNSSet)
        builder.append(collName(), _ns.coll());

    if (_isDeletesSet) {
        BSONArrayBuilder deletesBuilder(builder.subarrayStart(deletes()));
        for (std::vector<BatchedDeleteDocument*>::const_iterator it = _deletes.begin();
             it != _deletes.end();
             ++it) {
            BSONObj deleteDocument = (*it)->toBSON();
            deletesBuilder.append(deleteDocument);
        }
        deletesBuilder.done();
    }

    return builder.obj();
}

void BatchedDeleteRequest::parseRequest(const OpMsgRequest& request) {
    clear();

    auto deleteOp = DeleteOp::parse(request);

    _ns = std::move(deleteOp.getNamespace());
    _isNSSet = true;

    for (auto&& deleteEntry : deleteOp.getDeletes()) {
        _deletes.push_back(new BatchedDeleteDocument());  // _deletes takes ownership.
        std::string errMsg;
        uassert(ErrorCodes::FailedToParse,
                errMsg,
                _deletes.back()->parseBSON(deleteEntry.toBSON(), &errMsg) &&
                    _deletes.back()->isValid(&errMsg));
    }

    _isDeletesSet = true;
}

void BatchedDeleteRequest::clear() {
    _ns = NamespaceString();
    _isNSSet = false;

    unsetDeletes();
}

std::string BatchedDeleteRequest::toString() const {
    return toBSON().toString();
}

void BatchedDeleteRequest::setNS(NamespaceString ns) {
    _ns = std::move(ns);
    _isNSSet = true;
}

const NamespaceString& BatchedDeleteRequest::getNS() const {
    dassert(_isNSSet);
    return _ns;
}

void BatchedDeleteRequest::addToDeletes(BatchedDeleteDocument* deletes) {
    _deletes.push_back(deletes);
    _isDeletesSet = true;
}

void BatchedDeleteRequest::unsetDeletes() {
    for (std::vector<BatchedDeleteDocument*>::iterator it = _deletes.begin(); it != _deletes.end();
         ++it) {
        delete *it;
    }
    _deletes.clear();
    _isDeletesSet = false;
}

size_t BatchedDeleteRequest::sizeDeletes() const {
    return _deletes.size();
}

const std::vector<BatchedDeleteDocument*>& BatchedDeleteRequest::getDeletes() const {
    dassert(_isDeletesSet);
    return _deletes;
}

const BatchedDeleteDocument* BatchedDeleteRequest::getDeletesAt(size_t pos) const {
    dassert(_isDeletesSet);
    dassert(_deletes.size() > pos);
    return _deletes.at(pos);
}

}  // namespace mongo
