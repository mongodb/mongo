// dbhelpers.cpp

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

#include "stdafx.h"
#include "db.h"
#include "dbhelpers.h"
#include "query.h"
#include "json.h"

namespace mongo {

    void Helpers::ensureIndex(const char *ns, BSONObj keyPattern, const char *name) {
        NamespaceDetails *d = nsdetails(ns);
        if( d == 0 )
            return;

        for( int i = 0; i < d->nIndexes; i++ ) {
            if( d->indexes[i].keyPattern().woCompare(keyPattern) == 0 )
                return;
        }

        if( d->nIndexes >= MaxIndexes ) { 
            problem() << "Helper::ensureIndex fails, MaxIndexes exceeded " << ns << '\n';
            return;
        }

        string system_indexes = database->name + ".system.indexes";

        BSONObjBuilder b;
        b.append("name", name);
        b.append("ns", ns);
        b.append("key", keyPattern);
        BSONObj o = b.done();

        theDataFileMgr.insert(system_indexes.c_str(), o.objdata(), o.objsize());
    }

    /* fetch a single object from collection ns that matches query 
       set your db context first
    */
    bool Helpers::findOne(const char *ns, BSONObj query, BSONObj& result, bool requireIndex) { 
        auto_ptr<Cursor> c = getIndexCursor(ns, query, emptyObj);
        if ( c.get() == 0 ) {
            massert("index missing on server's findOne invocation", !requireIndex);
            c = DataFileMgr::findAll(ns);
        }

        if( !c->ok() ) 
            return false;

        JSMatcher matcher(query, c->indexKeyPattern());

        do {
            BSONObj js = c->current();
            if( matcher.matches(js) ) { 
                result = js;
                return true;
            }
            c->advance();
        } while( c->ok() );

        return false;
    }

    int test2_dbh() {
        dblock lk;
        DBContext c("dwight.foo");
        BSONObj q = fromjson("{\"x\":9}");
        BSONObj result;

        {
            BSONObj kp = fromjson("{\"x\":1}");
            Helpers::ensureIndex("dwight.foo", kp, "x_1");
        }

        cout << Helpers::findOne("dwight.foo", q, result, true) << endl;
        cout << result.toString() << endl;
        return 0;
    }

    /* Get the first object from a collection.  Generally only useful if the collection
       only ever has a single object -- which is a "singleton collection.

       Returns: true if object exists.
    */
    bool Helpers::getSingleton(const char *ns, BSONObj& result) {
        DBContext context(ns);

        auto_ptr<Cursor> c = DataFileMgr::findAll(ns);
        if ( !c->ok() )
            return false;

        result = c->current();
        return true;
    }

    void Helpers::putSingleton(const char *ns, BSONObj obj) {
        DBContext context(ns);
        stringstream ss;
        updateObjects(ns, obj, /*pattern=*/emptyObj, /*upsert=*/true, ss);
    }

    void Helpers::emptyCollection(const char *ns) {
        DBContext context(ns);
        deleteObjects(ns, emptyObj, false);
    }

} // namespace mongo
