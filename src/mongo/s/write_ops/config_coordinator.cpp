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

#include "mongo/s/write_ops/config_coordinator.h"

#include "mongo/base/owned_pointer_vector.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/s/write_ops/batched_command_request.h"
#include "mongo/s/write_ops/batched_command_response.h"
#include "mongo/util/net/message.h"

#include "mongo/db/field_parser.h"

namespace mongo {


    ConfigCoordinator::ConfigCoordinator( MultiCommandDispatch* dispatcher,
                                          const vector<ConnectionString>& configHosts ) :
        _dispatcher( dispatcher ), _configHosts( configHosts ) {
    }

    namespace {

        //
        // Types to handle the reachability fsync checks
        //

        /**
         * A BSON serializable object representing an fsync command request
         */
        class FsyncRequest : public BSONSerializable {
        MONGO_DISALLOW_COPYING(FsyncRequest);
        public:

            FsyncRequest() {
            }

            bool isValid( std::string* errMsg ) const {
                return true;
            }

            /** Returns the BSON representation of the entry. */
            BSONObj toBSON() const {
                return BSON( "fsync" << true );
            }

            bool parseBSON( const BSONObj& source, std::string* errMsg ) {
                // Not implemented
                dassert( false );
                return false;
            }

            void clear() {
                // Not implemented
                dassert( false );
            }

            string toString() const {
                return toBSON().toString();
            }
        };

        /**
         * A BSON serializable object representing an fsync command response
         */
        class FsyncResponse : public BSONSerializable {
        MONGO_DISALLOW_COPYING(FsyncResponse);
        public:

            static const BSONField<int> ok;
            static const BSONField<int> errCode;
            static const BSONField<string> errMessage;

            FsyncResponse() {
                clear();
            }

            bool isValid( std::string* errMsg ) const {
                return _isOkSet;
            }

            BSONObj toBSON() const {
                // Not implemented
                dassert( false );
                return BSONObj();
            }

            bool parseBSON( const BSONObj& source, std::string* errMsg ) {

                FieldParser::FieldState result;

                result = FieldParser::extractNumber( source, ok, &_ok, errMsg );
                if ( result == FieldParser::FIELD_INVALID )
                    return false;
                _isOkSet = result != FieldParser::FIELD_NONE;

                result = FieldParser::extract( source, errCode, &_errCode, errMsg );
                if ( result == FieldParser::FIELD_INVALID )
                    return false;
                _isErrCodeSet = result != FieldParser::FIELD_NONE;

                result = FieldParser::extract( source, errMessage, &_errMessage, errMsg );
                if ( result == FieldParser::FIELD_INVALID )
                    return false;
                _isErrMessageSet = result != FieldParser::FIELD_NONE;

                return true;
            }

            void clear() {
                _ok = false;
                _isOkSet = false;

                _errCode = 0;
                _isErrCodeSet = false;

                _errMessage = "";
                _isErrMessageSet = false;
            }

            string toString() const {
                return toBSON().toString();
            }

            int getOk() {
                dassert( _isOkSet );
                return _ok;
            }

            void setOk( int ok ) {
                _ok = ok;
                _isOkSet = true;
            }

            int getErrCode() {
                if ( _isErrCodeSet ) {
                    return _errCode;
                }
                else {
                    return errCode.getDefault();
                }
            }

            void setErrCode( int errCode ) {
                _errCode = errCode;
                _isErrCodeSet = true;
            }

            const string& getErrMessage() {
                dassert( _isErrMessageSet );
                return _errMessage;
            }

            void setErrMessage( const StringData& errMsg ) {
                _errMessage = errMsg.toString();
                _isErrMessageSet = true;
            }

        private:

            int _ok;
            bool _isOkSet;

            int _errCode;
            bool _isErrCodeSet;

            string _errMessage;
            bool _isErrMessageSet;

        };

        const BSONField<int> FsyncResponse::ok( "ok" );
        const BSONField<int> FsyncResponse::errCode( "code" );
        const BSONField<string> FsyncResponse::errMessage( "errmsg" );

