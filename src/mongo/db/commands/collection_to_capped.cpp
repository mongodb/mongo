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

#include "mongo/platform/basic.h"

#include "mongo/db/background.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/operation_context_impl.h"

namespace mongo {

    Status cloneCollectionAsCapped( OperationContext* txn,
                                    Database* db,
                                    const string& shortFrom,
                                    const string& shortTo,
                                    double size,
                                    bool temp,
                                    bool logForReplication ) {

        string fromNs = db->name() + "." + shortFrom;
        string toNs = db->name() + "." + shortTo;

        Collection* fromCollection = db->getCollection( txn, fromNs );
        if ( !fromCollection )
            return Status( ErrorCodes::NamespaceNotFound,
                           str::stream() << "source collection " << fromNs <<  " does not exist" );

        if ( db->getCollection( txn, toNs ) )
            return Status( ErrorCodes::NamespaceExists, "to collection already exists" );

        // create new collection
        {
            Client::Context ctx(txn,  toNs );
            BSONObjBuilder spec;
            spec.appendBool( "capped", true );
            spec.append( "size", size );
            if ( temp )
                spec.appendBool( "temp", true );

            Status status = userCreateNS( txn, ctx.db(), toNs, spec.done(), logForReplication );
            if ( !status.isOK() )
                return status;
        }

        Collection* toCollection = db->getCollection( txn, toNs );
        invariant( toCollection ); // we created above

        // how much data to ignore because it won't fit anyway
        // datasize and extentSize can't be compared exactly, so add some padding to 'size'
        long long excessSize =
            static_cast<long long>( fromCollection->dataSize() -
                                    ( toCollection->getRecordStore()->storageSize() * 2 ) );

        scoped_ptr<Runner> runner( InternalPlanner::collectionScan(fromNs,
                                                                   fromCollection,
                                                                   InternalPlanner::FORWARD ) );


        while ( true ) {
            BSONObj obj;
            Runner::RunnerState state = runner->getNext(&obj, NULL);

            switch( state ) {
            case Runner::RUNNER_EOF:
                return Status::OK();
            case Runner::RUNNER_DEAD:
                db->dropCollection( txn, toNs );
                return Status( ErrorCodes::InternalError, "runner turned dead while iterating" );
            case Runner::RUNNER_ERROR:
                return Status( ErrorCodes::InternalError, "runner error while iterating" );
            case Runner::RUNNER_ADVANCED:
                if ( excessSize > 0 ) {
                    excessSize -= ( 4 * obj.objsize() ); // 4x is for padding, power of 2, etc...
                    continue;
                }

                toCollection->insertDocument( txn, obj, true );
                if ( logForReplication )
                    repl::logOp(txn, "i", toNs.c_str(), obj);
                txn->recoveryUnit()->commitIfNeeded();
            }
        }

        invariant( false ); // unreachable
    }

    /* convertToCapped seems to use this */
    class CmdCloneCollectionAsCapped : public Command {
    public:
        CmdCloneCollectionAsCapped() : Command( "cloneCollectionAsCapped" ) {}
        virtual bool slaveOk() const { return false; }
        virtual bool isWriteCommandForConfigServer() const { return true; }
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
        bool run(OperationContext* txn, const string& dbname, BSONObj& jsobj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string from = jsobj.getStringField( "cloneCollectionAsCapped" );
            string to = jsobj.getStringField( "toCollection" );
            double size = jsobj.getField( "size" ).number();
            bool temp = jsobj.getField( "temp" ).trueValue();

            if ( from.empty() || to.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            Lock::DBWrite dbXLock(txn->lockState(), dbname);
            WriteUnitOfWork wunit(txn->recoveryUnit());
            Client::Context ctx(txn, dbname);

            Status status = cloneCollectionAsCapped( txn, ctx.db(), from, to, size, temp, true );
            if (status.isOK()) {
                wunit.commit();
            }
            return appendCommandStatus( result, status );
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
        virtual bool isWriteCommandForConfigServer() const { return true; }
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

        virtual std::vector<BSONObj> stopIndexBuilds(OperationContext* opCtx,
                                                     Database* db,
                                                     const BSONObj& cmdObj) {
            std::string collName = cmdObj.firstElement().valuestr();
            std::string ns = db->name() + "." + collName;

            IndexCatalog::IndexKillCriteria criteria;
            criteria.ns = ns;
            Collection* coll = db->getCollection(opCtx, ns);
            if (coll) {
                return IndexBuilder::killMatchingIndexBuilds(coll, criteria);
            }
            return std::vector<BSONObj>();
        }

        bool run(OperationContext* txn,
                 const string& dbname,
                 BSONObj& jsobj,
                 int,
                 string& errmsg,
                 BSONObjBuilder& result,
                 bool fromRepl ) {
            // calls renamecollection which does a global lock, so we must too:
            //
            Lock::GlobalWrite globalWriteLock(txn->lockState());
            WriteUnitOfWork wunit(txn->recoveryUnit());
            Client::Context ctx(txn, dbname);

            Database* db = ctx.db();

            stopIndexBuilds(txn, db, jsobj);
            BackgroundOperation::assertNoBgOpInProgForDb(dbname.c_str());

            string shortSource = jsobj.getStringField( "convertToCapped" );
            string longSource = dbname + "." + shortSource;
            double size = jsobj.getField( "size" ).number();

            if ( shortSource.empty() || size == 0 ) {
                errmsg = "invalid command spec";
                return false;
            }

            string shortTmpName = str::stream() << "tmp.convertToCapped." << shortSource;
            string longTmpName = str::stream() << dbname << "." << shortTmpName;

            if ( db->getCollection( txn, longTmpName ) ) {
                Status status = db->dropCollection( txn, longTmpName );
                if ( !status.isOK() )
                    return appendCommandStatus( result, status );
            }

            Status status = cloneCollectionAsCapped( txn, db, shortSource, shortTmpName, size, true, false );

            if ( !status.isOK() )
                return appendCommandStatus( result, status );

            verify( db->getCollection( txn, longTmpName ) );

            status = db->dropCollection( txn, longSource );
            if ( !status.isOK() )
                return appendCommandStatus( result, status );

            status = db->renameCollection( txn, longTmpName, longSource, false );
            if ( !status.isOK() )
                return appendCommandStatus( result, status );

            if (!fromRepl)
                repl::logOp(txn, "c",(dbname + ".$cmd").c_str(), jsobj);

            wunit.commit();
            return true;
        }
    } cmdConvertToCapped;

}
