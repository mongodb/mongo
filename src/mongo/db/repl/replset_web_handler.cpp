/**
*    Copyright (C) 2008 10gen Inc.
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

#include "mongo/db/cmdline.h"
#include "mongo/db/dbwebserver.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/mongoutils/html.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    extern void fillRsLog(std::stringstream&);

namespace {

    using namespace bson;
    using namespace mongoutils;
    using namespace mongoutils::html;

    class ReplSetHandler : public DbWebHandler {
    public:
        ReplSetHandler() : DbWebHandler( "_replSet" , 1 , true ) {}

        virtual bool handles( const string& url ) const {
            return startsWith( url , "/_replSet" );
        }

        virtual void handle( const char *rq, const std::string& url, BSONObj params,
                             string& responseMsg, int& responseCode,
                             vector<string>& headers,  const SockAddr &from ) {

            if( url == "/_replSetOplog" ) {
                responseMsg = _replSetOplog(params);
            }
            else
                responseMsg = _replSet();
            responseCode = 200;
        }

        string _replSetOplog(bo parms) {
            int _id = (int) str::toUnsigned( parms["_id"].String() );

            stringstream s;
            string t = "Replication oplog";
            s << start(t);
            s << p(t);

            if( theReplSet == 0 ) {
                if( cmdLine._replSet.empty() )
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://dochub.mongodb.org/core/replicasetconfiguration#ReplicaSetConfiguration-InitialSetup", "", "initiated")
                           + ".<br>" + ReplSet::startupStatusMsg.get());
                }
            }
            else {
                try {
                    theReplSet->getOplogDiagsAsHtml(_id, s);
                }
                catch(std::exception& e) {
                    s << "error querying oplog: " << e.what() << '\n';
                }
            }

            s << _end();
            return s.str();
        }

        /* /_replSet show replica set status in html format */
        string _replSet() {
            stringstream s;
            s << start("Replica Set Status " + prettyHostName());
            s << p( a("/", "back", "Home") + " | " +
                    a("/local/system.replset/?html=1", "", "View Replset Config") + " | " +
                    a("/replSetGetStatus?text=1", "", "replSetGetStatus") + " | " +
                    a("http://dochub.mongodb.org/core/replicasets", "", "Docs")
                  );

            if( theReplSet == 0 ) {
                if( cmdLine._replSet.empty() )
                    s << p("Not using --replSet");
                else  {
                    s << p("Still starting up, or else set is not yet " + a("http://dochub.mongodb.org/core/replicasetconfiguration#ReplicaSetConfiguration-InitialSetup", "", "initiated")
                           + ".<br>" + ReplSet::startupStatusMsg.get());
                }
            }
            else {
                try {
                    theReplSet->summarizeAsHtml(s);
                }
                catch(...) { s << "error summarizing replset status\n"; }
            }
            s << p("Recent replset log activity:");
            fillRsLog(s);
            s << _end();
            return s.str();
        }



    } replSetHandler;

}  // namespace
}  // namespace mongo
