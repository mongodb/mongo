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

#include "mongo/s/write_ops/batched_update_request.h"

#include "mongo/db/ops/write_ops.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

const BSONField<std::string> BatchedUpdateRequest::collName("update");
const BSONField<std::vector<BatchedUpdateDocument*>> BatchedUpdateRequest::updates("updates");

BatchedUpdateRequest::BatchedUpdateRequest() {
    clear();
}

BatchedUpdateRequest::~BatchedUpdateRequest() {
    unsetUpdates();
}

bool BatchedUpdateRequest::isValid(std::string* errMsg) const {
    std::string dummy;
    if (errMsg == NULL) {
        errMsg = &dummy;
    }

    // All the mandatory fields must be present.
    if (!_isNSSet) {
        *errMsg = str::stream() << "missing " << collName.name() << " field";
        return false;
    }

    if (!_isUpdatesSet) {
        *errMsg = str::stream() << "missing " << updates.name() << " field";
        return false;
    }

    return true;
}

BSONObj BatchedUpdateRequest::toBSON() const {
    BSONObjBuilder builder;

    if (_isNSSet)
        builder.append(collName(), _ns.coll());

    if (_isUpdatesSet) {
        BSONArrayBuilder updatesBuilder(builder.subarrayStart(updates()));
        for (std::vector<BatchedUpdateDocument*>::const_iterator it = _updates.begin();
             it != _updates.end();
             ++it) {
            BSONObj updateDocument = (*it)->toBSON();
            updatesBuilder.append(updateDocument);
        }
        updatesBuilder.done();
    }

    return builder.obj();
}

void BatchedUpdateRequest::parseRequest(const OpMsgRequest& request) {
    clear();

    auto updateOp = UpdateOp::parse(request);

    _ns = std::move(updateOp.getNamespace());
    _isNSSet = true;

    for (auto&& updateEntry : updateOp.getUpdates()) {
        _updates.push_back(new BatchedUpdateDocument());
        std::string errMsg;
        uassert(ErrorCodes::FailedToParse,
                errMsg,
                _updates.back()->parseBSON(updateEntry.toBSON(), &errMsg) &&
                    _updates.back()->isValid(&errMsg));
    }

    _isUpdatesSet = true;
}

void BatchedUpdateRequest::clear() {
    _ns = NamespaceString();
    _isNSSet = false;

    unsetUpdates();
}

std::string BatchedUpdateRequest::toString() const {
    return toBSON().toString();
}

void BatchedUpdateRequest::setNS(NamespaceString ns) {
    _ns = std::move(ns);
    _isNSSet = true;
}

const NamespaceString& BatchedUpdateRequest::getNS() const {
    dassert(_isNSSet);
    return _ns;
}

void BatchedUpdateRequest::addToUpdates(BatchedUpdateDocument* updates) {
    _updates.push_back(updates);
    _isUpdatesSet = true;
}

void BatchedUpdateRequest::unsetUpdates() {
    for (std::vector<BatchedUpdateDocument*>::iterator it = _updates.begin(); it != _updates.end();
         ++it) {
        delete *it;
    }
    _updates.clear();
    _isUpdatesSet = false;
}

size_t BatchedUpdateRequest::sizeUpdates() const {
    return _updates.size();
}

const std::vector<BatchedUpdateDocument*>& BatchedUpdateRequest::getUpdates() const {
    dassert(_isUpdatesSet);
    return _updates;
}

const BatchedUpdateDocument* BatchedUpdateRequest::getUpdatesAt(size_t pos) const {
    dassert(_isUpdatesSet);
    dassert(_updates.size() > pos);
    return _updates.at(pos);
}

}  // namespace mongo
