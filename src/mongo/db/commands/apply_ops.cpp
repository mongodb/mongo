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
*/

#include <sstream>
#include <string>
#include <vector>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/commands.h"
#include "mongo/db/instance.h"
#include "mongo/db/matcher.h"
#include "mongo/db/repl/oplog.h"

namespace mongo {
    class ApplyOpsCmd : public Command {
    public:
        virtual bool slaveOk() const { return false; }
        virtual LockType locktype() const { return WRITE; }
        virtual bool lockGlobally() const { return true; } // SERVER-4328 todo : is global ok or does this take a long time? i believe multiple ns used so locking individually requires more analysis
        ApplyOpsCmd() : Command( "applyOps" ) {}
        virtual void help( stringstream &help ) const {
            help << "internal (sharding)\n{ applyOps : [ ] , preCondition : [ { ns : ... , q : ... , res : ... } ] }";
        }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            // applyOps can do pretty much anything, so require all privileges.
            out->push_back(Privilege(PrivilegeSet::WILDCARD_RESOURCE,
                                     AuthorizationSession::getAllUserActions()));
        }
        virtual bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl) {

            if ( cmdObj.firstElement().type() != Array ) {
                errmsg = "ops has to be an array";
                return false;
            }

            BSONObj ops = cmdObj.firstElement().Obj();

            {
                // check input
                BSONObjIterator i( ops );
                while ( i.more() ) {
                    BSONElement e = i.next();
                    if ( e.type() == Object )
                        continue;
                    errmsg = "op not an object: ";
                    errmsg += e.fieldName();
                    return false;
                }
            }

            if ( cmdObj["preCondition"].type() == Array ) {
                BSONObjIterator i( cmdObj["preCondition"].Obj() );
                while ( i.more() ) {
                    BSONObj f = i.next().Obj();

                    BSONObj realres = db.findOne( f["ns"].String() , f["q"].Obj() );

                    Matcher m( f["res"].Obj() );
                    if ( ! m.matches( realres ) ) {
                        result.append( "got" , realres );
                        result.append( "whatFailed" , f );
                        errmsg = "pre-condition failed";
                        return false;
                    }
                }
            }

            // apply
            int num = 0;
            int errors = 0;
            
            BSONObjIterator i( ops );
            BSONArrayBuilder ab;
            const bool alwaysUpsert = cmdObj.hasField("alwaysUpsert") ?
                    cmdObj["alwaysUpsert"].trueValue() : true;
            
            while ( i.more() ) {
                BSONElement e = i.next();
                const BSONObj& temp = e.Obj();
                
                Client::Context ctx(temp["ns"].String());
                bool failed = applyOperation_inlock(temp, false, alwaysUpsert);
                ab.append(!failed);
                if ( failed )
                    errors++;

                num++;
            }

            result.append( "applied" , num );
            result.append( "results" , ab.arr() );

            if ( ! fromRepl ) {
                // We want this applied atomically on slaves
                // so we re-wrap without the pre-condition for speed

                string tempNS = str::stream() << dbname << ".$cmd";

                // TODO: possibly use mutable BSON to remove preCondition field
                // once it is available
                BSONObjIterator iter(cmdObj);
                BSONObjBuilder cmdBuilder;

                while (iter.more()) {
                    BSONElement elem(iter.next());
                    if (strcmp(elem.fieldName(), "preCondition") != 0) {
                        cmdBuilder.append(elem);
                    }
                }

                logOp("c", tempNS.c_str(), cmdBuilder.done());
            }

            return errors == 0;
        }

        DBDirectClient db;

    } applyOpsCmd;

}
