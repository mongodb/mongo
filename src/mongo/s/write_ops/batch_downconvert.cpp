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

#include "mongo/bson/util/builder.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/util/assert_util.h"

namespace mongo {

    Status BatchSafeWriter::extractGLEErrors( const BSONObj& gleResponse, GLEErrors* errors ) {

        // DRAGONS
        // Parsing GLE responses is incredibly finicky.
        // The order of testing here is extremely important.

        ///////////////////////////////////////////////////////////////////////
        // IMPORTANT!
        // Also update extractGLEErrors in batch_api.js for any changes made here.

        const bool isOK = gleResponse["ok"].trueValue();
        const string err = gleResponse["err"].str();
        const string errMsg = gleResponse["errmsg"].str();
        const string wNote = gleResponse["wnote"].str();
        const string jNote = gleResponse["jnote"].str();
        const int code = gleResponse["code"].numberInt();
        const bool timeout = gleResponse["wtimeout"].trueValue();

        if ( err == "norepl" || err == "noreplset" ) {
            // Know this is legacy gle and the repl not enforced - write concern error in 2.4
            errors->wcError.reset( new WCErrorDetail );
            errors->wcError->setErrCode( ErrorCodes::WriteConcernFailed );
            if ( !errMsg.empty() ) {
                errors->wcError->setErrMessage( errMsg );
            }
            else if ( !wNote.empty() ) {
                errors->wcError->setErrMessage( wNote );
            }
            else {
                errors->wcError->setErrMessage( err );
            }
        }
        else if ( timeout ) {
            // Know there was no write error
            errors->wcError.reset( new WCErrorDetail );
            errors->wcError->setErrCode( ErrorCodes::WriteConcernFailed );
            if ( !errMsg.empty() ) {
                errors->wcError->setErrMessage( errMsg );
            }
            else {
                errors->wcError->setErrMessage( err );
            }
            errors->wcError->setErrInfo( BSON( "wtimeout" << true ) );
        }
        else if ( code == 10990 /* no longer primary */
                  || code == 16805 /* replicatedToNum no longer primary */
                  || code == 14830 /* gle wmode changed / invalid */
                  // 2.6 Error codes
                  || code == ErrorCodes::NotMaster
                  || code == ErrorCodes::UnknownReplWriteConcern
                  || code == ErrorCodes::WriteConcernFailed ) {
            // Write concern errors that get returned as regular errors (result may not be ok: 1.0)
            errors->wcError.reset( new WCErrorDetail );
            errors->wcError->setErrCode( code );
            errors->wcError->setErrMessage( errMsg );
        }
        else if ( !isOK ) {

            //
            // !!! SOME GLE ERROR OCCURRED, UNKNOWN WRITE RESULT !!!
            //

            return Status( DBException::convertExceptionCode(
                               code ? code : ErrorCodes::UnknownError ),
                           errMsg );
        }
        else if ( !err.empty() ) {
            // Write error
            errors->writeError.reset( new WriteErrorDetail );
            int writeErrorCode = code == 0 ? ErrorCodes::UnknownError : code;

            // COMPATIBILITY
            // Certain clients expect write commands to always report 11000 for duplicate key
            // errors, while legacy GLE can return additional codes.
            if ( writeErrorCode == 11001 /* dup key in update */
                 || writeErrorCode == 12582 /* dup key capped */) {
                writeErrorCode = ErrorCodes::DuplicateKey;
            }

            errors->writeError->setErrCode( writeErrorCode );
            errors->writeError->setErrMessage( err );
        }
        else if ( !jNote.empty() ) {
            // Know this is legacy gle and the journaling not enforced - write concern error in 2.4
            errors->wcError.reset( new WCErrorDetail );
            errors->wcError->setErrCode( ErrorCodes::WriteConcernFailed );
            errors->wcError->setErrMessage( jNote );
        }

        // See if we had a version error reported as a writeback id - this is the only kind of
        // write error where the write concern may still be enforced.
        // The actual version that was stale is lost in the writeback itself.
        const int opsSinceWriteback = gleResponse["writebackSince"].numberInt();
        const bool hadWriteback = !gleResponse["writeback"].eoo();

        if ( hadWriteback && opsSinceWriteback == 0 ) {

            // We shouldn't have a previous write error
            dassert( !errors->writeError.get() );
            if ( errors->writeError.get() ) {
                // Somehow there was a write error *and* a writeback from the last write
                warning() << "both a write error and a writeback were reported "
                          << "when processing a legacy write: " << errors->writeError->toBSON()
                          << endl;
            }

            errors->writeError.reset( new WriteErrorDetail );
            errors->writeError->setErrCode( ErrorCodes::StaleShardVersion );
            errors->writeError->setErrInfo( BSON( "downconvert" << true ) ); // For debugging
            errors->writeError->setErrMessage( "shard version was stale" );
        }

        return Status::OK();
    }

