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
*/

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/commands/rename_collection.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builder.h"
#include "mongo/db/instance.h"
#include "mongo/db/namespace_string.h"

namespace mongo {

    class CmdRenameCollection : public Command {
    public:
        // Absolute maximum Namespace is 128 incl NUL
        // Namespace is 128 minus .$ and $extra so 120 before additions
        static const int maxNamespaceLen = 120;
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
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            rename_collection::addPrivilegesRequiredForRenameCollection(cmdObj, out);
        }
        virtual void help( stringstream &help ) const {
            help << " example: { renameCollection: foo.a, to: bar.b }";
        }

        virtual std::vector<BSONObj> stopIndexBuilds(const std::string& dbname,
                                                     const BSONObj& cmdObj) {
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );

            BSONObj criteria = BSON("op" << "insert" << "ns" << dbname+".system.indexes" <<
                                    "insert.ns" << source);

            std::vector<BSONObj> prelim = IndexBuilder::killMatchingIndexBuilds(criteria);
            std::vector<BSONObj> indexes;

            for (int i = 0; i < static_cast<int>(prelim.size()); i++) {
                // Change the ns
                BSONObj stripped = prelim[i].removeField("ns");
                BSONObjBuilder builder;
                builder.appendElements(stripped);
                builder.append("ns", target);
                indexes.push_back(builder.done());
            }

            return indexes;
        }

        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {
            string source = cmdObj.getStringField( name.c_str() );
            string target = cmdObj.getStringField( "to" );
            uassert(15967,"invalid collection name: " + target, NamespaceString::validCollectionName(target.c_str()));
            if ( source.empty() || target.empty() ) {
                errmsg = "invalid command syntax";
                return false;
            }

            string sourceDB = nsToDatabase(source);
            string targetDB = nsToDatabase(target);
            string databaseName = sourceDB;
            databaseName += ".system.indexes";

            int longestIndexNameLength = 0;
            vector<BSONObj> oldIndSpec = Helpers::findAll(databaseName, BSON("ns" << source));
            for (size_t i = 0; i < oldIndSpec.size(); ++i) {
                int thisLength = oldIndSpec[i].getField("name").valuesize();
                if (thisLength > longestIndexNameLength) {
                     longestIndexNameLength = thisLength;
                }
            }
            unsigned int longestAllowed = maxNamespaceLen - longestIndexNameLength - 1;
            if (target.size() > longestAllowed) {
                StringBuilder sb;
                sb << "collection name length of " << target.size()
                << " exceeds maximum length of " << longestAllowed
                << ", allowing for index names";
                uasserted(16451, sb.str());
            }

            bool capped = false;
            long long size = 0;
            std::vector<BSONObj> indexesInProg;
            {
                Client::Context ctx( source );
                NamespaceDetails *nsd = nsdetails( source );
                uassert( 10026 ,  "source namespace does not exist", nsd );
                indexesInProg = stopIndexBuilds(dbname, cmdObj);
                capped = nsd->isCapped();
                if ( capped )
                    for( DiskLoc i = nsd->firstExtent(); !i.isNull(); i = i.ext()->xnext )
                        size += i.ext()->length;
            }

            Client::Context ctx( target );

            if ( nsdetails( target ) ) {
                uassert( 10027 ,  "target namespace exists", cmdObj["dropTarget"].trueValue() );
                BSONObjBuilder bb( result.subobjStart( "dropTarget" ) );
                dropCollection( target , errmsg , bb );
                bb.done();
                if ( errmsg.size() > 0 )
                    return false;
            }


            // if we are renaming in the same database, just
            // rename the namespace and we're done.
            {
                if ( sourceDB == targetDB ) {
                    renameNamespace( source.c_str(), target.c_str(), cmdObj["stayTemp"].trueValue() );
                    // make sure we drop counters etc
                    Top::global.collectionDropped( source );
                    return true;
                }
            }

            // renaming across databases, so we must copy all
            // the data and then remove the source collection.
            BSONObjBuilder spec;
            if ( capped ) {
                spec.appendBool( "capped", true );
                spec.append( "size", double( size ) );
            }
            if ( !userCreateNS( target.c_str(), spec.done(), errmsg, false ) )
                return false;

            auto_ptr< DBClientCursor > c;
            DBDirectClient bridge;

            {
                c = bridge.query( source, BSONObj(), 0, 0, 0, fromRepl ? QueryOption_SlaveOk : 0 );
            }
            while( 1 ) {
                {
                    if ( !c->more() )
                        break;
                }
                BSONObj o = c->next();
                theDataFileMgr.insertWithObjMod( target.c_str(), o );
            }

            string sourceIndexes = nsToDatabase( source ) + ".system.indexes";
            string targetIndexes = nsToDatabase( target ) + ".system.indexes";
            {
                c = bridge.query( sourceIndexes, QUERY( "ns" << source ), 0, 0, 0, fromRepl ? QueryOption_SlaveOk : 0 );
            }
            while( 1 ) {
                {
                    if ( !c->more() )
                        break;
                }
                BSONObj o = c->next();
                BSONObjBuilder b;
                BSONObjIterator i( o );
                while( i.moreWithEOO() ) {
                    BSONElement e = i.next();
                    if ( e.eoo() )
                        break;
                    if ( strcmp( e.fieldName(), "ns" ) == 0 ) {
                        b.append( "ns", target );
                    }
                    else {
                        b.append( e );
                    }
                }
                BSONObj n = b.done();
                theDataFileMgr.insertWithObjMod( targetIndexes.c_str(), n );
            }

            {
                Client::Context ctx( source );
                dropCollection( source, errmsg, result );
                IndexBuilder::restoreIndexes(targetIndexes, indexesInProg);
            }
            return true;
        }
    } cmdrenamecollection;

}
