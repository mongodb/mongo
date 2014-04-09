// create_indexes.cpp

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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/s/d_logic.h"
#include "mongo/s/shard_key_pattern.h"

namespace mongo {

    /**
     * { createIndexes : "bar", indexes : [ { ns : "test.bar", key : { x : 1 }, name: "x_1" } ] }
     */
    class CmdCreateIndex : public Command {
    public:
        CmdCreateIndex() : Command( "createIndexes" ){}

        virtual bool isWriteCommandForConfigServer() const { return false; }
        virtual bool logTheOp() { return false; }
        virtual bool slaveOk() const { return false; } // TODO: this could be made true...

        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            ActionSet actions;
            actions.addAction(ActionType::createIndex);
            Privilege p(parseResourcePattern(dbname, cmdObj), actions);
            if ( client->getAuthorizationSession()->isAuthorizedForPrivilege(p) )
                return Status::OK();
            return Status(ErrorCodes::Unauthorized, "Unauthorized");
        }


        BSONObj _addNsToSpec( const NamespaceString& ns, const BSONObj& obj ) {
            BSONObjBuilder b;
            b.append( "ns", ns );
            b.appendElements( obj );
            return b.obj();
        }

        virtual bool run( const string& dbname, BSONObj& cmdObj, int options,
                          string& errmsg, BSONObjBuilder& result,
                          bool fromRepl = false ) {

            // ---  parse

            NamespaceString ns( dbname, cmdObj[name].String() );
            Status status = userAllowedWriteNS( ns );
            if ( !status.isOK() )
                return appendCommandStatus( result, status );

            if ( cmdObj["indexes"].type() != Array ) {
                errmsg = "indexes has to be an array";
                result.append( "cmdObj", cmdObj );
                return false;
            }

            std::vector<BSONObj> specs;
            {
                BSONObjIterator i( cmdObj["indexes"].Obj() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() != Object ) {
                        errmsg = "everything in indexes has to be an Object";
                        result.append( "cmdObj", cmdObj );
                        return false;
                    }
                    specs.push_back( e.Obj() );
                }
            }

            if ( specs.size() == 0 ) {
                errmsg = "no indexes to add";
                return false;
            }

            // check specs
            for ( size_t i = 0; i < specs.size(); i++ ) {
                BSONObj spec = specs[i];
                if ( spec["ns"].eoo() ) {
                    spec = _addNsToSpec( ns, spec );
                    specs[i] = spec;
                }

                if ( spec["ns"].type() != String ) {
                    errmsg = "spec has no ns";
                    result.append( "spec", spec );
                    return false;
                }
                if ( ns != spec["ns"].String() ) {
                    errmsg = "namespace mismatch";
                    result.append( "spec", spec );
                    return false;
                }
            }


            {
                // We first take a read lock to see if we need to do anything
                // as many calls are ensureIndex (and hence no-ops), this is good so its a shared
                // lock for common calls. We only take write lock if needed.
                // Note: createIndexes command does not currently respect shard versioning.
                Client::ReadContext readContext( ns,
                                                 storageGlobalParams.dbpath,
                                                 false /* doVersion */ );
                const Collection* collection = readContext.ctx().db()->getCollection( ns.ns() );
                if ( collection ) {
                    for ( size_t i = 0; i < specs.size(); i++ ) {
                        BSONObj spec = specs[i];
                        StatusWith<BSONObj> statusWithSpec =
                            collection->getIndexCatalog()->prepareSpecForCreate( spec );
                        status = statusWithSpec.getStatus();
                        if ( status.code() == ErrorCodes::IndexAlreadyExists ) {
                            specs.erase( specs.begin() + i );
                            i--;
                            continue;
                        }
                        if ( !status.isOK() )
                            return appendCommandStatus( result, status );
                    }

                    if ( specs.size() == 0 ) {
                        result.append( "numIndexesBefore",
                                       collection->getIndexCatalog()->numIndexesTotal() );
                        result.append( "note", "all indexes already exist" );
                        return true;
                    }

                    // need to create index
                }
            }

            // now we know we have to create index(es)
            // Note: createIndexes command does not currently respect shard versioning.
            Client::WriteContext writeContext( ns.ns(),
                                               storageGlobalParams.dbpath,
                                               false /* doVersion */ );
            Database* db = writeContext.ctx().db();

            Collection* collection = db->getCollection( ns.ns() );
            result.appendBool( "createdCollectionAutomatically", collection == NULL );
            if ( !collection ) {
                collection = db->createCollection( ns.ns() );
                invariant( collection );
            }

            result.append( "numIndexesBefore", collection->getIndexCatalog()->numIndexesTotal() );

            for ( size_t i = 0; i < specs.size(); i++ ) {
                BSONObj spec = specs[i];

                if ( spec["unique"].trueValue() ) {
                    status = checkUniqueIndexConstraints( ns.ns(), spec["key"].Obj() );

                    if ( !status.isOK() ) {
                        appendCommandStatus( result, status );
                        return false;
                    }
                }

                status = collection->getIndexCatalog()->createIndex( spec, true );
                if ( status.code() == ErrorCodes::IndexAlreadyExists ) {
                    if ( !result.hasField( "note" ) )
                        result.append( "note", "index already exists" );
                    continue;
                }

                if ( !status.isOK() ) {
                    appendCommandStatus( result, status );
                    return false;
                }
            }

            result.append( "numIndexesAfter", collection->getIndexCatalog()->numIndexesTotal() );
            if ( !fromRepl ) {
                string cmdNs = ns.getCommandNS();
                logOp( "c", cmdNs.c_str(), cmdObj );
            }

            return true;
        }

    private:
        static Status checkUniqueIndexConstraints(const StringData& ns,
                                                  const BSONObj& newIdxKey) {
            Lock::assertWriteLocked( ns );

            if ( shardingState.enabled() ) {
                CollectionMetadataPtr metadata(
                        shardingState.getCollectionMetadata( ns.toString() ));

                if ( metadata ) {
                    BSONObj shardKey(metadata->getKeyPattern());
                    if ( !isUniqueIndexCompatible( shardKey, newIdxKey )) {
                        return Status(ErrorCodes::CannotCreateIndex,
                                str::stream() << "cannot create unique index over " << newIdxKey
                                              << " with shard key pattern " << shardKey);
                    }
                }
            }

            return Status::OK();
        }

    } cmdCreateIndex;

}
