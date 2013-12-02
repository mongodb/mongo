/**
 *    Copyright (C) 2013 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/s/write_ops/batch_downconvert.h"

#include "mongo/util/assert_util.h"

namespace mongo {

    void SafeWriter::fillLastError( const BSONObj& gleResult, LastError* error ) {
        if ( gleResult["code"].isNumber() ) error->code = gleResult["code"].numberInt();
        if ( !gleResult["err"].eoo() ) {
            if ( gleResult["err"].type() == String ) {
                error->msg = gleResult["err"].String();
            }
            else {
                dassert( gleResult["err"].type() == jstNULL );
            }
        }
        if ( gleResult["n"].isNumber() ) error->nObjects = gleResult["n"].numberInt();
        if ( gleResult["updatedExisting"].eoo() ) {
            error->updatedExisting = LastError::NotUpdate;
        }
        else {
            error->updatedExisting =
                gleResult["updatedExisting"].trueValue() ? LastError::True : LastError::False;
        }
        if ( !gleResult["upserted"].eoo() ) {
            dassert( gleResult["upserted"].isABSONObj() );
            error->upsertedId = gleResult["upserted"].Obj();
        }
        // Needed b/c we detect stale config on legacy hosts via this field
        if ( !gleResult["writeback"].eoo() ) {
            dassert( gleResult["writeback"].type() == jstOID );
            error->writebackId = gleResult["writeback"].OID();
        }
    }

    bool BatchSafeWriter::isFailedOp( const LastError& error ) {
        return error.msg != "";
    }

    WriteErrorDetail* BatchSafeWriter::lastErrorToBatchError( const LastError& lastError ) {

        bool isFailedOp = lastError.msg != "";
        bool isStaleOp = lastError.writebackId.isSet();
        dassert( !( isFailedOp && isStaleOp ) );

        if ( isFailedOp ) {
            WriteErrorDetail* batchError = new WriteErrorDetail;
            if ( lastError.code != 0 ) batchError->setErrCode( lastError.code );
            else batchError->setErrCode( ErrorCodes::UnknownError );
            batchError->setErrMessage( lastError.msg );
            return batchError;
        }
        else if ( isStaleOp ) {
            WriteErrorDetail* batchError = new WriteErrorDetail;
            batchError->setErrCode( ErrorCodes::StaleShardVersion );
            batchError->setErrInfo( BSON( "downconvert" << true ) ); // For debugging
            batchError->setErrMessage( "shard version was stale" );
            return batchError;
        }

        return NULL;
    }

    void BatchSafeWriter::safeWriteBatch( DBClientBase* conn,
                                          const BatchedCommandRequest& request,
                                          BatchedCommandResponse* response ) {

        // N starts at zero, and we add to it for each item
        response->setN( 0 );

        for ( size_t i = 0; i < request.sizeWriteOps(); ++i ) {

            BatchItemRef itemRef( &request, static_cast<int>( i ) );
            LastError lastError;

            _safeWriter->safeWrite( conn, itemRef, &lastError );

            // Register the error if we need to
            WriteErrorDetail* batchError = lastErrorToBatchError( lastError );
            if ( batchError ) {
                batchError->setIndex( i );
                response->addToErrDetails( batchError );
            }

            // Special case for making legacy "n" field result for insert match the write
            // command result.
            if ( request.getBatchType() == BatchedCommandRequest::BatchType_Insert &&
                    batchError == NULL &&
                    StringData( request.getNS() ).startsWith( "config." )) {
                dassert( request.getInsertRequest()->getDocuments().size() == 1 );
                // n is always 0 for legacy inserts.
                dassert( lastError.nObjects == 0 );

                lastError.nObjects = 1;
            }

            response->setN( response->getN() + lastError.nObjects );

            if ( !lastError.upsertedId.isEmpty() ) {
                BatchedUpsertDetail* upsertedId = new BatchedUpsertDetail;
                upsertedId->setIndex( i );
                upsertedId->setUpsertedID( lastError.upsertedId );
                response->addToUpsertDetails( upsertedId );
            }

            // Break on first error if we're ordered
            if ( request.getOrdered() && BatchSafeWriter::isFailedOp( lastError ) ) break;
        }

        response->setOk( !response->isErrCodeSet() );
        dassert( response->isValid( NULL ) );
    }
}