    void BatchSafeWriter::extractGLEStats( const BSONObj& gleResponse, GLEStats* stats ) {
        stats->n = gleResponse["n"].numberInt();
        if ( !gleResponse["upserted"].eoo() ) {
            stats->upsertedId = gleResponse["upserted"].wrap( "upserted" );
        }
        if ( gleResponse["lastOp"].type() == Timestamp ) {
            stats->lastOp = gleResponse["lastOp"]._opTime();
        }
    }

    static BSONObj fixWCForConfig( const BSONObj& writeConcern ) {
        BSONObjBuilder fixedB;
        BSONObjIterator it( writeConcern );
        while ( it.more() ) {
            BSONElement el = it.next();
            if ( StringData( el.fieldName() ).compare( "w" ) != 0 ) {
                fixedB.append( el );
            }
        }
        return fixedB.obj();
    }

    void BatchSafeWriter::safeWriteBatch( DBClientBase* conn,
                                          const BatchedCommandRequest& request,
                                          BatchedCommandResponse* response ) {

        const NamespaceString nss( request.getNS() );

        // N starts at zero, and we add to it for each item
        response->setN( 0 );

        // GLE path always sets nModified to -1 (sentinel) to indicate we should omit it later.
        response->setNModified(-1);

        for ( size_t i = 0; i < request.sizeWriteOps(); ++i ) {

            // Break on first error if we're ordered
            if ( request.getOrdered() && response->isErrDetailsSet() )
                break;

            BatchItemRef itemRef( &request, static_cast<int>( i ) );

            BSONObj gleResult;
            GLEErrors errors;
            Status status = _safeWriter->safeWrite( conn,
                                                    itemRef,
                                                    WriteConcernOptions::Acknowledged,
                                                    &gleResult );

            if ( status.isOK() ) {
                status = extractGLEErrors( gleResult, &errors );
            }

            if ( !status.isOK() ) {
                response->clear();
                response->setOk( false );
                response->setErrCode( ErrorCodes::RemoteResultsUnavailable );
                
                StringBuilder builder;
                builder << "could not get write error from safe write";
                builder << causedBy( status.toString() );
                response->setErrMessage( builder.str() );
                return;
            }

            if ( errors.wcError.get() ) {
                response->setWriteConcernError( errors.wcError.release() );
            }

            //
            // STATS HANDLING
            //

            GLEStats stats;
            extractGLEStats( gleResult, &stats );

            // Special case for making legacy "n" field result for insert match the write
            // command result.
            if ( request.getBatchType() == BatchedCommandRequest::BatchType_Insert
                 && !errors.writeError.get() ) {
                // n is always 0 for legacy inserts.
                dassert( stats.n == 0 );
                stats.n = 1;
            }

            response->setN( response->getN() + stats.n );

            if ( !stats.upsertedId.isEmpty() ) {
                BatchedUpsertDetail* upsertedId = new BatchedUpsertDetail;
                upsertedId->setIndex( i );
                upsertedId->setUpsertedID( stats.upsertedId );
                response->addToUpsertDetails( upsertedId );
            }

            response->setLastOp( stats.lastOp );

            // Save write error
            if ( errors.writeError.get() ) {
                errors.writeError->setIndex( i );
                response->addToErrDetails( errors.writeError.release() );
            }
        }

        //
        // WRITE CONCERN ERROR HANDLING
        //

        // The last write is weird, since we enforce write concern and check the error through
        // the same GLE if possible.  If the last GLE was an error, the write concern may not
        // have been enforced in that same GLE, so we need to send another after resetting the
        // error.

        BSONObj writeConcern;
        if ( request.isWriteConcernSet() ) {
            writeConcern = request.getWriteConcern();
            // Pre-2.4.2 mongods react badly to 'w' being set on config servers
            if ( nss.db() == "config" )
                writeConcern = fixWCForConfig( writeConcern );
        }

        bool needToEnforceWC = WriteConcernOptions::Acknowledged.woCompare(writeConcern) != 0 &&
                WriteConcernOptions::Unacknowledged.woCompare(writeConcern) != 0;

        if ( needToEnforceWC &&
                ( !response->isErrDetailsSet() ||
                        ( !request.getOrdered() &&
                                // Not all errored. Note: implicit response->isErrDetailsSet().
                                response->sizeErrDetails() < request.sizeWriteOps() ))) {

            // Might have gotten a write concern validity error earlier, these are
            // enforced even if the wc isn't applied, so we ignore.
            response->unsetWriteConcernError();

            const string dbName( nss.db().toString() );

            Status status( Status::OK() );

            if ( response->isErrDetailsSet() ) {
                const WriteErrorDetail* lastError = response->getErrDetails().back();

                // If last write op was an error.
                if ( lastError->getIndex() == static_cast<int>( request.sizeWriteOps() - 1 )) {
                    // Reset previous errors so we can apply the write concern no matter what
                    // as long as it is valid.
                    status = _safeWriter->clearErrors( conn, dbName );
                }
            }

            BSONObj gleResult;
            if ( status.isOK() ) {
                status = _safeWriter->enforceWriteConcern( conn,
                                                           dbName,
                                                           writeConcern,
                                                           &gleResult );
            }

            GLEErrors errors;
            if ( status.isOK() ) {
                status = extractGLEErrors( gleResult, &errors );
            }
            
            if ( !status.isOK() ) {
                auto_ptr<WCErrorDetail> wcError( new WCErrorDetail );
                wcError->setErrCode( status.code() );
                wcError->setErrMessage( status.reason() );
                response->setWriteConcernError( wcError.release() ); 
            }
            else if ( errors.wcError.get() ) {
                response->setWriteConcernError( errors.wcError.release() );
            }
        }

        response->setOk( true );
        dassert( response->isValid( NULL ) );
    }

