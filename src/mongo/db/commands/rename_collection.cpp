// rename_collection.cpp

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

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/instance.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/structure/collection_iterator.h"

namespace mongo {

    class CmdRenameCollection : public Command {
    public:
        CmdRenameCollection() : Command( "renameCollection" ) {}
        virtual bool adminOnly() const {
            return true;
        }
        virtual bool slaveOk() const {
            return false;
        }
        virtual LockType locktype() const { return WRITE; }
        virtual bool lockGlobally() const { return true; }
        virtual bool logTheOp() {
            return true; // can't log steps when doing fast rename within a db, so always log the op rather than individual steps comprising it.
        }
        virtual Status checkAuthForCommand(ClientBasic* client,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
            return rename_collection::checkAuthForRenameCollectionCommand(client, dbname, cmdObj);
        }
        virtual void help( stringstream &help ) const {
            help << " example: { renameCollection: foo.a, to: bar.b }";
        }

        virtual std::vector<BSONObj> stopIndexBuilds(Database* db,
                                                     const BSONObj& cmdObj) {
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );

            IndexCatalog::IndexKillCriteria criteria;
            criteria.ns = source;
            std::vector<BSONObj> prelim = 
                IndexBuilder::killMatchingIndexBuilds(db->getCollection(source), criteria);

            std::vector<BSONObj> indexes;

            for (int i = 0; i < static_cast<int>(prelim.size()); i++) {
                // Change the ns
                BSONObj stripped = prelim[i].removeField("ns");
                BSONObjBuilder builder;
                builder.appendElements(stripped);
                builder.append("ns", target);
                indexes.push_back(builder.obj());
            }

            return indexes;
        }

