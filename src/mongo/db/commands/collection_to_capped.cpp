// collection_to_capped.cpp

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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#include "mongo/db/background.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h" // XXX-remove
#include "mongo/db/commands.h"
#include "mongo/db/instance.h" // XXX-remove
#include "mongo/db/namespace_details.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/storage/extent.h"

namespace mongo {

    /* convertToCapped seems to use this */
    class CmdCloneCollectionAsCapped : public Command {
    public:
        CmdCloneCollectionAsCapped() : Command( "cloneCollectionAsCapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual void help( stringstream &help ) const {
            help << "{ cloneCollectionAsCapped:<fromName>, toCollection:<toName>, size:<sizeInBytes> }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet sourceActions;
            sourceActions.addAction(ActionType::find);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), sourceActions));

            ActionSet targetActions;
            targetActions.addAction(ActionType::insert);
            targetActions.addAction(ActionType::createIndex);
            targetActions.addAction(ActionType::convertToCapped);
            std::string collection = cmdObj.getStringField("toCollection");
            uassert(16708, "bad 'toCollection' value", !collection.empty());

            out->push_back(Privilege(ResourcePattern::forExactNamespace(
                                             NamespaceString(dbname, collection)),
                                     targetActions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string from = jsobj.getStringField( "cloneCollectionAsCapped" );
            string to = jsobj.getStringField( "toCollection" );
            long long size = (long long)jsobj.getField( "size" ).number();

            if ( from.empty() || to.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            Database* db = cc().database();

            string fromNs = dbname + "." + from;
            string toNs = dbname + "." + to;

            Collection* fromCollection = db->getCollection( fromNs );
            massert( 10301, "source collection " + fromNs + " does not exist", fromCollection );

            massert( 17287, "to collection already exists", !db->getCollection( toNs ) );

            // create new collection
            {
                Client::Context ctx( toNs );
                BSONObjBuilder spec;
                spec.appendBool( "capped", true );
                spec.append( "size", static_cast<double>( size ) );
                if ( jsobj.hasField("temp") )
                    spec.append( jsobj["temp"] );
                if ( !userCreateNS( toNs.c_str(), spec.done(), errmsg, true ) )
                    return false;
            }

            auto_ptr<Runner> runner;

            {
                NamespaceDetails* details = fromCollection->details();
                DiskLoc extent = details->firstExtent();

                // datasize and extentSize can't be compared exactly, so add some padding to 'size'
                long long excessSize = fromCollection->dataSize() - size * 2;

                // skip ahead some extents since not all the data fits,
                // so we have to chop a bunch off
                for( ;
                     excessSize > extent.ext()->length && extent != details->lastExtent();
                     extent = extent.ext()->xnext ) {

                    excessSize -= extent.ext()->length;
                    LOG( 2 ) << "cloneCollectionAsCapped skipping extent of size "
                             << extent.ext()->length << endl;
                    LOG( 6 ) << "excessSize: " << excessSize << endl;
                }
                DiskLoc startLoc = extent.ext()->firstRecord;

                runner.reset( InternalPlanner::collectionScan(fromNs,
                                                              InternalPlanner::FORWARD,
                                                              startLoc) );
            }

            Collection* toCollection = db->getCollection( toNs );
            verify( toCollection );

            while ( true ) {
                BSONObj obj;
                Runner::RunnerState state = runner->getNext(&obj, NULL);

                switch( state ) {
                case Runner::RUNNER_EOF:
                    return true;
                case Runner::RUNNER_DEAD:
                    db->dropCollection( toNs );
                    errmsg = "runner turned dead while iterating";
                    return false;
                case Runner::RUNNER_ERROR:
                    errmsg = "runner error while iterating";
                    return false;
                case Runner::RUNNER_ADVANCED:
                    toCollection->insertDocument( obj, true );
                    logOp( "i", toNs.c_str(), obj );
                    getDur().commitIfNeeded();
                }
            }

            return true;
        }
    } cmdCloneCollectionAsCapped;

    /* jan2010:
       Converts the given collection to a capped collection w/ the specified size.
       This command is not highly used, and is not currently supported with sharded
       environments.
       */
    class CmdConvertToCapped : public Command {
    public:
        CmdConvertToCapped() : Command( "convertToCapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        // calls renamecollection which does a global lock, so we must too:
        virtual bool lockGlobally() const { return true; }
        virtual void help( stringstream &help ) const {
            help << "{ convertToCapped:<fromCollectionName>, size:<sizeInBytes> }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::convertToCapped);
            out->push_back(Privilege(parseResourcePattern(dbname, cmdObj), actions));
        }
        bool run(const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            BackgroundOperation::assertNoBgOpInProgForDb(dbname.c_str());

            string from = jsobj.getStringField( "convertToCapped" );
            long long size = (long long)jsobj.getField( "size" ).number();

            if ( from.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            string shortTmpName = str::stream() << "tmp.convertToCapped." << from;
            string longTmpName = str::stream() << dbname << "." << shortTmpName;

            DBDirectClient client;
            client.dropCollection( longTmpName );

            BSONObj info;
            if ( !client.runCommand( dbname ,
                                     BSON( "cloneCollectionAsCapped" << from << "toCollection" << shortTmpName << "size" << double( size ) << "temp" << true ),
                                     info ) ) {
                errmsg = "cloneCollectionAsCapped failed: " + info.toString();
                return false;
            }

            if ( !client.dropCollection( dbname + "." + from ) ) {
                errmsg = "failed to drop original collection";
                return false;
            }

            if ( !client.runCommand( "admin",
                                     BSON( "renameCollection" << longTmpName <<
                                           "to" << ( dbname + "." + from ) <<
                                           "stayTemp" << false // explicit
                                           ),
                                     info ) ) {
                errmsg = "renameCollection failed: " + info.toString();
                return false;
            }

            return true;
        }
    } cmdConvertToCapped;

}
