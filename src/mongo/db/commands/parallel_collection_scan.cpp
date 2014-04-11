// parallel_collection_scan.cpp

/**
*    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/storage/extent.h"
#include "mongo/db/structure/catalog/namespace_details.h"
#include "mongo/util/touch_pages.h"

namespace mongo {


    class ParallelCollectionScanCmd : public Command {
    public:

        struct ExtentInfo {
            ExtentInfo( DiskLoc dl, size_t s )
                : diskLoc(dl), size(s) {
            }
            DiskLoc diskLoc;
            size_t size;
        };

        class ExtentRunner : public Runner {
        public:
            ExtentRunner( const StringData& ns,
                          Database* db,
                          Collection* collection,
                          const vector<ExtentInfo>& extents )
                : _ns( ns.toString() ),
                  _collection( collection ),
                  _extents( extents ),
                  _extentManager( db->getExtentManager() ) {

                invariant( _extents.size() > 0 );

                _touchExtent( 0 );
                _currentExtent = 0;
                _currentRecord = _getExtent( _currentExtent )->firstRecord;
                if ( _currentRecord.isNull() )
                    _advance();
            }
            ~ExtentRunner() {
            }

            virtual RunnerState getNext(BSONObj* objOut, DiskLoc* dlOut) {
                if ( _collection == NULL )
                    return RUNNER_DEAD;
                if ( _currentRecord.isNull() )
                    return RUNNER_EOF;

                if ( objOut )
                    *objOut = _collection->docFor( _currentRecord );
                if ( dlOut )
                    *dlOut = _currentRecord;
                _advance();
                return RUNNER_ADVANCED;
            }

            virtual bool isEOF() {
                return _collection == NULL || _currentRecord.isNull();
            }
            virtual void kill() {
                _collection = NULL;
            }
            virtual void setYieldPolicy(YieldPolicy policy) {
                invariant( false );
            }
            virtual void saveState() {}
            virtual bool restoreState() { return true;}
            virtual const string& ns() { return _ns; }
            virtual void invalidate(const DiskLoc& dl, InvalidationType type) {
                switch ( type ) {
                case INVALIDATION_DELETION:
                    if ( dl == _currentRecord )
                        _advance();
                    break;
                case INVALIDATION_MUTATION:
                    // no-op
                    break;
                }
            }
            virtual const Collection* collection() {
                return _collection;
            }
            virtual Status getInfo(TypeExplain** explain, PlanInfo** planInfo) const {
                return Status( ErrorCodes::InternalError, "no" );
            }
        private:

            /**
             * @return if more data
             */
            bool _advance() {

                while ( _currentRecord.isNull() ) {
                    // need to move to next extent
                    if ( _currentExtent + 1 >= _extents.size() )
                        return false;
                    _currentExtent++;
                    _touchExtent( _currentExtent );
                    _currentRecord = _getExtent( _currentExtent )->firstRecord;
                    if ( !_currentRecord.isNull() )
                        return true;
                    // if we're here, the extent was empty, keep looking
                }

                // we're in an extent, advance
                _currentRecord = _extentManager.getNextRecordInExtent( _currentRecord );
                if ( _currentRecord.isNull() ) {
                    // finished this extent, need to move to the next one
                    return _advance();
                }
                return true;
            }

            Extent* _getExtent( size_t offset ) {
                DiskLoc dl = _extents[offset].diskLoc;
                return _extentManager.getExtent( dl );
            }

            void _touchExtent( size_t offset ) {
                Extent* e = _getExtent( offset );
                touch_pages( reinterpret_cast<const char*>(e), e->length );
            }

            string _ns;
            Collection* _collection;
            vector<ExtentInfo> _extents;
            ExtentManager& _extentManager;

            size_t _currentExtent;
            DiskLoc _currentRecord;
        };

        // ------------------------------------------------

        ParallelCollectionScanCmd() : Command( "parallelCollectionScan" ){}

        virtual LockType locktype() const { return READ; }
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return true; }

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            ActionSet actions;
            actions.addAction(ActionType::find);
            Privilege p(parseResourcePattern(dbname, cmdObj), actions);
            if ( client->getAuthorizationSession()->isAuthorizedForPrivilege(p) )
                return Status::OK();
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }

        virtual bool run( const string& dbname, BSONObj& cmdObj, int options,
                          string& errmsg, BSONObjBuilder& result,
                          bool fromRepl = false ) {

            NamespaceString ns( dbname, cmdObj[name].String() );

            Database* db = cc().database();
            Collection* collection = db->getCollection( ns );

            if ( !collection )
                return appendCommandStatus( result,
                                            Status( ErrorCodes::NamespaceNotFound,
                                                    str::stream() <<
                                                    "ns does not exist: " << ns.ns() ) );

            size_t numCursors = static_cast<size_t>( cmdObj["numCursors"].numberInt() );

            if ( numCursors == 0 || numCursors > 10000 )
                return appendCommandStatus( result,
                                            Status( ErrorCodes::BadValue,
                                                    str::stream() <<
                                                    "numCursors has to be between 1 and 10000" <<
                                                    " was: " << numCursors ) );

            vector< vector<ExtentInfo> > buckets;

            const ExtentManager& extentManager = db->getExtentManager();

            {
                DiskLoc extentDiskLoc = collection->details()->firstExtent();
                int extentNumber = 0;
                while (!extentDiskLoc.isNull()) {

                    Extent* thisExtent = extentManager.getExtent( extentDiskLoc );
                    ExtentInfo info( extentDiskLoc, thisExtent->length );
                    if ( buckets.size() < numCursors ) {
                        vector<ExtentInfo> v;
                        v.push_back( info );
                        buckets.push_back( v );
                    }
                    else {
                        buckets[ extentNumber % buckets.size() ].push_back( info );
                    }

                    extentDiskLoc = thisExtent->xnext;
                    extentNumber++;
                }

                BSONArrayBuilder bucketsBuilder;
                for ( size_t i = 0; i < buckets.size(); i++ ) {

                    auto_ptr<Runner> runner( new ExtentRunner( ns.ns(),
                                                               db,
                                                               collection,
                                                               buckets[i] ) );
                    ClientCursor* cc = new ClientCursor( collection, runner.release() );

                    // we are mimicking the aggregation cursor output here
                    // that is why there are ns, ok and empty firstBatch
                    BSONObjBuilder threadResult;
                    {
                        BSONObjBuilder cursor;
                        cursor.appendArray( "firstBatch", BSONObj() );
                        cursor.append( "ns", ns );
                        cursor.append( "id", cc->cursorid() );
                        threadResult.append( "cursor", cursor.obj() );
                    }
                    threadResult.appendBool( "ok", 1 );

                    bucketsBuilder.append( threadResult.obj() );
                }
                result.appendArray( "cursors", bucketsBuilder.obj() );
            }

            return true;

        }
    } parallelCollectionScanCmd;

}