    /**
     * Suppress the "err" and "code" field if they are coming from a previous write error and
     * are not related to write concern.  Also removes any write stats information (e.g. "n")
     *
     * Also, In some cases, 2.4 GLE w/ wOpTime can give us duplicate "err" and "code" fields b/c of
     * reporting a previous error.  The later field is what we want - dedup and use later field.
     *
     * Returns the stripped GLE response.
     */
    BSONObj BatchSafeWriter::stripNonWCInfo( const BSONObj& gleResponse ) {

        BSONObjIterator it( gleResponse );
        BSONObjBuilder builder;

        BSONElement codeField; // eoo
        BSONElement errField; // eoo

        while ( it.more() ) {
            BSONElement el = it.next();
            StringData fieldName( el.fieldName() );
            if ( fieldName.compare( "err" ) == 0 ) {
                errField = el;
            }
            else if ( fieldName.compare( "code" ) == 0 ) {
                codeField = el;
            }
            else if ( fieldName.compare( "n" ) == 0 || fieldName.compare( "nModified" ) == 0
                      || fieldName.compare( "upserted" ) == 0
                      || fieldName.compare( "updatedExisting" ) == 0 ) {
                // Suppress field
            }
            else {
                builder.append( el );
            }
        }

        if ( !codeField.eoo() ) {
            if ( !gleResponse["ok"].trueValue() ) {
                // The last code will be from the write concern
                builder.append( codeField );
            }
            else {
                // The code is from a non-wc error on this connection - suppress it
            }
        }

        if ( !errField.eoo() ) {
            string err = errField.str();
            if ( err == "norepl" || err == "noreplset" || err == "timeout" ) {
                // Append err if it's from a write concern issue
                builder.append( errField );
            }
            else {
                // Suppress non-write concern err as null, but we need to report null err if ok
                if ( gleResponse["ok"].trueValue() )
                    builder.appendNull( errField.fieldName() );
            }
        }

        return builder.obj();
    }

    namespace {

        /**
         * Trivial implementation of a BSON serializable object for backwards-compatibility.
         *
         * NOTE: This is not a good example of using BSONSerializable.  For anything more complex,
         * create an implementation with fields defined.
         */
        class RawBSONSerializable : public BSONSerializable {
        MONGO_DISALLOW_COPYING(RawBSONSerializable);
        public:

            RawBSONSerializable() {
            }

            RawBSONSerializable( const BSONObj& doc ) :
                _doc( doc ) {
            }

