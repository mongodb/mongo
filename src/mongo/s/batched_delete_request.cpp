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
 */

#include "mongo/s/batched_delete_request.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string BatchedDeleteRequest::BATCHED_DELETE_REQUEST = "delete";
        const BSONField<std::string> BatchedDeleteRequest::collName("insert");
        const BSONField<std::vector<BatchedDeleteDocument*> > BatchedDeleteRequest::deletes("deletes");
        const BSONField<BSONObj> BatchedDeleteRequest::writeConcern("writeConcern");
        const BSONField<bool> BatchedDeleteRequest::continueOnError("continueOnError", false);
        const BSONField<ChunkVersion> BatchedDeleteRequest::shardVersion("shardVersion");
        const BSONField<long long> BatchedDeleteRequest::session("session");

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

        if (!_isWriteConcernSet) {
            *errMsg = stream() << "missing " << writeConcern.name() << " field";
            return false;
        }

        if (!_isContinueOnErrorSet) {
            *errMsg = stream() << "missing " << continueOnError.name() << " field";
            return false;
        }

        return true;
    }

    BSONObj BatchedDeleteRequest::toBSON() const {
        BSONObjBuilder builder;

        if (_isCollNameSet) builder.append(collName(), _collName);

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

        if (_isWriteConcernSet) builder.append(writeConcern(), _writeConcern);

        if (_isContinueOnErrorSet) builder.append(continueOnError(), _continueOnError);

        if (_shardVersion.get()) {
            // ChunkVersion wants to be an array.
            builder.append(shardVersion(), static_cast<BSONArray>(_shardVersion->toBSON()));
        }

        if (_isSessionSet) builder.append(session(), _session);

        return builder.obj();
    }

    bool BatchedDeleteRequest::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, collName, &_collName, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isCollNameSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, deletes, &_deletes, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isDeletesSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, writeConcern, &_writeConcern, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isWriteConcernSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, continueOnError, &_continueOnError, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isContinueOnErrorSet = fieldState == FieldParser::FIELD_SET;

        ChunkVersion* tempChunkVersion = NULL;
        fieldState = FieldParser::extract(source, shardVersion, &tempChunkVersion, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        if (fieldState == FieldParser::FIELD_SET) _shardVersion.reset(tempChunkVersion);

        fieldState = FieldParser::extract(source, session, &_session, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isSessionSet = fieldState == FieldParser::FIELD_SET;

        return true;
    }

    void BatchedDeleteRequest::clear() {
        _collName.clear();
        _isCollNameSet = false;

        unsetDeletes();

        _writeConcern = BSONObj();
        _isWriteConcernSet = false;

        _continueOnError = false;
        _isContinueOnErrorSet = false;

        _shardVersion.reset();

        _session = 0;
        _isSessionSet = false;

    }

    void BatchedDeleteRequest::cloneTo(BatchedDeleteRequest* other) const {
        other->clear();

        other->_collName = _collName;
        other->_isCollNameSet = _isCollNameSet;

        for(std::vector<BatchedDeleteDocument*>::const_iterator it = _deletes.begin();
            it != _deletes.end();
            ++it) {
            auto_ptr<BatchedDeleteDocument> tempBatchDeleteDocument(new BatchedDeleteDocument);
            (*it)->cloneTo(tempBatchDeleteDocument.get());
            other->addToDeletes(*it);
        }
        other->_isDeletesSet = _isDeletesSet;

        other->_writeConcern = _writeConcern;
        other->_isWriteConcernSet = _isWriteConcernSet;

        other->_continueOnError = _continueOnError;
        other->_isContinueOnErrorSet = _isContinueOnErrorSet;

        if (other->_shardVersion.get()) _shardVersion->cloneTo(other->_shardVersion.get());

        other->_session = _session;
        other->_isSessionSet = _isSessionSet;
    }

    std::string BatchedDeleteRequest::toString() const {
        return toBSON().toString();
    }

    void BatchedDeleteRequest::setCollName(const StringData& collName) {
        _collName = collName.toString();
        _isCollNameSet = true;
    }

    void BatchedDeleteRequest::unsetCollName() {
         _isCollNameSet = false;
     }

    bool BatchedDeleteRequest::isCollNameSet() const {
         return _isCollNameSet;
    }

    const std::string& BatchedDeleteRequest::getCollName() const {
        dassert(_isCollNameSet);
        return _collName;
    }

    void BatchedDeleteRequest::setDeletes(const std::vector<BatchedDeleteDocument*>& deletes) {
        for (std::vector<BatchedDeleteDocument*>::const_iterator it = deletes.begin();
             it != deletes.end();
             ++it) {
            auto_ptr<BatchedDeleteDocument> tempBatchDeleteDocument(new BatchedDeleteDocument);
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
        for(std::vector<BatchedDeleteDocument*>::iterator it = _deletes.begin();
            it != _deletes.end();
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

    void BatchedDeleteRequest::setContinueOnError(bool continueOnError) {
        _continueOnError = continueOnError;
        _isContinueOnErrorSet = true;
    }

    void BatchedDeleteRequest::unsetContinueOnError() {
         _isContinueOnErrorSet = false;
     }

    bool BatchedDeleteRequest::isContinueOnErrorSet() const {
         return _isContinueOnErrorSet;
    }

    bool BatchedDeleteRequest::getContinueOnError() const {
        dassert(_isContinueOnErrorSet);
        return _continueOnError;
    }

    void BatchedDeleteRequest::setShardVersion(const ChunkVersion& shardVersion) {
        auto_ptr<ChunkVersion> temp(new ChunkVersion);
        shardVersion.cloneTo(temp.get());
        _shardVersion.reset(temp.release());
    }

    void BatchedDeleteRequest::unsetShardVersion() {
        _shardVersion.reset();
     }

    bool BatchedDeleteRequest::isShardVersionSet() const {
        return _shardVersion.get() != NULL;
    }

    const ChunkVersion& BatchedDeleteRequest::getShardVersion() const {
        dassert(_shardVersion.get());
        return *_shardVersion;
    }

    void BatchedDeleteRequest::setSession(long long session) {
        _session = session;
        _isSessionSet = true;
    }

    void BatchedDeleteRequest::unsetSession() {
         _isSessionSet = false;
     }

    bool BatchedDeleteRequest::isSessionSet() const {
         return _isSessionSet;
    }

    long long BatchedDeleteRequest::getSession() const {
        dassert(_isSessionSet);
        return _session;
    }

} // namespace mongo
