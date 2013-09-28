// dbhash.cpp

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

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/database.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/util/md5.hpp"
#include "mongo/util/timer.h"

namespace mongo {

    class DBHashCmd : public Command {
    public:
        DBHashCmd() : Command( "dbHash", false, "dbhash" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::dbHash);
            out->push_back(Privilege(ResourcePattern::forDatabaseName(dbname), actions));
        }
        virtual bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool) {
            Timer timer;

            set<string> desiredCollections;
            if ( cmdObj["collections"].type() == Array ) {
                BSONObjIterator i( cmdObj["collections"].Obj() );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() != String ) {
                        errmsg = "collections entries have to be strings";
                        return false;
                    }
                    desiredCollections.insert( e.String() );
                }
            }

            list<string> colls;
            Database* db = cc().database();
            if ( db )
                db->namespaceIndex().getNamespaces( colls );
            colls.sort();

            result.appendNumber( "numCollections" , (long long)colls.size() );
            result.append( "host" , prettyHostName() );

            md5_state_t globalState;
            md5_init(&globalState);

            BSONObjBuilder bb( result.subobjStart( "collections" ) );
            for ( list<string>::iterator i=colls.begin(); i != colls.end(); i++ ) {
                string fullCollectionName = *i;
                string shortCollectionName = fullCollectionName.substr( dbname.size() + 1 );

                if ( shortCollectionName.find( "system." ) == 0 )
                    continue;

                if ( desiredCollections.size() > 0 &&
                     desiredCollections.count( shortCollectionName ) == 0 )
                    continue;

                NamespaceDetails * nsd = nsdetails( fullCollectionName );

                // debug SERVER-761
                NamespaceDetails::IndexIterator ii = nsd->ii();
                while( ii.more() ) {
                    const IndexDetails &idx = ii.next();
                    if ( !idx.head.isValid() || !idx.info.isValid() ) {
                        log() << "invalid index for ns: " << fullCollectionName << " " << idx.head << " " << idx.info;
                        if ( idx.info.isValid() )
                            log() << " " << idx.info.obj();
                        log() << endl;
                    }
                }

                auto_ptr<Runner> runner;
                int idNum = nsd->findIdIndex();
                if ( idNum >= 0 ) {
                    runner.reset(InternalPlanner::indexScan(fullCollectionName,
                                                            nsd,
                                                            idNum,
                                                            BSONObj(),
                                                            BSONObj(),
                                                            false,
                                                            InternalPlanner::FORWARD,
                                                            InternalPlanner::IXSCAN_FETCH));
                }
                else if ( nsd->isCapped() ) {
                    runner.reset(InternalPlanner::collectionScan(fullCollectionName));
                }
                else {
                    log() << "can't find _id index for: " << fullCollectionName << endl;
                    continue;
                }

                md5_state_t st;
                md5_init(&st);

                long long n = 0;
                Runner::RunnerState state;
                BSONObj c;
                verify(NULL != runner.get());
                while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&c, NULL))) {
                    md5_append( &st , (const md5_byte_t*)c.objdata() , c.objsize() );
                    n++;
                }
                if (Runner::RUNNER_EOF != state) {
                    warning() << "error while hashing, db dropped? ns=" << fullCollectionName << endl;
                }
                md5digest d;
                md5_finish(&st, d);
                string hash = digestToString( d );

                bb.append( shortCollectionName, hash );

                md5_append( &globalState , (const md5_byte_t*)hash.c_str() , hash.size() );
            }
            bb.done();

            md5digest d;
            md5_finish(&globalState, d);
            string hash = digestToString( d );

            result.append( "md5" , hash );
            result.appendNumber( "timeMillis", timer.millis() );
            return 1;
        }

    } dbhashCmd;

}
