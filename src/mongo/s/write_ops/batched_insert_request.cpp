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

#include "mongo/s/write_ops/batched_insert_request.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string BatchedInsertRequest::BATCHED_INSERT_REQUEST = "insert";
    const BSONField<std::string> BatchedInsertRequest::collName("insert");
    const BSONField<std::vector<BSONObj> > BatchedInsertRequest::documents("documents");
    const BSONField<BSONObj> BatchedInsertRequest::writeConcern("writeConcern");
    const BSONField<bool> BatchedInsertRequest::ordered("ordered", true);
    const BSONField<BSONObj> BatchedInsertRequest::metadata("metadata");

    BatchedInsertRequest::BatchedInsertRequest() {
        clear();
    }

    BatchedInsertRequest::~BatchedInsertRequest() {
    }

    bool BatchedInsertRequest::isValid(std::string* errMsg) const {
        std::string dummy;
        if (errMsg == NULL) {
            errMsg = &dummy;
        }

        // All the mandatory fields must be present.
        if (!_isCollNameSet) {
            *errMsg = stream() << "missing " << collName.name() << " field";
            return false;
        }

        if (!_isDocumentsSet) {
            *errMsg = stream() << "missing " << documents.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj BatchedInsertRequest::toBSON() const {
        BSONObjBuilder builder;

        if (_isCollNameSet) builder.append(collName(), _collName);

        if (_isDocumentsSet) {
            BSONArrayBuilder documentsBuilder(builder.subarrayStart(documents()));
            for (std::vector<BSONObj>::const_iterator it = _documents.begin();
                 it != _documents.end();
                 ++it) {
                documentsBuilder.append(*it);
            }
            documentsBuilder.done();
        }

        if (_isWriteConcernSet) builder.append(writeConcern(), _writeConcern);

        if (_isOrderedSet) builder.append(ordered(), _ordered);

        if (_metadata) builder.append(metadata(), _metadata->toBSON());

        return builder.obj();
    }

    bool BatchedInsertRequest::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        BSONObjIterator sourceIt(source);

        while ( sourceIt.more() ) {

            BSONElement sourceEl = sourceIt.next();

            if ( collName() == sourceEl.fieldName() ) {
                FieldParser::FieldState fieldState =
                    FieldParser::extract( sourceEl, collName, &_collName, errMsg );
                if (fieldState == FieldParser::FIELD_INVALID) return false;
                _isCollNameSet = fieldState == FieldParser::FIELD_SET;
            }
            else if ( documents() == sourceEl.fieldName() ) {
                FieldParser::FieldState fieldState =
                    FieldParser::extract( sourceEl, documents, &_documents, errMsg );
                if ( fieldState == FieldParser::FIELD_INVALID ) return false;
                _isDocumentsSet = fieldState == FieldParser::FIELD_SET;
            }
            else if ( writeConcern() == sourceEl.fieldName() ) {
                FieldParser::FieldState fieldState =
                    FieldParser::extract(sourceEl, writeConcern, &_writeConcern, errMsg);
                if (fieldState == FieldParser::FIELD_INVALID) return false;
                _isWriteConcernSet = fieldState == FieldParser::FIELD_SET;
            }
            else if ( ordered() == sourceEl.fieldName() ) {
                FieldParser::FieldState fieldState =
                    FieldParser::extract(sourceEl, ordered, &_ordered, errMsg);
                if (fieldState == FieldParser::FIELD_INVALID) return false;
                _isOrderedSet = fieldState == FieldParser::FIELD_SET;
            }
            else if ( metadata() == sourceEl.fieldName() ) {
                BSONObj metadataObj;
                FieldParser::FieldState fieldState =
                    FieldParser::extract(sourceEl, metadata, &metadataObj, errMsg);
                if (fieldState == FieldParser::FIELD_INVALID) return false;

                if (!metadataObj.isEmpty()) {
                    _metadata.reset(new BatchedRequestMetadata());
                    if (!_metadata->parseBSON(metadataObj, errMsg)) {
                        return false;
                    }
                }
            }
        }

        return true;
    }

    void BatchedInsertRequest::clear() {
        _collName.clear();
        _isCollNameSet = false;

        _documents.clear();
        _isDocumentsSet =false;

        _writeConcern = BSONObj();
        _isWriteConcernSet = false;

        _ordered = false;
        _isOrderedSet = false;

        _metadata.reset();
    }

    void BatchedInsertRequest::cloneTo(BatchedInsertRequest* other) const {
        other->clear();

        other->_collName = _collName;
        other->_isCollNameSet = _isCollNameSet;

        for(std::vector<BSONObj>::const_iterator it = _documents.begin();
            it != _documents.end();
            ++it) {
            other->addToDocuments(*it);
        }
        other->_isDocumentsSet = _isDocumentsSet;

        other->_writeConcern = _writeConcern;
        other->_isWriteConcernSet = _isWriteConcernSet;

        other->_ordered = _ordered;
        other->_isOrderedSet = _isOrderedSet;

        if (_metadata) {
            other->_metadata.reset(new BatchedRequestMetadata());
            _metadata->cloneTo(other->_metadata.get());
        }
    }

    std::string BatchedInsertRequest::toString() const {
        return toBSON().toString();
    }

    void BatchedInsertRequest::setCollName(const StringData& collName) {
        _collName = collName.toString();
        _isCollNameSet = true;
    }

    void BatchedInsertRequest::unsetCollName() {
         _isCollNameSet = false;
     }

    bool BatchedInsertRequest::isCollNameSet() const {
         return _isCollNameSet;
    }

    const std::string& BatchedInsertRequest::getCollName() const {
        dassert(_isCollNameSet);
        return _collName;
    }

    void BatchedInsertRequest::setDocuments(const std::vector<BSONObj>& documents) {
        for (std::vector<BSONObj>::const_iterator it = documents.begin();
             it != documents.end();
             ++it) {
            addToDocuments((*it).getOwned());
        }
        _isDocumentsSet = documents.size() > 0;
    }

    void BatchedInsertRequest::addToDocuments(const BSONObj& documents) {
        _documents.push_back(documents);
        _isDocumentsSet = true;
    }

    void BatchedInsertRequest::unsetDocuments() {
        _documents.clear();
        _isDocumentsSet = false;
    }

    bool BatchedInsertRequest::isDocumentsSet() const {
        return _isDocumentsSet;
    }

    size_t BatchedInsertRequest::sizeDocuments() const {
        return _documents.size();
    }

    const std::vector<BSONObj>& BatchedInsertRequest::getDocuments() const {
        dassert(_isDocumentsSet);
        return _documents;
    }

    std::vector<BSONObj>& BatchedInsertRequest::getDocuments() {
        dassert(_isDocumentsSet);
        return _documents;
    }

    const BSONObj& BatchedInsertRequest::getDocumentsAt(size_t pos) const {
        dassert(_isDocumentsSet);
        dassert(_documents.size() > pos);
        return _documents.at(pos);
    }

    void BatchedInsertRequest::setWriteConcern(const BSONObj& writeConcern) {
        _writeConcern = writeConcern.getOwned();
        _isWriteConcernSet = true;
    }

    void BatchedInsertRequest::unsetWriteConcern() {
         _isWriteConcernSet = false;
     }

    bool BatchedInsertRequest::isWriteConcernSet() const {
         return _isWriteConcernSet;
    }

    const BSONObj& BatchedInsertRequest::getWriteConcern() const {
        dassert(_isWriteConcernSet);
        return _writeConcern;
    }

    void BatchedInsertRequest::setOrdered(bool ordered) {
        _ordered = ordered;
        _isOrderedSet = true;
    }

    void BatchedInsertRequest::unsetOrdered() {
         _isOrderedSet = false;
     }

    bool BatchedInsertRequest::isOrderedSet() const {
         return _isOrderedSet;
    }

    bool BatchedInsertRequest::getOrdered() const {
        if (_isOrderedSet) {
            return _ordered;
        }
        else {
            return ordered.getDefault();
        }
    }

    void BatchedInsertRequest::setMetadata(BatchedRequestMetadata* metadata) {
        _metadata.reset(metadata);
    }

    BatchedRequestMetadata* BatchedInsertRequest::getMetadata() const {
        return _metadata.get();
    }

} // namespace mongo