            bool isValid( std::string* errMsg ) const {
                return true;
            }

            BSONObj toBSON() const {
                return _doc;
            }

            bool parseBSON( const BSONObj& source, std::string* errMsg ) {
                _doc = source.getOwned();
                return true;
            }

            void clear() {
                _doc = BSONObj();
            }

            string toString() const {
                return toBSON().toString();
            }

        private:

            BSONObj _doc;
        };
    }

    // Adds a wOpTime and a wElectionId field to a set of gle options
    static BSONObj buildGLECmdWithOpTime( const BSONObj& gleOptions,
                                          const OpTime& opTime,
                                          const OID& electionId ) {
        BSONObjBuilder builder;
        BSONObjIterator it( gleOptions );

        for ( int i = 0; it.more(); ++i ) {
            BSONElement el = it.next();

            // Make sure first element is getLastError : 1
            if ( i == 0 ) {
                StringData elName( el.fieldName() );
                if ( !elName.equalCaseInsensitive( "getLastError" ) ) {
                    builder.append( "getLastError", 1 );
                }
            }

            builder.append( el );
        }
        builder.appendTimestamp( "wOpTime", opTime.asDate() );
        builder.appendOID( "wElectionId", const_cast<OID*>(&electionId) );
        return builder.obj();
    }

    Status enforceLegacyWriteConcern( MultiCommandDispatch* dispatcher,
                                      const StringData& dbName,
                                      const BSONObj& options,
                                      const HostOpTimeMap& hostOpTimes,
                                      vector<LegacyWCResponse>* legacyWCResponses ) {

        if ( hostOpTimes.empty() ) {
            return Status::OK();
        }

        for ( HostOpTimeMap::const_iterator it = hostOpTimes.begin(); it != hostOpTimes.end();
            ++it ) {

            const ConnectionString& shardEndpoint = it->first;
            const HostOpTime hot = it->second;
            const OpTime& opTime = hot.opTime;
            const OID& electionId = hot.electionId;

            LOG( 3 ) << "enforcing write concern " << options << " on " << shardEndpoint.toString()
                     << " at opTime " << opTime.toStringPretty() << " with electionID "
                     << electionId;

            BSONObj gleCmd = buildGLECmdWithOpTime( options, opTime, electionId );

            RawBSONSerializable gleCmdSerial( gleCmd );
            dispatcher->addCommand( shardEndpoint, dbName, gleCmdSerial );
        }

        dispatcher->sendAll();

        vector<Status> failedStatuses;

        while ( dispatcher->numPending() > 0 ) {

            ConnectionString shardEndpoint;
            RawBSONSerializable gleResponseSerial;

            Status dispatchStatus = dispatcher->recvAny( &shardEndpoint, &gleResponseSerial );
            if ( !dispatchStatus.isOK() ) {
                // We need to get all responses before returning
                failedStatuses.push_back( dispatchStatus );
                continue;
            }

            BSONObj gleResponse = BatchSafeWriter::stripNonWCInfo( gleResponseSerial.toBSON() );

            // Use the downconversion tools to determine if this GLE response is ok, a
            // write concern error, or an unknown error we should immediately abort for.
            BatchSafeWriter::GLEErrors errors;
            Status extractStatus = BatchSafeWriter::extractGLEErrors( gleResponse, &errors );
            if ( !extractStatus.isOK() ) {
                failedStatuses.push_back( extractStatus );
                continue;
            }

            LegacyWCResponse wcResponse;
            wcResponse.shardHost = shardEndpoint.toString();
            wcResponse.gleResponse = gleResponse;
            if ( errors.wcError.get() ) {
                wcResponse.errToReport = errors.wcError->getErrMessage();
            }

            legacyWCResponses->push_back( wcResponse );
        }

        if ( failedStatuses.empty() ) {
            return Status::OK();
        }

        StringBuilder builder;
        builder << "could not enforce write concern";

        for ( vector<Status>::const_iterator it = failedStatuses.begin();
            it != failedStatuses.end(); ++it ) {
            const Status& failedStatus = *it;
            if ( it == failedStatuses.begin() ) {
                builder << causedBy( failedStatus.toString() );
            }
            else {
                builder << ":: and ::" << failedStatus.toString();
            }
        }

        return Status( failedStatuses.size() == 1u ? failedStatuses.front().code() : 
                                                     ErrorCodes::MultipleErrorsOccurred, 
                       builder.str() );
    }
}
