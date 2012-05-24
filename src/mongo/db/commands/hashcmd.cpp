/* hashcmd.cpp
 *
 * Defines a shell command for hashing a BSONElement value
 */


/**
*    Copyright (C) 2012 10gen Inc.
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

#include "mongo/db/commands.h"
#include "mongo/db/hasher.h"

namespace mongo {

    class CmdHashElt : public Command {
    public:
        CmdHashElt() : Command("_hashBSONElement") {};
        virtual LockType locktype() const { return NONE; }
        virtual bool slaveOk() const { return true; }
        virtual void help( stringstream& help ) const {
            help << "returns the hash of the first BSONElement val in a BSONObj";
        }

        /* CmdObj has the form {"hash" : <thingToHash>}
         * or {"hash" : <thingToHash>, "seed" : <number> }
         * Result has the form
         * {"key" : <thingTohash>, "seed" : <int>, "out": NumberLong(<hash>)}
         *
         * Example use in the shell:
         *> db.runCommand({hash: "hashthis", seed: 1})
         *> {"key" : "hashthis",
         *>  "seed" : 1,
         *>  "out" : NumberLong(6271151123721111923),
         *>  "ok" : 1 }
         **/
        bool run( const string& db,
                  BSONObj& cmdObj,
                  int options, string& errmsg,
                  BSONObjBuilder& result,
                  bool fromRepl = false ){
            result.appendAs(cmdObj.firstElement(),"key");

            int seed = 0;
            if (cmdObj.hasField("seed")){
                if (! cmdObj["seed"].isNumber()) {
                    errmsg += "seed must be a number";
                    return false;
                }
                seed = cmdObj["seed"].numberInt();
            }
            result.append( "seed" , seed );

            result.append( "out" , BSONElementHasher::hash64( cmdObj.firstElement() , seed ) );
            return true;
        }
    } cmdHashElt;
}
