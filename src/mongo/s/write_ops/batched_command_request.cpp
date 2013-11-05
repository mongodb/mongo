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

#include "mongo/s/write_ops/batched_command_request.h"

#include "mongo/bson/bsonobjiterator.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

    BatchedCommandRequest::BatchedCommandRequest( BatchType batchType ) :
            _batchType( batchType ) {
        switch ( getBatchType() ) {
        case BatchedCommandRequest::BatchType_Insert:
            _insertReq.reset( new BatchedInsertRequest );
            return;
        case BatchedCommandRequest::BatchType_Update:
            _updateReq.reset( new BatchedUpdateRequest );
            return;
        default:
            dassert( getBatchType() == BatchedCommandRequest::BatchType_Delete );
            _deleteReq.reset( new BatchedDeleteRequest );
            return;
        }
    }

// This macro just invokes a given method on one of the three types of ops with parameters
#define INVOKE(M,...) \
{\
    switch ( getBatchType() ) {\
    case BatchedCommandRequest::BatchType_Insert:\
        return _insertReq->M(__VA_ARGS__);\
    case BatchedCommandRequest::BatchType_Update:\
        return _updateReq->M(__VA_ARGS__);\
    default:\
        dassert( getBatchType() == BatchedCommandRequest::BatchType_Delete );\
        return _deleteReq->M(__VA_ARGS__);\
    }\
}

    BatchedCommandRequest::BatchType BatchedCommandRequest::getBatchType() const {
        return _batchType;
    }

    BatchedInsertRequest* BatchedCommandRequest::getInsertRequest() const {
        return _insertReq.get();
    }

    BatchedUpdateRequest* BatchedCommandRequest::getUpdateRequest() const {
        return _updateReq.get();
    }

    BatchedDeleteRequest* BatchedCommandRequest::getDeleteRequest() const {
        return _deleteReq.get();
    }

    bool BatchedCommandRequest::isInsertIndexRequest() const {
        if ( _batchType != BatchedCommandRequest::BatchType_Insert ) return false;
        return NamespaceString( getNS() ).isSystemDotIndexes();
    }

    static bool extractUniqueIndex( const BSONObj& indexDesc ) {
        return indexDesc["unique"].trueValue();
    }

    bool BatchedCommandRequest::isUniqueIndexRequest() const {
        if ( !isInsertIndexRequest() ) return false;
        return extractUniqueIndex( getInsertRequest()->getDocumentsAt( 0 ) );
    }

    static void extractIndexNSS( const BSONObj& indexDesc, NamespaceString* indexNSS ) {
        *indexNSS = NamespaceString( indexDesc["ns"].str() );
    }

    string BatchedCommandRequest::getTargetingNS() const {
        if ( !isInsertIndexRequest() ) return getNS();
        NamespaceString nss;
        extractIndexNSS( getInsertRequest()->getDocumentsAt( 0 ), &nss );
        return nss.toString();
    }

    static BSONObj extractIndexKeyPattern( const BSONObj& indexDesc ) {
        return indexDesc["key"].Obj();
    }

    BSONObj BatchedCommandRequest::getIndexKeyPattern() const {
        dassert( isInsertIndexRequest() );
        return extractIndexKeyPattern( getInsertRequest()->getDocumentsAt( 0 ) );
    }

    bool BatchedCommandRequest::isVerboseWC() const {
        if ( !isWriteConcernSet() ) {
            return true;
        }

        BSONObj writeConcern = getWriteConcern();
        BSONElement wElem = writeConcern["w"];
        if ( !wElem.isNumber() || wElem.Number() != 0 ) {
            return true;
        }

        return false;
    }

    void BatchedCommandRequest::cloneTo( BatchedCommandRequest* other ) const {
        other->_insertReq.reset();
        other->_updateReq.reset();
        other->_deleteReq.reset();
        other->_batchType = _batchType;

        switch ( getBatchType() ) {
        case BatchedCommandRequest::BatchType_Insert:
            other->_insertReq.reset( new BatchedInsertRequest );
            _insertReq->cloneTo( other->_insertReq.get() );
            return;
        case BatchedCommandRequest::BatchType_Update:
            other->_updateReq.reset( new BatchedUpdateRequest );
            _updateReq->cloneTo( other->_updateReq.get() );
            return;
        default:
            dassert( getBatchType() == BatchedCommandRequest::BatchType_Delete );
            other->_deleteReq.reset( new BatchedDeleteRequest );
            _deleteReq->cloneTo( other->_deleteReq.get() );
            return;
        }
    }

    bool BatchedCommandRequest::isValid( std::string* errMsg ) const {
        INVOKE( isValid, errMsg );
    }

    BSONObj BatchedCommandRequest::toBSON() const {
        INVOKE( toBSON );
    }

    bool BatchedCommandRequest::parseBSON( const BSONObj& source, std::string* errMsg ) {
        INVOKE( parseBSON, source, errMsg );
    }

    void BatchedCommandRequest::clear() {
        INVOKE( clear );
    }

    std::string BatchedCommandRequest::toString() const {
        INVOKE( toString );
    }

    void BatchedCommandRequest::setNS( const StringData& collName ) {
        INVOKE( setCollName, collName );
    }

    void BatchedCommandRequest::unsetNS() {
        INVOKE( unsetCollName );
    }

    bool BatchedCommandRequest::isNSSet() const {
        INVOKE( isCollNameSet );
    }

    const std::string& BatchedCommandRequest::getNS() const {
        INVOKE( getCollName );
    }

    void BatchedCommandRequest::setWriteOps( const std::vector<BSONObj>& writeOps ) {
        switch ( getBatchType() ) {
        case BatchedCommandRequest::BatchType_Insert:
            _insertReq->setDocuments( writeOps );
            return;
        case BatchedCommandRequest::BatchType_Update:
            _updateReq->unsetUpdates();
            for ( std::vector<BSONObj>::const_iterator it = writeOps.begin(); it != writeOps.end();
                    ++it ) {
                auto_ptr<BatchedUpdateDocument> updateDoc( new BatchedUpdateDocument );
                string errMsg;
                bool parsed = updateDoc->parseBSON( *it, &errMsg ) && updateDoc->isValid( &errMsg );
                (void) parsed; // Suppress warning in non-debug
                dassert( parsed );
                _updateReq->addToUpdates( updateDoc.release() );
            }
            return;
        default:
            dassert( getBatchType() == BatchedCommandRequest::BatchType_Delete );
            _deleteReq->unsetDeletes();
            for ( std::vector<BSONObj>::const_iterator it = writeOps.begin(); it != writeOps.end();
                    ++it ) {
                auto_ptr<BatchedDeleteDocument> deleteDoc( new BatchedDeleteDocument );
                string errMsg;
                bool parsed = deleteDoc->parseBSON( *it, &errMsg ) && deleteDoc->isValid( &errMsg );
                (void) parsed; // Suppress warning in non-debug
                dassert( parsed );
                _deleteReq->addToDeletes( deleteDoc.release() );
            }
            return;
        }
    }

    void BatchedCommandRequest::unsetWriteOps() {
        switch ( getBatchType() ) {
        case BatchedCommandRequest::BatchType_Insert:
            _insertReq->unsetDocuments();
            return;
        case BatchedCommandRequest::BatchType_Update:
            _updateReq->unsetUpdates();
            return;
        default:
            dassert( getBatchType() == BatchedCommandRequest::BatchType_Delete );
            _deleteReq->unsetDeletes();
        }
    }

    bool BatchedCommandRequest::isWriteOpsSet() const {
        switch ( getBatchType() ) {
        case BatchedCommandRequest::BatchType_Insert:
            return _insertReq->isDocumentsSet();
        case BatchedCommandRequest::BatchType_Update:
            return _updateReq->isUpdatesSet();
        default:
            dassert( getBatchType() == BatchedCommandRequest::BatchType_Delete );
            return _deleteReq->isDeletesSet();
        }
    }

    std::size_t BatchedCommandRequest::sizeWriteOps() const {
        switch ( getBatchType() ) {
        case BatchedCommandRequest::BatchType_Insert:
            return _insertReq->sizeDocuments();
        case BatchedCommandRequest::BatchType_Update:
            return _updateReq->sizeUpdates();
        default:
            return _deleteReq->sizeDeletes();
        }
    }

    std::vector<BSONObj> BatchedCommandRequest::getWriteOps() const {
        vector<BSONObj> writeOps;
        switch ( getBatchType() ) {
        case BatchedCommandRequest::BatchType_Insert:
            return _insertReq->getDocuments();
        case BatchedCommandRequest::BatchType_Update:
            for ( std::vector<BatchedUpdateDocument*>::const_iterator it = _updateReq->getUpdates()
                    .begin(); it != _updateReq->getUpdates().end(); ++it ) {
                writeOps.push_back( ( *it )->toBSON() );
            }
            return writeOps;
        default:
            dassert( getBatchType() == BatchedCommandRequest::BatchType_Delete );
            for ( std::vector<BatchedDeleteDocument*>::const_iterator it = _deleteReq->getDeletes()
                    .begin(); it != _deleteReq->getDeletes().end(); ++it ) {
                writeOps.push_back( ( *it )->toBSON() );
            }
            return writeOps;
        }
    }

    void BatchedCommandRequest::setWriteConcern( const BSONObj& writeConcern ) {
        INVOKE( setWriteConcern, writeConcern );
    }

    void BatchedCommandRequest::unsetWriteConcern() {
        INVOKE( unsetWriteConcern );
    }

    bool BatchedCommandRequest::isWriteConcernSet() const {
        INVOKE( isWriteConcernSet );
    }

    const BSONObj& BatchedCommandRequest::getWriteConcern() const {
        INVOKE( getWriteConcern );
    }

    void BatchedCommandRequest::setOrdered( bool continueOnError ) {
        INVOKE( setOrdered, continueOnError );
    }

    void BatchedCommandRequest::unsetOrdered() {
        INVOKE( unsetOrdered );
    }

    bool BatchedCommandRequest::isOrderedSet() const {
        INVOKE( isOrderedSet );
    }

    bool BatchedCommandRequest::getOrdered() const {
        INVOKE( getOrdered );
    }

    void BatchedCommandRequest::setShardName( const StringData& shardName ) {
        INVOKE( setShardName, shardName );
    }

    void BatchedCommandRequest::unsetShardName() {
        INVOKE( unsetShardName );
    }

    bool BatchedCommandRequest::isShardNameSet() const {
        INVOKE( isShardNameSet );
    }

    const string& BatchedCommandRequest::getShardName() const {
        INVOKE( getShardName );
    }

    void BatchedCommandRequest::setShardVersion( const ChunkVersion& shardVersion ) {
        INVOKE( setShardVersion, shardVersion );
    }

    void BatchedCommandRequest::unsetShardVersion() {
        INVOKE( unsetShardVersion );
    }

    bool BatchedCommandRequest::isShardVersionSet() const {
        INVOKE( isShardVersionSet );
    }

    const ChunkVersion& BatchedCommandRequest::getShardVersion() const {
        INVOKE( getShardVersion );
    }

    void BatchedCommandRequest::setSession( long long sessionId ) {
        INVOKE( setSession, sessionId );
    }

    void BatchedCommandRequest::unsetSession() {
        INVOKE( unsetSession );
    }

    bool BatchedCommandRequest::isSessionSet() const {
        INVOKE( isSessionSet );
    }

    long long BatchedCommandRequest::getSession() const {
        INVOKE( getSession );
    }

    bool BatchedCommandRequest::containsUpserts( const BSONObj& writeCmdObj ) {

        BSONElement updatesEl = writeCmdObj[BatchedUpdateRequest::updates()];
        if ( updatesEl.type() != Array ) {
            return false;
        }

        BSONObjIterator it( updatesEl.Obj() );
        while ( it.more() ) {
            BSONElement updateEl = it.next();
            if ( !updateEl.isABSONObj() ) continue;
            if ( updateEl.Obj()[BatchedUpdateDocument::upsert()].trueValue() ) return true;
        }

        return false;
    }

    bool BatchedCommandRequest::getIndexedNS( const BSONObj& writeCmdObj,
                                              string* nsToIndex,
                                              string* errMsg ) {

        BSONElement documentsEl = writeCmdObj[BatchedInsertRequest::documents()];
        if ( documentsEl.type() != Array ) {
            *errMsg = "index write batch is invalid";
            return false;
        }

        BSONObjIterator it( documentsEl.Obj() );
        if ( !it.more() ) {
            *errMsg = "index write batch is empty";
            return false;
        }

        BSONElement indexDescEl = it.next();
        *nsToIndex = indexDescEl["ns"].str();
        if ( *nsToIndex == "" ) {
            *errMsg = "index write batch contains an invalid index descriptor";
            return false;
        }

        if ( it.more() ) {
            *errMsg = "index write batches may only contain a single index descriptor";
            return false;
        }

        return true;
    }

} // namespace mongo