        //
        // Types to associate responses with particular config servers
        //

        struct ConfigResponse {
            ConfigResponse( const ConnectionString& configHost ) :
                configHost( configHost ) {
            }

            const ConnectionString configHost;
            BatchedCommandResponse response;
        };

        struct ConfigFsyncResponse {
            ConfigFsyncResponse( const ConnectionString& configHost ) :
                configHost( configHost ) {
            }

            const ConnectionString configHost;
            FsyncResponse response;
        };
    }

    //
    // Error processing helpers
    //

    static void buildErrorFrom( const Status& status, BatchedCommandResponse* response ) {
        response->setOk( false );
        response->setN( 0 );
        response->setErrCode( static_cast<int>( status.code() ) );
        response->setErrMessage( status.reason() );

        dassert( response->isValid( NULL ) );
    }

    static void buildFsyncErrorFrom( const Status& status, FsyncResponse* response ) {
        response->setOk( false );
        response->setErrCode( static_cast<int>( status.code() ) );
        response->setErrMessage( status.reason() );
    }

    static bool areResponsesEqual( const BatchedCommandResponse& responseA,
                                   const BatchedCommandResponse& responseB ) {

        // TODO: Better reporting of why not equal
        if ( responseA.getOk() != responseB.getOk() )
            return false;
        if ( responseA.getN() != responseB.getN() )
            return false;
        if ( responseA.isSingleUpsertedSet() != responseB.isSingleUpsertedSet() )
            return false;
        if ( responseA.isUpsertDetailsSet() != responseB.isUpsertDetailsSet() )
            return false;

        if ( responseA.isSingleUpsertedSet() ) {
            BSONObj upsertA = responseA.getSingleUpserted();
            BSONObj upsertB = responseB.getSingleUpserted();
            if ( upsertA.woCompare( upsertB ) != 0 )
                return false;
        }

        if ( responseA.isUpsertDetailsSet() ) {
            // TODO:
        }

        if ( responseA.getOk() )
            return true;

        // TODO: Compare errors here

        return true;
    }

    static bool areAllResponsesEqual( const vector<ConfigResponse*>& responses ) {

        BatchedCommandResponse* lastResponse = NULL;
        for ( vector<ConfigResponse*>::const_iterator it = responses.begin(); it != responses.end();
            ++it ) {

            BatchedCommandResponse* response = &( *it )->response;

            if ( lastResponse != NULL ) {
                if ( !areResponsesEqual( *lastResponse, *response ) ) {
                    return false;
                }
            }

            lastResponse = response;
        }

        return true;
    }

    static void combineResponses( const vector<ConfigResponse*>& responses,
                                  BatchedCommandResponse* clientResponse ) {

        if ( areAllResponsesEqual( responses ) ) {
            responses.front()->response.cloneTo( clientResponse );
            return;
        }

        clientResponse->setOk( false );
        clientResponse->setN( 0 );
        clientResponse->setErrCode( ErrorCodes::ManualInterventionRequired );

        BSONObjBuilder errInfoB;
        for ( vector<ConfigResponse*>::const_iterator it = responses.begin(); it != responses.end();
            ++it ) {
            ConfigResponse* response = *it;
            errInfoB.append( response->configHost.toString(), response->response.toBSON() );
        }

        clientResponse->setErrInfo( errInfoB.obj() );
        clientResponse->setErrMessage( "config write was not consistent, "
                                       "manual intervention may be required" );

    }

    static void combineFsyncErrors( const vector<ConfigFsyncResponse*>& responses,
                                    BatchedCommandResponse* clientResponse ) {

        clientResponse->setOk( false );
        clientResponse->setN( 0 );
        clientResponse->setErrCode( ErrorCodes::RemoteValidationError );

        BSONObjBuilder errInfoB;
        for ( vector<ConfigFsyncResponse*>::const_iterator it = responses.begin();
            it != responses.end(); ++it ) {
            ConfigFsyncResponse* fsyncResponse = *it;
            if ( fsyncResponse->response.getOk() )
                continue;
            errInfoB.append( fsyncResponse->configHost.toString(),
                             fsyncResponse->response.toBSON() );
        }

        clientResponse->setErrInfo( errInfoB.obj() );
        clientResponse->setErrMessage( "could not verify config servers were "
                                       "active and reachable before write" );
    }

