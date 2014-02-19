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

    bool BatchedCommandRequest::isValidIndexRequest( string* errMsg ) const {

        string dummy;
        if ( !errMsg )
            errMsg = &dummy;
        dassert( isInsertIndexRequest() );

        if ( sizeWriteOps() != 1 ) {
            *errMsg = "invalid batch request for index creation";
            return false;
        }

        NamespaceString targetNSS( getTargetingNS() );
        if ( !targetNSS.isValid() ) {
            *errMsg = targetNSS.ns() + " is not a valid namespace to index";
            return false;
        }

        NamespaceString reqNSS( getNS() );
        if ( reqNSS.db().compare( targetNSS.db() ) != 0 ) {
            *errMsg = targetNSS.ns() + " namespace is not in the request database "
                      + reqNSS.db().toString();
            return false;
        }

        return true;
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

    void BatchedCommandRequest::setMetadata(BatchedRequestMetadata* metadata) {
        INVOKE( setMetadata, metadata );
    }

    void BatchedCommandRequest::unsetMetadata() {
        INVOKE( unsetMetadata );
    }

    bool BatchedCommandRequest::isMetadataSet() const {
        INVOKE( isMetadataSet );
    }

    BatchedRequestMetadata* BatchedCommandRequest::getMetadata() const {
        INVOKE( getMetadata );
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
