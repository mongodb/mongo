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

#include "mongo/s/write_ops/batched_delete_request.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::unique_ptr;
using std::string;

using mongoutils::str::stream;

const std::string BatchedDeleteRequest::BATCHED_DELETE_REQUEST = "delete";
const BSONField<std::string> BatchedDeleteRequest::collName("delete");
const BSONField<std::vector<BatchedDeleteDocument*>> BatchedDeleteRequest::deletes("deletes");
const BSONField<BSONObj> BatchedDeleteRequest::writeConcern("writeConcern");
const BSONField<bool> BatchedDeleteRequest::ordered("ordered", true);
const BSONField<BSONObj> BatchedDeleteRequest::metadata("metadata");

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
    if (!_isCollNameSet) {
        *errMsg = stream() << "missing " << collName.name() << " field";
        return false;
    }

    if (!_isDeletesSet) {
        *errMsg = stream() << "missing " << deletes.name() << " field";
        return false;
    }

    return true;
}

BSONObj BatchedDeleteRequest::toBSON() const {
    BSONObjBuilder builder;

    if (_isCollNameSet)
        builder.append(collName(), _collName);

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

    if (_isWriteConcernSet)
        builder.append(writeConcern(), _writeConcern);

    if (_isOrderedSet)
        builder.append(ordered(), _ordered);

    if (_metadata)
        builder.append(metadata(), _metadata->toBSON());

    return builder.obj();
}

bool BatchedDeleteRequest::parseBSON(const BSONObj& source, string* errMsg) {
    clear();

    std::string dummy;
    if (!errMsg)
        errMsg = &dummy;

    FieldParser::FieldState fieldState;
    std::string collNameTemp;
    fieldState = FieldParser::extract(source, collName, &collNameTemp, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _collName = NamespaceString(collNameTemp);
    _isCollNameSet = fieldState == FieldParser::FIELD_SET;

    fieldState = FieldParser::extract(source, deletes, &_deletes, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isDeletesSet = fieldState == FieldParser::FIELD_SET;

    fieldState = FieldParser::extract(source, writeConcern, &_writeConcern, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isWriteConcernSet = fieldState == FieldParser::FIELD_SET;

    fieldState = FieldParser::extract(source, ordered, &_ordered, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;
    _isOrderedSet = fieldState == FieldParser::FIELD_SET;

    BSONObj metadataObj;
    fieldState = FieldParser::extract(source, metadata, &metadataObj, errMsg);
    if (fieldState == FieldParser::FIELD_INVALID)
        return false;

    if (!metadataObj.isEmpty()) {
        _metadata.reset(new BatchedRequestMetadata());
        if (!_metadata->parseBSON(metadataObj, errMsg)) {
            return false;
        }
    }

    return true;
}

void BatchedDeleteRequest::clear() {
    _collName = NamespaceString();
    _isCollNameSet = false;

    unsetDeletes();

    _writeConcern = BSONObj();
    _isWriteConcernSet = false;

    _ordered = false;
    _isOrderedSet = false;

    _metadata.reset();
}

void BatchedDeleteRequest::cloneTo(BatchedDeleteRequest* other) const {
    other->clear();

    other->_collName = _collName;
    other->_isCollNameSet = _isCollNameSet;

    for (std::vector<BatchedDeleteDocument*>::const_iterator it = _deletes.begin();
         it != _deletes.end();
         ++it) {
        unique_ptr<BatchedDeleteDocument> tempBatchDeleteDocument(new BatchedDeleteDocument);
        (*it)->cloneTo(tempBatchDeleteDocument.get());
        other->addToDeletes(tempBatchDeleteDocument.release());
    }
    other->_isDeletesSet = _isDeletesSet;

    other->_writeConcern = _writeConcern;
    other->_isWriteConcernSet = _isWriteConcernSet;

    other->_ordered = _ordered;
    other->_isOrderedSet = _isOrderedSet;

    if (_metadata) {
        other->_metadata.reset(new BatchedRequestMetadata());
        _metadata->cloneTo(other->_metadata.get());
    }
}

std::string BatchedDeleteRequest::toString() const {
    return toBSON().toString();
}

void BatchedDeleteRequest::setCollName(StringData collName) {
    _collName = NamespaceString(collName);
    _isCollNameSet = true;
}

const std::string& BatchedDeleteRequest::getCollName() const {
    dassert(_isCollNameSet);
    return _collName.ns();
}

void BatchedDeleteRequest::setCollNameNS(const NamespaceString& collName) {
    _collName = collName;
    _isCollNameSet = true;
}

const NamespaceString& BatchedDeleteRequest::getCollNameNS() const {
    dassert(_isCollNameSet);
    return _collName;
}

const NamespaceString& BatchedDeleteRequest::getTargetingNSS() const {
    return getCollNameNS();
}

void BatchedDeleteRequest::setDeletes(const std::vector<BatchedDeleteDocument*>& deletes) {
    for (std::vector<BatchedDeleteDocument*>::const_iterator it = deletes.begin();
         it != deletes.end();
         ++it) {
        unique_ptr<BatchedDeleteDocument> tempBatchDeleteDocument(new BatchedDeleteDocument);
        (*it)->cloneTo(tempBatchDeleteDocument.get());
        addToDeletes(tempBatchDeleteDocument.release());
    }
    _isDeletesSet = deletes.size() > 0;
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

bool BatchedDeleteRequest::isDeletesSet() const {
    return _isDeletesSet;
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

void BatchedDeleteRequest::setWriteConcern(const BSONObj& writeConcern) {
    _writeConcern = writeConcern.getOwned();
    _isWriteConcernSet = true;
}

void BatchedDeleteRequest::unsetWriteConcern() {
    _isWriteConcernSet = false;
}

bool BatchedDeleteRequest::isWriteConcernSet() const {
    return _isWriteConcernSet;
}

const BSONObj& BatchedDeleteRequest::getWriteConcern() const {
    dassert(_isWriteConcernSet);
    return _writeConcern;
}

void BatchedDeleteRequest::setOrdered(bool ordered) {
    _ordered = ordered;
    _isOrderedSet = true;
}

void BatchedDeleteRequest::unsetOrdered() {
    _isOrderedSet = false;
}

bool BatchedDeleteRequest::isOrderedSet() const {
    return _isOrderedSet;
}

bool BatchedDeleteRequest::getOrdered() const {
    if (_isOrderedSet) {
        return _ordered;
    } else {
        return ordered.getDefault();
    }
}

void BatchedDeleteRequest::setMetadata(BatchedRequestMetadata* metadata) {
    _metadata.reset(metadata);
}

void BatchedDeleteRequest::unsetMetadata() {
    _metadata.reset();
}

bool BatchedDeleteRequest::isMetadataSet() const {
    return _metadata.get();
}

BatchedRequestMetadata* BatchedDeleteRequest::getMetadata() const {
    return _metadata.get();
}

}  // namespace mongo