    /**
     * The core config write functionality.
     *
     * Config writes run in two passes - the first is a quick check to ensure the config servers
     * are all reachable, the second runs the actual write.
     *
     * TODO: Upgrade and move this logic to the config servers, a state machine implementation
     * is probably the next step.
     */
    void ConfigCoordinator::executeBatch( const BatchedCommandRequest& clientRequest,
                                          BatchedCommandResponse* clientResponse,
                                          bool fsyncCheck ) {

        NamespaceString nss( clientRequest.getNS() );
        dassert( nss.db() == "config" || nss.db() == "admin" );
        dassert( clientRequest.sizeWriteOps() == 1u );

        if ( fsyncCheck ) {

            //
            // Sanity check that all configs are still reachable using fsync, preserving legacy
            // behavior
            //

            OwnedPointerVector<ConfigFsyncResponse> fsyncResponsesOwned;
            vector<ConfigFsyncResponse*>& fsyncResponses = fsyncResponsesOwned.mutableVector();

            //
            // Send side
            //

            for ( vector<ConnectionString>::iterator it = _configHosts.begin();
                it != _configHosts.end(); ++it ) {
                ConnectionString& configHost = *it;
                FsyncRequest fsyncRequest;
                _dispatcher->addCommand( configHost, "admin", fsyncRequest );
            }

            _dispatcher->sendAll();

            //
            // Recv side
            //

            bool fsyncError = false;
            while ( _dispatcher->numPending() > 0 ) {

                ConnectionString configHost;
                fsyncResponses.push_back( new ConfigFsyncResponse( configHost ) );
                ConfigFsyncResponse& fsyncResponse = *fsyncResponses.back();
                Status dispatchStatus = _dispatcher->recvAny( &configHost,
                                                              &fsyncResponse.response );

                // We've got to recv everything, no matter what
                if ( !dispatchStatus.isOK() ) {
                    fsyncError = true;
                    buildFsyncErrorFrom( dispatchStatus, &fsyncResponse.response );
                }
                else if ( !fsyncResponse.response.getOk() ) {
                    fsyncError = true;
                }
            }

            if ( fsyncError ) {
                combineFsyncErrors( fsyncResponses, clientResponse );
                return;
            }
            else {
                fsyncResponsesOwned.clear();
            }
        }

        //
        // Do the actual writes
        //

        BatchedCommandRequest configRequest( clientRequest.getBatchType() );
        clientRequest.cloneTo( &configRequest );
        configRequest.setNS( nss.coll() );

        OwnedPointerVector<ConfigResponse> responsesOwned;
        vector<ConfigResponse*>& responses = responsesOwned.mutableVector();

        //
        // Send the actual config writes
        //

        // Get as many batches as we can at once
        for ( vector<ConnectionString>::iterator it = _configHosts.begin();
            it != _configHosts.end(); ++it ) {
            ConnectionString& configHost = *it;
            _dispatcher->addCommand( configHost, nss.db(), configRequest );
        }

        // Send them all out
        _dispatcher->sendAll();

        //
        // Recv side
        //

        while ( _dispatcher->numPending() > 0 ) {

            // Get the response
            ConnectionString configHost;
            responses.push_back( new ConfigResponse( configHost ) );
            ConfigResponse& configResponse = *responses.back();
            Status dispatchStatus = _dispatcher->recvAny( &configHost, &configResponse.response );

            if ( !dispatchStatus.isOK() ) {
                buildErrorFrom( dispatchStatus, &configResponse.response );
            }
        }

        combineResponses( responses, clientResponse );
    }

}
