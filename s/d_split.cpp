// d_split.cpp

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

#include "pch.h"
#include <map>
#include <string>

#include "../db/btree.h"
#include "../db/commands.h"
#include "../db/dbmessage.h"
#include "../db/jsobj.h"
#include "../db/query.h"
#include "../db/queryoptimizer.h"

namespace mongo {

    // TODO: Fold these checks into each command.
    static IndexDetails *cmdIndexDetailsForRange( const char *ns, string &errmsg, BSONObj &min, BSONObj &max, BSONObj &keyPattern ) {
        if ( ns[ 0 ] == '\0' || min.isEmpty() || max.isEmpty() ) {
            errmsg = "invalid command syntax (note: min and max are required)";
            return 0;
        }
        return indexDetailsForRange( ns, errmsg, min, max, keyPattern );
    }


    class CmdMedianKey : public Command {
    public:
        CmdMedianKey() : Command( "medianKey" ) {}
        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return READ; } 
        virtual void help( stringstream &help ) const {
            help << "internal.\nexample: { medianKey:\"blog.posts\", keyPattern:{x:1}, min:{x:10}, max:{x:55} }\n"
                "NOTE: This command may take awhile to run";
        }
        bool run(const string& dbname, BSONObj& jsobj, string& errmsg, BSONObjBuilder& result, bool fromRepl ){
            const char *ns = jsobj.getStringField( "medianKey" );
            BSONObj min = jsobj.getObjectField( "min" );
            BSONObj max = jsobj.getObjectField( "max" );
            BSONObj keyPattern = jsobj.getObjectField( "keyPattern" );
            
            Client::Context ctx( ns );

            IndexDetails *id = cmdIndexDetailsForRange( ns, errmsg, min, max, keyPattern );
            if ( id == 0 )
                return false;

            int num = 0;
            NamespaceDetails *d = nsdetails(ns);
            int idxNo = d->idxNo(*id);
            for( BtreeCursor c( d, idxNo, *id, min, max, false, 1 ); c.ok(); c.advance(), ++num );
            num /= 2;
            BtreeCursor c( d, idxNo, *id, min, max, false, 1 );
            for( ; num; c.advance(), --num );

            ostringstream os;
            os << "Finding median for index: " << keyPattern << " between " << min << " and " << max;
            logIfSlow( os.str() );

            if ( !c.ok() ) {
                errmsg = "no index entries in the specified range";
                return false;
            }

            result.append( "median", c.prettyKey( c.currKey() ) );
            return true;
        }
    } cmdMedianKey;

}  // namespace mongo
