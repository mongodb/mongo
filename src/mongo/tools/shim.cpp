// shim.cpp

/**
*    Copyright (C) 2014 MongoDB, INC.
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
*    must comply with the GNU Affero General Public License in all respects
*    for all of the code used other than as permitted herein. If you modify
*    file(s) with this exception, you may extend this exception to your
*    version of the file(s), but you are not obligated to do so. If you do not
*    wish to do so, delete this exception statement from your version. If you
*    delete this exception statement from all source files in the program,
*    then also delete it in the license file.
*/

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/json.h"
#include "mongo/tools/mongoshim_options.h"
#include "mongo/tools/tool.h"
#include "mongo/tools/tool_logger.h"
#include "mongo/util/options_parser/option_section.h"

using std::string;
using std::vector;

using namespace mongo;

class Shim : public BSONTool {
public:
    Shim() : BSONTool() { }

    virtual void printHelp( ostream & out ) {
        printMongoShimHelp(&out);
    }

    virtual void gotObject( const BSONObj& obj ) {
        if (mongoShimGlobalParams.upsert) {
            BSONObjBuilder b;
            invariant(!mongoShimGlobalParams.upsertFields.empty());
            for (vector<string>::const_iterator it = mongoShimGlobalParams.upsertFields.begin(),
                 end = mongoShimGlobalParams.upsertFields.end(); it != end; ++it) {
                BSONElement e = obj.getFieldDotted(it->c_str());
                // If we cannot construct a valid query using the provided upsertFields,
                // insert the object and skip the rest of the fields.
                if (e.eoo()) {
                    conn().insert(_ns, obj);
                    return;
                }
                b.appendAs(e, *it);
            }
            Query query(b.obj());
            bool upsert = true;
            bool multi = false;
            conn().update(_ns, query, obj, upsert, multi);
        }
        else {
            conn().insert(_ns, obj );
        }
    }

    int doRun() {

        try {
            _ns = getNS();
        }
        catch (...) {
            printHelp(cerr);
            return 1;
        }

        if ( mongoShimGlobalParams.load ) {
            if ( mongoShimGlobalParams.drop ) {
                conn().dropCollection( _ns );
            }
            processFile( "-" );
        }
        else {
            ostream *out = &cout;

            Query q(mongoShimGlobalParams.query);
            if (mongoShimGlobalParams.sort != "") {
                BSONObj sortSpec = mongo::fromjson(mongoShimGlobalParams.sort);
                q.sort(sortSpec);
            }

            if (mongoShimGlobalParams.snapShotQuery) {
                q.snapshot();
            }

            auto_ptr<DBClientCursor> cursor = conn().query(_ns,
                                                           q,
                                                           mongoShimGlobalParams.limit,
                                                           mongoShimGlobalParams.skip,
                                                           NULL,
                                                           0,
                                                           QueryOption_NoCursorTimeout);

            while ( cursor->more() ) {
                BSONObj obj = cursor->next();
                out->write( obj.objdata(), obj.objsize() );
            }
        }

        return 0;
    }

private:
    string _ns;
};

REGISTER_MONGO_TOOL(Shim);
