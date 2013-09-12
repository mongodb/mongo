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

#include "mongo/s/batched_update_request.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string BatchedUpdateRequest::BATCHED_UPDATE_REQUEST = "update";
        const BSONField<std::string> BatchedUpdateRequest::collName("insert");
        const BSONField<std::vector<BatchedUpdateDocument*> > BatchedUpdateRequest::updates("updates");
        const BSONField<BSONObj> BatchedUpdateRequest::writeConcern("writeConcern");
        const BSONField<bool> BatchedUpdateRequest::continueOnError("continueOnError", false);
        const BSONField<ChunkVersion> BatchedUpdateRequest::shardVersion("shardVersion");
        const BSONField<long long> BatchedUpdateRequest::session("session");

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
        if (!_isCollNameSet) {
            *errMsg = stream() << "missing " << collName.name() << " field";
            return false;
        }
        if (!_isUpdatesSet) {
            *errMsg = stream() << "missing " << updates.name() << " field";
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

    BSONObj BatchedUpdateRequest::toBSON() const {
        BSONObjBuilder builder;

        if (_isCollNameSet) builder.append(collName(), _collName);

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

        if (_isWriteConcernSet) builder.append(writeConcern(), _writeConcern);

        if (_isContinueOnErrorSet) builder.append(continueOnError(), _continueOnError);

        if (_shardVersion.get()) {
            // ChunkVersion wants to be an array.
            builder.append(shardVersion(), static_cast<BSONArray>(_shardVersion->toBSON()));
        }

        if (_isSessionSet) builder.append(session(), _session);

        return builder.obj();
    }

    bool BatchedUpdateRequest::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, collName, &_collName, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isCollNameSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, updates, &_updates, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isUpdatesSet = fieldState == FieldParser::FIELD_SET;

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

    void BatchedUpdateRequest::clear() {
        _collName.clear();
        _isCollNameSet = false;

        unsetUpdates();

        _writeConcern = BSONObj();
        _isWriteConcernSet = false;

        _continueOnError = false;
        _isContinueOnErrorSet = false;

        _shardVersion.reset();

        _session = 0;
        _isSessionSet = false;

    }

    void BatchedUpdateRequest::cloneTo(BatchedUpdateRequest* other) const {
        other->clear();

        other->_collName = _collName;
        other->_isCollNameSet = _isCollNameSet;

        for(std::vector<BatchedUpdateDocument*>::const_iterator it = _updates.begin();
            it != _updates.end();
            ++it) {
            auto_ptr<BatchedUpdateDocument> tempBatchUpdateDocument(new BatchedUpdateDocument);
            (*it)->cloneTo(tempBatchUpdateDocument.get());
            other->addToUpdates(*it);
        }
        other->_isUpdatesSet = _isUpdatesSet;

        other->_writeConcern = _writeConcern;
        other->_isWriteConcernSet = _isWriteConcernSet;

        other->_continueOnError = _continueOnError;
        other->_isContinueOnErrorSet = _isContinueOnErrorSet;

        if (other->_shardVersion.get()) _shardVersion->cloneTo(other->_shardVersion.get());

        other->_session = _session;
        other->_isSessionSet = _isSessionSet;
    }

    std::string BatchedUpdateRequest::toString() const {
        return toBSON().toString();
    }

    void BatchedUpdateRequest::setCollName(const StringData& collName) {
        _collName = collName.toString();
        _isCollNameSet = true;
    }

    void BatchedUpdateRequest::unsetCollName() {
         _isCollNameSet = false;
     }

    bool BatchedUpdateRequest::isCollNameSet() const {
         return _isCollNameSet;
    }

    const std::string& BatchedUpdateRequest::getCollName() const {
        dassert(_isCollNameSet);
        return _collName;
    }

    void BatchedUpdateRequest::setUpdates(const std::vector<BatchedUpdateDocument*>& updates) {
        unsetUpdates();
        for (std::vector<BatchedUpdateDocument*>::const_iterator it = updates.begin();
             it != updates.end();
             ++it) {
            auto_ptr<BatchedUpdateDocument> tempBatchUpdateDocument(new BatchedUpdateDocument);
            (*it)->cloneTo(tempBatchUpdateDocument.get());
            addToUpdates(tempBatchUpdateDocument.release());
        }
        _isUpdatesSet = updates.size() > 0;
    }

    void BatchedUpdateRequest::addToUpdates(BatchedUpdateDocument* updates) {
        _updates.push_back(updates);
        _isUpdatesSet = true;
    }

    void BatchedUpdateRequest::unsetUpdates() {
        for(std::vector<BatchedUpdateDocument*>::iterator it = _updates.begin();
            it != _updates.end();
            ++it) {
            delete *it;
        }
        _updates.clear();
        _isUpdatesSet = false;
    }

    bool BatchedUpdateRequest::isUpdatesSet() const {
        return _isUpdatesSet;
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

    void BatchedUpdateRequest::setWriteConcern(const BSONObj& writeConcern) {
        _writeConcern = writeConcern.getOwned();
        _isWriteConcernSet = true;
    }

    void BatchedUpdateRequest::unsetWriteConcern() {
         _isWriteConcernSet = false;
     }

    bool BatchedUpdateRequest::isWriteConcernSet() const {
         return _isWriteConcernSet;
    }

    const BSONObj& BatchedUpdateRequest::getWriteConcern() const {
        dassert(_isWriteConcernSet);
        return _writeConcern;
    }

    void BatchedUpdateRequest::setContinueOnError(bool continueOnError) {
        _continueOnError = continueOnError;
        _isContinueOnErrorSet = true;
    }

    void BatchedUpdateRequest::unsetContinueOnError() {
         _isContinueOnErrorSet = false;
     }

    bool BatchedUpdateRequest::isContinueOnErrorSet() const {
         return _isContinueOnErrorSet;
    }

    bool BatchedUpdateRequest::getContinueOnError() const {
        dassert(_isContinueOnErrorSet);
        return _continueOnError;
    }

    void BatchedUpdateRequest::setShardVersion(const ChunkVersion& shardVersion) {
        auto_ptr<ChunkVersion> temp(new ChunkVersion);
        shardVersion.cloneTo(temp.get());
        _shardVersion.reset(temp.release());
    }

    void BatchedUpdateRequest::unsetShardVersion() {
        _shardVersion.reset();
     }

    bool BatchedUpdateRequest::isShardVersionSet() const {
        return _shardVersion.get() != NULL;
    }

    const ChunkVersion& BatchedUpdateRequest::getShardVersion() const {
        dassert(_shardVersion.get());
        return *_shardVersion;
    }

    void BatchedUpdateRequest::setSession(long long session) {
        _session = session;
        _isSessionSet = true;
    }

    void BatchedUpdateRequest::unsetSession() {
         _isSessionSet = false;
     }

    bool BatchedUpdateRequest::isSessionSet() const {
         return _isSessionSet;
    }

    long long BatchedUpdateRequest::getSession() const {
        dassert(_isSessionSet);
        return _session;
    }

} // namespace mongo
