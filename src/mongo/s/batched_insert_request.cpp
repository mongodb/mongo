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

#include "mongo/s/batched_insert_request.h"

#include "mongo/db/field_parser.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    using mongoutils::str::stream;

    const std::string BatchedInsertRequest::BATCHED_INSERT_REQUEST = "insert";
    const BSONField<std::string> BatchedInsertRequest::collName("insert");
    const BSONField<std::vector<BSONObj> > BatchedInsertRequest::documents("documents");
    const BSONField<BSONObj> BatchedInsertRequest::writeConcern("writeConcern");
    const BSONField<bool> BatchedInsertRequest::continueOnError("continueOnError", false);
    const BSONField<ChunkVersion> BatchedInsertRequest::shardVersion("shardVersion");
    const BSONField<long long> BatchedInsertRequest::session("session");

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

        if (_isContinueOnErrorSet) builder.append(continueOnError(), _continueOnError);

        if (_shardVersion.get()) {
            // ChunkVersion wants to be an array.
            builder.append(shardVersion(), static_cast<BSONArray>(_shardVersion->toBSON()));
        }

        if (_isSessionSet) builder.append(session(), _session);

        return builder.obj();
    }

    bool BatchedInsertRequest::parseBSON(const BSONObj& source, string* errMsg) {
        clear();

        std::string dummy;
        if (!errMsg) errMsg = &dummy;

        FieldParser::FieldState fieldState;
        fieldState = FieldParser::extract(source, collName, &_collName, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isCollNameSet = fieldState == FieldParser::FIELD_SET;

        fieldState = FieldParser::extract(source, documents, &_documents, errMsg);
        if (fieldState == FieldParser::FIELD_INVALID) return false;
        _isDocumentsSet = fieldState == FieldParser::FIELD_SET;

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

    void BatchedInsertRequest::clear() {
        _collName.clear();
        _isCollNameSet = false;

        _documents.clear();
        _isDocumentsSet =false;

        _writeConcern = BSONObj();
        _isWriteConcernSet = false;

        _continueOnError = false;
        _isContinueOnErrorSet = false;

        _shardVersion.reset();

        _session = 0;
        _isSessionSet = false;

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

        other->_continueOnError = _continueOnError;
        other->_isContinueOnErrorSet = _isContinueOnErrorSet;

        if (other->_shardVersion.get()) _shardVersion->cloneTo(other->_shardVersion.get());

        other->_session = _session;
        other->_isSessionSet = _isSessionSet;
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

    void BatchedInsertRequest::setContinueOnError(bool continueOnError) {
        _continueOnError = continueOnError;
        _isContinueOnErrorSet = true;
    }

    void BatchedInsertRequest::unsetContinueOnError() {
         _isContinueOnErrorSet = false;
     }

    bool BatchedInsertRequest::isContinueOnErrorSet() const {
         return _isContinueOnErrorSet;
    }

    bool BatchedInsertRequest::getContinueOnError() const {
        dassert(_isContinueOnErrorSet);
        return _continueOnError;
    }

    void BatchedInsertRequest::setShardVersion(const ChunkVersion& shardVersion) {
        auto_ptr<ChunkVersion> temp(new ChunkVersion);
        shardVersion.cloneTo(temp.get());
        _shardVersion.reset(temp.release());
    }

    void BatchedInsertRequest::unsetShardVersion() {
        _shardVersion.reset();
     }

    bool BatchedInsertRequest::isShardVersionSet() const {
        return _shardVersion.get() != NULL;
    }

    const ChunkVersion& BatchedInsertRequest::getShardVersion() const {
        dassert(_shardVersion.get());
        return *_shardVersion;
    }

    void BatchedInsertRequest::setSession(long long session) {
        _session = session;
        _isSessionSet = true;
    }

    void BatchedInsertRequest::unsetSession() {
         _isSessionSet = false;
     }

    bool BatchedInsertRequest::isSessionSet() const {
         return _isSessionSet;
    }

    long long BatchedInsertRequest::getSession() const {
        dassert(_isSessionSet);
        return _session;
    }

} // namespace mongo