        virtual void restoreIndexBuildsOnSource(std::vector<BSONObj> indexesInProg, std::string source) {
            IndexBuilder::restoreIndexes( indexesInProg );
        }

        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );

            if ( !NamespaceString::validCollectionComponent(target.c_str()) ) {
                errmsg = "invalid collection name: " + target;
                return false;
            }
            if ( source.empty() || target.empty() ) {
                errmsg = "invalid command syntax";
                return false;
            }

            if (!fromRepl) { // If it got through on the master, need to allow it here too
                Status sourceStatus = userAllowedWriteNS(source);
                if (!sourceStatus.isOK()) {
                    errmsg = "error with source namespace: " + sourceStatus.reason();
                    return false;
                }

                Status targetStatus = userAllowedWriteNS(target);
                if (!targetStatus.isOK()) {
                    errmsg = "error with target namespace: " + targetStatus.reason();
                    return false;
                }
            }

            string sourceDB = nsToDatabase(source);
            string targetDB = nsToDatabase(target);

            bool capped = false;
            long long size = 0;
            std::vector<BSONObj> indexesInProg;

            {
                Client::Context srcCtx( source );
                Collection* sourceColl = srcCtx.db()->getCollection( source );

                if ( !sourceColl ) {
                    errmsg = "source namespace does not exist";
                    return false;
                }

                // Ensure that collection name does not exceed maximum length.
                // Ensure that index names do not push the length over the max.
                // Iterator includes unfinished indexes.
                IndexCatalog::IndexIterator sourceIndIt =
                    sourceColl->getIndexCatalog()->getIndexIterator( true );
                int longestIndexNameLength = 0;
                while ( sourceIndIt.more() ) {
                    int thisLength = sourceIndIt.next()->indexName().length();
                    if ( thisLength > longestIndexNameLength )
                        longestIndexNameLength = thisLength;
                }

                unsigned int longestAllowed =
                    min(int(Namespace::MaxNsColletionLen),
                        int(Namespace::MaxNsLen) - 2/*strlen(".$")*/ - longestIndexNameLength);
                if (target.size() > longestAllowed) {
                    StringBuilder sb;
                    sb << "collection name length of " << target.size()
                       << " exceeds maximum length of " << longestAllowed
                       << ", allowing for index names";
                    errmsg = sb.str();
                    return false;
                }

                {

                    indexesInProg = stopIndexBuilds( srcCtx.db(), cmdObj );
                    capped = sourceColl->isCapped();
                    if ( capped ) {
                        size = sourceColl->storageSize();
                    }
                }
            }

            {
                Client::Context ctx( target );

                // Check if the target namespace exists and if dropTarget is true.
                // If target exists and dropTarget is not true, return false.
                if ( ctx.db()->getCollection( target ) ) {
                    if ( !cmdObj["dropTarget"].trueValue() ) {
                        errmsg = "target namespace exists";
                        return false;
                    }

                    Status s = cc().database()->dropCollection( target );
                    if ( !s.isOK() ) {
                        errmsg = s.toString();
                        restoreIndexBuildsOnSource( indexesInProg, source );
                        return false;
                    }
                }

                // If we are renaming in the same database, just
                // rename the namespace and we're done.
                if ( sourceDB == targetDB ) {
                    Status s = ctx.db()->renameCollection( source, target,
                                                           cmdObj["stayTemp"].trueValue() );
                    if ( !s.isOK() ) {
                        errmsg = s.toString();
                        restoreIndexBuildsOnSource( indexesInProg, source );
                        return false;
                    }
                    return true;
                }

                // Otherwise, we are enaming across databases, so we must copy all
                // the data and then remove the source collection.

                // Create the target collection.
                Collection* targetColl = NULL;
                if ( capped ) {
                    CollectionOptions options;
                    options.capped = true;
                    options.cappedSize = size;
                    options.setNoIdIndex();

                    targetColl = ctx.db()->createCollection( target, options );
                }
                else {
                    CollectionOptions options;
                    options.setNoIdIndex();
                    // No logOp necessary because the entire renameCollection command is one logOp.
                    targetColl = ctx.db()->createCollection( target, options );
                }
                if ( !targetColl ) {
                    errmsg = "Failed to create target collection.";
                    restoreIndexBuildsOnSource( indexesInProg, source );
                    return false;
                }
            }

            // Copy over all the data from source collection to target collection.
            bool insertSuccessful = true;
            boost::scoped_ptr<CollectionIterator> sourceIt;

            {
                Client::Context srcCtx( source );
                Collection* sourceColl = srcCtx.db()->getCollection( source );
                sourceIt.reset( sourceColl->getIterator( DiskLoc(), false, CollectionScanParams::FORWARD ) );
            }

            Collection* targetColl = NULL;
            while ( !sourceIt->isEOF() ) {
                BSONObj o;
                {
                    Client::Context srcCtx( source );
                    o = sourceIt->getNext().obj();
                }
                // Insert and check return status of insert.
                {
                    Client::Context ctx( target );
                    if ( !targetColl )
                        targetColl = ctx.db()->getCollection( target );
                    // No logOp necessary because the entire renameCollection command is one logOp.
                    Status s = targetColl->insertDocument( o, true ).getStatus();
                    if ( !s.isOK() ) {
                        insertSuccessful = false;
                        errmsg = s.toString();
                        break;
                    }
                }
            }

            // If inserts were unsuccessful, drop the target collection and return false.
            if ( !insertSuccessful ) {
                Client::Context ctx( target );
                Status s = ctx.db()->dropCollection( target );
                if ( !s.isOK() )
                    errmsg = s.toString();
                restoreIndexBuildsOnSource( indexesInProg, source );
                return false;
            }

            // Copy over the indexes to temp storage and then to the target..
            vector<BSONObj> copiedIndexes;
            bool indexSuccessful = true;
            {
                Client::Context srcCtx( source );
                Collection* sourceColl = srcCtx.db()->getCollection( source );
                IndexCatalog::IndexIterator sourceIndIt =
                    sourceColl->getIndexCatalog()->getIndexIterator( true );

                while ( sourceIndIt.more() ) {
                    BSONObj currIndex = sourceIndIt.next()->infoObj();

                    // Process the source index.
                    BSONObjBuilder b;
                    BSONObjIterator i( currIndex );
                    while( i.moreWithEOO() ) {
                        BSONElement e = i.next();
                        if ( e.eoo() )
                            break;
                        else if ( strcmp( e.fieldName(), "ns" ) == 0 )
                            b.append( "ns", target );
                        else
                            b.append( e );
                    }

                    BSONObj newIndex = b.obj();
                    copiedIndexes.push_back( newIndex );
                }
            }

            {
                Client::Context ctx( target );
                if ( !targetColl )
                    targetColl = ctx.db()->getCollection( target );

                for ( vector<BSONObj>::iterator it = copiedIndexes.begin();
                                                it != copiedIndexes.end(); ++it ) {
                    Status s = targetColl->getIndexCatalog()->createIndex( *it, true );
                    if ( !s.isOK() ) {
                        indexSuccessful = false;
                        errmsg = s.toString();
                        break;
                    }
                }

                // If indexes were unsuccessful, drop the target collection and return false.
                if ( !indexSuccessful ) {
                    Status s = ctx.db()->dropCollection( target );
                    if ( !s.isOK() )
                        errmsg = s.toString();
                    restoreIndexBuildsOnSource( indexesInProg, source );
                    return false;
                }
            }

            // Drop the source collection.
            {
                Client::Context srcCtx( source );
                Status s = srcCtx.db()->dropCollection( source );
                if ( !s.isOK() ) {
                    errmsg = s.toString();
                    restoreIndexBuildsOnSource( indexesInProg, source );
                    return false;
                }
            }

            return true;
        }
    } cmdrenamecollection;

}
