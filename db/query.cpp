// query.cpp

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
#include "query.h"
#include "pdfile.h"
#include "jsobj.h"
#include "../util/builder.h"
#include <time.h>
#include "introspect.h"
#include "btree.h"
#include "../util/lruishmap.h"
#include "javajs.h"
#include "json.h"
#include "repl.h"
#include "replset.h"
#include "scanandorder.h"

namespace mongo {

    /* We cut off further objects once we cross this threshold; thus, you might get
       a little bit more than this, it is a threshold rather than a limit.
    */
    const int MaxBytesToReturnToClientAtOnce = 4 * 1024 * 1024;

//ns->query->DiskLoc
    LRUishMap<BSONObj,DiskLoc,5> lrutest(123);

    int nextCursorId = 1;
    extern bool useCursors;

    void appendElementHandlingGtLt(BSONObjBuilder& b, BSONElement& e);

    int matchDirection( const BSONObj &index, const BSONObj &sort ) {
        int direction = 0;
        BSONObjIterator i( index );
        BSONObjIterator s( sort );
        while ( 1 ) {
            BSONElement ie = i.next();
            BSONElement se = s.next();
            if ( ie.eoo() ) {
                if ( !se.eoo() )
                    return 0;
                return direction;
            }
            if ( strcmp( ie.fieldName(), se.fieldName() ) != 0 )
                return 0;

            int d = ie.number() == se.number() ? 1 : -1;
            if ( direction == 0 )
                direction = d;
            else if ( direction != d )
                return 0;
        }
    }

    /* todo: _ cache query plans
             _ use index on partial match with the query

       parameters
         query - the query, e.g., { name: 'joe' }
         order - order by spec, e.g., { name: 1 } 1=ASC, -1=DESC
         simpleKeyMatch - set to true if the query is purely for a single key value
                          unchanged otherwise.
    */
    auto_ptr<Cursor> getIndexCursor(const char *ns, BSONObj& query, BSONObj& order, bool *simpleKeyMatch = 0, bool *isSorted = 0, string *hint = 0) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 ) return auto_ptr<Cursor>();

        if ( hint && !hint->empty() ) {
            /* todo: more work needed.  doesn't handle $lt & $gt for example.
                     waiting for query optimizer rewrite (see queryoptimizer.h) before finishing the work.
            */
            for (int i = 0; i < d->nIndexes; i++ ) {
                IndexDetails& ii = d->indexes[i];
                if ( ii.indexName() == *hint ) {
                    BSONObj startKey = ii.getKeyFromQuery(query);
                    int direction = 1;
                    if ( simpleKeyMatch )
                        *simpleKeyMatch = query.nFields() == startKey.nFields();
                    if ( isSorted ) *isSorted = false;
                    return auto_ptr<Cursor>(
                               new BtreeCursor(ii, startKey, direction, query));
                }
            }
        }

        if ( !order.isEmpty() ) {
            // order by
            for (int i = 0; i < d->nIndexes; i++ ) {
                BSONObj idxInfo = d->indexes[i].info.obj(); // { name:, ns:, key: }
                assert( strcmp(ns, idxInfo.getStringField("ns")) == 0 );
                BSONObj idxKey = idxInfo.getObjectField("key");
                int direction = matchDirection( idxKey, order );
                if ( direction != 0 ) {
                    BSONObjBuilder b;
                    DEV out() << " using index " << d->indexes[i].indexNamespace() << '\n';
                    if ( isSorted )
                        *isSorted = true;

                    return auto_ptr<Cursor>(new BtreeCursor(d->indexes[i], BSONObj(), direction, query));
                }
            }
        }

        // queryFields, e.g. { 'name' }
        set<string> queryFields;
        query.getFieldNames(queryFields);

        // regular query without order by
        for (int i = 0; i < d->nIndexes; i++ ) {
            BSONObj idxInfo = d->indexes[i].info.obj(); // { name:, ns:, key: }
            BSONObj idxKey = idxInfo.getObjectField("key");
            set<string> keyFields;
            idxKey.getFieldNames(keyFields);

            // keyFields: e.g. { "name" }
            bool match = keyFields == queryFields;
            if ( 0 && !match && queryFields.size() > 1 && simpleKeyMatch == 0 && keyFields.size() == 1 ) {
                // TEMP
                string s = *(keyFields.begin());
                match = queryFields.count(s) == 1;
            }

            if ( match ) {
                bool simple = true;
                //BSONObjBuilder b;
                BSONObj q = query.extractFieldsUnDotted(idxKey);
                assert(q.objsize() != 0); // guard against a seg fault if details is 0
                /* regexp: only supported if form is /^text/ */
                BSONObjBuilder b2;
                BSONObjIterator it(q);
                bool first = true;
                while ( it.more() ) {
                    BSONElement e = it.next();
                    if ( e.eoo() )
                        break;

                    // GT/LT
                    if ( e.type() == Object ) {
                        int op = getGtLtOp(e);
                        if ( op ) {
                            if ( !first || !it.next().eoo() ) {
                                // compound keys with GT/LT not supported yet via index.
                                goto fail;
                            }
                            if ( op >= JSMatcher::opIN ) {
                                // $in does not use an index (at least yet, should when # of elems is tiny)
                                // likewise $ne
                                goto fail;
                            }

                            {
                                BSONObjIterator k(e.embeddedObject());
                                k.next();
                                if ( !k.next().eoo() ) {
                                    /* compound query like { $lt : 9, $gt : 2 }
                                       for those our method below won't work.
                                       need more work on "stopOnMiss" in general -- may
                                       be issues with it.  so fix this to use index after
                                       that is fixed.
                                    */
                                    OCCASIONALLY out() << "finish query optimizer for lt gt compound\n";
                                    goto fail;
                                }
                            }

                            int direction = - JSMatcher::opDirection(op);
                            return auto_ptr<Cursor>( new BtreeCursor(
                                                         d->indexes[i],
                                                         BSONObj(),
                                                         direction, query) );
                        }
                    }

                    first = false;
                    if ( e.type() == RegEx ) {
                        simple = false;
                        if ( *e.regexFlags() )
                            goto fail;
                        const char *re = e.regex();
                        const char *p = re;
                        if ( *p++ != '^' ) goto fail;
                        while ( *p ) {
                            if ( *p == ' ' || (*p>='0'&&*p<='9') || (*p>='@'&&*p<='Z') || (*p>='a'&&*p<='z') )
                                ;
                            else
                                goto fail;
                            p++;
                        }
                        if ( it.more() && !it.next().eoo() ) // we must be the last part of the key (for now until we are smarter)
                            goto fail;
                        // ok!
                        b2.append(e.fieldName(), re+1);
                        break;
                    }
                    else {
                        b2.append(e);
                        //appendElementHandlingGtLt(b2, e);
                    }
                }
                BSONObj q2 = b2.done();
                DEV out() << "using index " << d->indexes[i].indexNamespace() << endl;
                if ( simple && simpleKeyMatch ) *simpleKeyMatch = true;
                return auto_ptr<Cursor>(
                           new BtreeCursor(d->indexes[i], q2, 1, query));
            }
        }

fail:
        DEV out() << "getIndexCursor fail " << ns << '\n';
        return auto_ptr<Cursor>();
    }

    /* ns:      namespace, e.g. <database>.<collection>
       pattern: the "where" clause / criteria
       justOne: stop after 1 match
    */
    int deleteObjects(const char *ns, BSONObj pattern, bool justOne, bool god) {
        if ( strstr(ns, ".system.") && !god ) {
            /*if( strstr(ns, ".system.namespaces") ){
            	out() << "info: delete on system namespace " << ns << '\n';
            }
            else if( strstr(ns, ".system.indexes") ) {
            	out() << "info: delete on system namespace " << ns << '\n';
            }
            else*/ {
                out() << "ERROR: attempt to delete in system namespace " << ns << endl;
                return -1;
            }
        }

        int nDeleted = 0;
        BSONObj order;
        auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
        if ( c.get() == 0 )
            c = theDataFileMgr.findAll(ns);
        JSMatcher matcher(pattern, c->indexKeyPattern());

        while ( c->ok() ) {
            Record *r = c->_current();
            DiskLoc rloc = c->currLoc();
            BSONObj js(r);

            bool deep;
            if ( !matcher.matches(js, &deep) ) {
                c->advance(); // advance must be after noMoreMatches() because it uses currKey()
            }
            else {
                c->advance(); // must advance before deleting as the next ptr will die
                assert( !deep || !c->getsetdup(rloc) ); // can't be a dup, we deleted it!
                if ( !justOne )
                    c->noteLocation();

                theDataFileMgr.deleteRecord(ns, r, rloc);
                nDeleted++;
                if ( justOne )
                    break;
                c->checkLocation();
            }
        }

        return nDeleted;
    }

    struct Mod {
        enum Op { INC, SET } op;
        const char *fieldName;
        double *ndouble;
        int *nint;
        void setn(double n) {
            if ( ndouble ) *ndouble = n;
            else *nint = (int) n;
        }
        double getn() {
            return ndouble ? *ndouble : *nint;
        }
        int type;
        static void getMods(vector<Mod>& mods, BSONObj from);
        static void applyMods(vector<Mod>& mods, BSONObj obj);
    };

    void Mod::applyMods(vector<Mod>& mods, BSONObj obj) {
        for ( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ ) {
            Mod& m = *i;
            BSONElement e = obj.findElement(m.fieldName);
            if ( e.isNumber() ) {
                if ( m.op == INC ) {
                    e.setNumber( e.number() + m.getn() );
                    m.setn( e.number() );
                    // *m.n = e.number() += *m.n;
                } else {
                    e.setNumber( m.getn() ); // $set or $SET
                }
            }
        }
    }

    /* get special operations like $inc
       { $inc: { a:1, b:1 } }
       { $set: { a:77 } }
       NOTE: MODIFIES source from object!
    */
    void Mod::getMods(vector<Mod>& mods, BSONObj from) {
        BSONObjIterator it(from);
        while ( it.more() ) {
            BSONElement e = it.next();
            const char *fn = e.fieldName();
            if ( *fn == '$' && e.type() == Object &&
                    fn[4] == 0 ) {
                BSONObj j = e.embeddedObject();
                BSONObjIterator jt(j);
                Op op = Mod::SET;
                if ( strcmp("$inc",fn) == 0 ) {
                    op = Mod::INC;
                    // we rename to $SET instead of $set so that on an op like
                    //   { $set: {x:1}, $inc: {y:1} }
                    // we don't get two "$set" fields which isn't allowed
                    strcpy((char *) fn, "$SET");
                }
                while ( jt.more() ) {
                    BSONElement f = jt.next();
                    if ( f.eoo() )
                        break;
                    Mod m;
                    m.op = op;
                    m.fieldName = f.fieldName();
                    if ( f.isNumber() ) {
                        if ( f.type() == NumberDouble ) {
                            m.ndouble = (double *) f.value();
                            m.nint = 0;
                        }
                        else {
                            m.ndouble = 0;
                            m.nint = (int *) f.value();
                        }
                        mods.push_back( m );
                    }
                }
            }
        }
    }

    /* todo:
         _ smart requery find record immediately
       returns:
         2: we did applyMods() but didn't logOp()
    	 5: we did applyMods() and did logOp() (so don't do it again)
         (clean these up later...)
    */
    int _updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss, bool logop=false) {
        //out() << "TEMP BAD";
        //lrutest.find(updateobj);

        int profile = database->profile;

        //	out() << "update ns:" << ns << " objsize:" << updateobj.objsize() << " queryobjsize:" <<
        //		pattern.objsize();

        if ( strstr(ns, ".system.") ) {
            out() << "\nERROR: attempt to update in system namespace " << ns << endl;
            ss << " can't update system namespace ";
            return 0;
        }

        int nscanned = 0;
        {
            BSONObj order;
            auto_ptr<Cursor> c = getIndexCursor(ns, pattern, order);
            if ( c.get() == 0 )
                c = theDataFileMgr.findAll(ns);
            JSMatcher matcher(pattern, c->indexKeyPattern());
            while ( c->ok() ) {
                Record *r = c->_current();
                nscanned++;
                BSONObj js(r);
                if ( !matcher.matches(js) ) {
                }
                else {
                    /* note: we only update one row and quit.  if you do multiple later,
                       be careful or multikeys in arrays could break things badly.  best
                       to only allow updating a single row with a multikey lookup.
                       */

                    if ( profile )
                        ss << " nscanned:" << nscanned;

                    /* look for $inc etc.  note as listed here, all fields to inc must be this type, you can't set some
                       regular ones at the moment. */
                    const char *firstField = updateobj.firstElement().fieldName();
                    if ( firstField[0] == '$' ) {
                        vector<Mod> mods;
                        Mod::getMods(mods, updateobj);
                        NamespaceDetailsTransient& ndt = NamespaceDetailsTransient::get(ns);
                        set<string>& idxKeys = ndt.indexKeys();
                        for ( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ ) {
                            if ( idxKeys.count(i->fieldName) ) {
                                uassert("can't $inc/$set an indexed field", false);
                            }
                        }

                        Mod::applyMods(mods, c->currLoc().obj());
                        if ( profile )
                            ss << " fastmod ";
                        if ( logop ) {
                            if ( mods.size() ) {
                                logOp("u", ns, updateobj, &pattern, &upsert);
                                return 5;
                            }
                        }
                        return 2;
                    }

                    theDataFileMgr.update(ns, r, c->currLoc(), updateobj.objdata(), updateobj.objsize(), ss);
                    return 1;
                }
                c->advance();
            }
        }

        if ( profile )
            ss << " nscanned:" << nscanned;

        if ( upsert ) {
            if ( updateobj.firstElement().fieldName()[0] == '$' ) {
                /* upsert of an $inc. build a default */
                vector<Mod> mods;
                Mod::getMods(mods, updateobj);
                BSONObjBuilder b;
                b.appendElements(pattern);
                for ( vector<Mod>::iterator i = mods.begin(); i != mods.end(); i++ )
                    b.append(i->fieldName, i->getn());
                BSONObj obj = b.done();
                theDataFileMgr.insert(ns, (void*) obj.objdata(), obj.objsize());
                if ( profile )
                    ss << " fastmodinsert ";
                return 3;
            }
            if ( profile )
                ss << " upsert ";
            theDataFileMgr.insert(ns, (void*) updateobj.objdata(), updateobj.objsize());
            return 4;
        }
        return 0;
    }
    /* todo: we can optimize replication by just doing insert when an upsert triggers.
    */
    void updateObjects(const char *ns, BSONObj updateobj, BSONObj pattern, bool upsert, stringstream& ss) {
        int rc = _updateObjects(ns, updateobj, pattern, upsert, ss, true);
        if ( rc != 5 )
            logOp("u", ns, updateobj, &pattern, &upsert);
    }

    int queryTraceLevel = 0;
    int otherTraceLevel = 0;

    int initialExtentSize(int len);

    bool _runCommands(const char *ns, BSONObj& jsobj, stringstream& ss, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl);

    bool runCommands(const char *ns, BSONObj& jsobj, stringstream& ss, BufBuilder &b, BSONObjBuilder& anObjBuilder, bool fromRepl) {
        try {
            return _runCommands(ns, jsobj, ss, b, anObjBuilder, fromRepl);
        }
        catch ( AssertionException e ) {
            if ( !e.msg.empty() )
                anObjBuilder.append("assertion", e.msg);
        }
        ss << " assertion ";
        anObjBuilder.append("errmsg", "db assertion failure");
        anObjBuilder.append("ok", 0.0);
        BSONObj x = anObjBuilder.done();
        b.append((void*) x.objdata(), x.objsize());
        return true;
    }

    int nCaught = 0;

    void killCursors(int n, long long *ids) {
        int k = 0;
        for ( int i = 0; i < n; i++ ) {
            if ( ClientCursor::erase(ids[i]) )
                k++;
        }
        log() << "killCursors: found " << k << " of " << n << '\n';
    }

    BSONObj id_obj = fromjson("{\"_id\":ObjectId( \"000000000000000000000000\" )}");
    BSONObj empty_obj = fromjson("{}");

    /* { count: "collectionname"[, query: <query>] }
       returns -1 on error.
    */
    int runCount(const char *ns, BSONObj& cmd, string& err) {
        NamespaceDetails *d = nsdetails(ns);
        if ( d == 0 ) {
            err = "ns does not exist";
            return -1;
        }

        BSONObj query = cmd.getObjectField("query");

        if ( query.isEmpty() ) {
            // count of all objects
            return (int) d->nrecords;
        }

        auto_ptr<Cursor> c;

        bool simpleKeyToMatch = false;
        c = getIndexCursor(ns, query, empty_obj, &simpleKeyToMatch);

        if ( c.get() ) {
            if ( simpleKeyToMatch ) {
                /* Here we only look at the btree keys to determine if a match, instead of looking
                   into the records, which would be much slower.
                   */
                int count = 0;
                BtreeCursor *bc = dynamic_cast<BtreeCursor *>(c.get());
                if ( c->ok() && !query.woCompare( bc->currKeyNode().key, BSONObj(), false ) ) {
                    BSONObj firstMatch = bc->currKeyNode().key;
                    count++;
                    while ( c->advance() ) {
                        if ( !firstMatch.woEqual( bc->currKeyNode().key ) )
                            break;
                        count++;
                    }
                }
                return count;
            }
        } else {
            c = findTableScan(ns, empty_obj);
        }

        int count = 0;
        auto_ptr<JSMatcher> matcher(new JSMatcher(query, c->indexKeyPattern()));
        while ( c->ok() ) {
            BSONObj js = c->current();
            bool deep;
            if ( !matcher->matches(js, &deep) ) {
            }
            else if ( !deep || !c->getsetdup(c->currLoc()) ) { // i.e., check for dups on deep items only
                // got a match.
                count++;
            }
            c->advance();
        }
        return count;
    }

    /* This is for languages whose "objects" are not well ordered (JSON is well ordered).
       [ { a : ... } , { b : ... } ] -> { a : ..., b : ... }
    */
    inline BSONObj transformOrderFromArrayFormat(BSONObj order) {
        /* note: this is slow, but that is ok as order will have very few pieces */
        BSONObjBuilder b;
        char p[2] = "0";

        while ( 1 ) {
            BSONObj j = order.getObjectField(p);
            if ( j.isEmpty() )
                break;
            BSONElement e = j.firstElement();
            uassert("bad order array", !e.eoo());
            uassert("bad order array [2]", e.isNumber());
            b.append(e);
            (*p)++;
            uassert("too many ordering elements", *p <= '9');
        }

        return b.doneAndDecouple();
    }

    QueryResult* runQuery(Message& message, const char *ns, int ntoskip, int _ntoreturn, BSONObj jsobj,
                          auto_ptr< set<string> > filter, stringstream& ss, int queryOptions)
    {
        Timer t;
        int nscanned = 0;
        bool wantMore = true;
        int ntoreturn = _ntoreturn;
        if ( _ntoreturn < 0 ) {
            ntoreturn = -_ntoreturn;
            wantMore = false;
        }
        ss << "query " << ns << " ntoreturn:" << ntoreturn;

        int n = 0;
        BufBuilder b(32768);
        BSONObjBuilder cmdResBuf;
        long long cursorid = 0;

        b.skip(sizeof(QueryResult));

        /* we assume you are using findOne() for running a cmd... */
        if ( ntoreturn == 1 && runCommands(ns, jsobj, ss, b, cmdResBuf, false) ) {
            n = 1;
        }
        else {

            uassert("not master", isMaster() || (queryOptions & Option_SlaveOk));

            string hint;
            bool explain = false;
            bool _gotquery = false;
            BSONObj query;// = jsobj.getObjectField("query");
            {
                BSONElement e = jsobj.findElement("query");
                if ( !e.eoo() && (e.type() == Object || e.type() == Array) ) {
                    query = e.embeddedObject();
                    _gotquery = true;
                }
            }
            BSONObj order;
            {
                BSONElement e = jsobj.findElement("orderby");
                if ( !e.eoo() ) {
                    order = e.embeddedObjectUserCheck();
                    if ( e.type() == Array )
                        order = transformOrderFromArrayFormat(order);
                }
            }
            if ( !_gotquery && order.isEmpty() )
                query = jsobj;
            else {
                explain = jsobj.getBoolField("$explain");
                hint = jsobj.getStringField("$hint");
            }

            /* The ElemIter will not be happy if this isn't really an object. So throw exception
               here when that is true.
                (Which may indicate bad data from appserver?)
            */
            if ( query.objsize() == 0 ) {
                out() << "Bad query object?\n  jsobj:";
                out() << jsobj.toString() << "\n  query:";
                out() << query.toString() << endl;
                uassert("bad query object", false);
            }

            bool isSorted = false;
            auto_ptr<Cursor> c = getSpecialCursor(ns);

            if ( c.get() == 0 )
                c = getIndexCursor(ns, query, order, 0, &isSorted, &hint);
            if ( c.get() == 0 )
                c = findTableScan(ns, order, &isSorted);

            auto_ptr<JSMatcher> matcher(new JSMatcher(query, c->indexKeyPattern()));
            JSMatcher &debug1 = *matcher;
            assert( debug1.getN() < 1000 );

            auto_ptr<ScanAndOrder> so;
            bool ordering = false;
            if ( !order.isEmpty() && !isSorted ) {
                ordering = true;
                ss << " scanAndOrder ";
                so = auto_ptr<ScanAndOrder>(new ScanAndOrder(ntoskip, ntoreturn,order));
                wantMore = false;
                //			scanAndOrder(b, c.get(), order, ntoreturn);
            }

            while ( c->ok() ) {
                BSONObj js = c->current();
                //if( queryTraceLevel >= 50 )
                //	out() << " checking against:\n " << js.toString() << endl;
                nscanned++;
                bool deep;
                if ( !matcher->matches(js, &deep) ) {
                }
                else if ( !deep || !c->getsetdup(c->currLoc()) ) { // i.e., check for dups on deep items only
                    // got a match.
                    assert( js.objsize() >= 0 ); //defensive for segfaults
                    if ( ordering ) {
                        // note: no cursors for non-indexed, ordered results.  results must be fairly small.
                        so->add(js);
                    }
                    else if ( ntoskip > 0 ) {
                        ntoskip--;
                    } else {
                        if ( explain ) {
                            n++;
                            if ( n >= ntoreturn && !wantMore )
                                break; // .limit() was used, show just that much.
                        }
                        else {
                            bool ok = fillQueryResultFromObj(b, filter.get(), js);
                            if ( ok ) n++;
                            if ( ok ) {
                                if ( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
                                        (ntoreturn==0 && (b.len()>1*1024*1024 || n>=101)) ) {
                                    /* if ntoreturn is zero, we return up to 101 objects.  on the subsequent getmore, there
                                    is only a size limit.  The idea is that on a find() where one doesn't use much results,
                                    we don't return much, but once getmore kicks in, we start pushing significant quantities.

                                    The n limit (vs. size) is important when someone fetches only one small field from big
                                    objects, which causes massive scanning server-side.
                                    */
                                    /* if only 1 requested, no cursor saved for efficiency...we assume it is findOne() */
                                    if ( wantMore && ntoreturn != 1 ) {
                                        if ( useCursors ) {
                                            c->advance();
                                            if ( c->ok() ) {
                                                // more...so save a cursor
                                                ClientCursor *cc = new ClientCursor();
                                                cc->c = c;
                                                cursorid = cc->cursorid;
                                                DEV out() << "  query has more, cursorid: " << cursorid << endl;
                                                //cc->pattern = query;
                                                cc->matcher = matcher;
                                                cc->ns = ns;
                                                cc->pos = n;
                                                cc->filter = filter;
                                                cc->originalMessage = message;
                                                cc->updateLocation();
                                            }
                                        }
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
                c->advance();
            } // end while

            if ( explain ) {
                BSONObjBuilder builder;
                builder.append("cursor", c->toString());
                builder.append("nscanned", nscanned);
                builder.append("n", ordering ? so->size() : n);
                if ( ordering )
                    builder.append("scanAndOrder", true);
                builder.append("millis", t.millis());
                BSONObj obj = builder.done();
                fillQueryResultFromObj(b, 0, obj);
                n = 1;
            } else if ( ordering ) {
                so->fill(b, filter.get(), n);
            }
            else if ( cursorid == 0 && (queryOptions & Option_CursorTailable) && c->tailable() ) {
                c->setAtTail();
                ClientCursor *cc = new ClientCursor();
                cc->c = c;
                cursorid = cc->cursorid;
                DEV out() << "  query has no more but tailable, cursorid: " << cursorid << endl;
                //cc->pattern = query;
                cc->matcher = matcher;
                cc->ns = ns;
                cc->pos = n;
                cc->filter = filter;
                cc->originalMessage = message;
                cc->updateLocation();
            }
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->resultFlags() = 0;
        qr->len = b.len();
        ss << " reslen:" << b.len();
        //	qr->channel = 0;
        qr->setOperation(opReply);
        qr->cursorId = cursorid;
        qr->startingFrom = 0;
        qr->nReturned = n;
        b.decouple();

        int duration = t.millis();
        if ( (database && database->profile) || duration >= 100 ) {
            ss << " nscanned:" << nscanned << ' ';
            if ( ntoskip )
                ss << " ntoskip:" << ntoskip;
            if ( database && database->profile )
                ss << " <br>query: ";
            ss << jsobj.toString() << ' ';
        }
        ss << " nreturned:" << n;
        return qr;
    }

//int dump = 0;

    /* empty result for error conditions */
    QueryResult* emptyMoreResult(long long cursorid) {
        BufBuilder b(32768);
        b.skip(sizeof(QueryResult));
        QueryResult *qr = (QueryResult *) b.buf();
        qr->cursorId = 0; // 0 indicates no more data to retrieve.
        qr->startingFrom = 0;
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->nReturned = 0;
        b.decouple();
        return qr;
    }

    QueryResult* getMore(const char *ns, int ntoreturn, long long cursorid) {
        BufBuilder b(32768);

        ClientCursor *cc = ClientCursor::find(cursorid);

        b.skip(sizeof(QueryResult));

        int resultFlags = 0;
        int start = 0;
        int n = 0;

        if ( !cc ) {
            DEV log() << "getMore: cursorid not found " << ns << " " << cursorid << endl;
            cursorid = 0;
            resultFlags = QueryResult::ResultFlag_CursorNotFound;
        }
        else {
            start = cc->pos;
            Cursor *c = cc->c.get();
            c->checkLocation();
            c->tailResume();
            while ( 1 ) {
                if ( !c->ok() ) {
                    if ( c->tailing() ) {
                        c->setAtTail();
                        break;
                    }
                    DEV log() << "  getmore: last batch, erasing cursor " << cursorid << endl;
                    bool ok = ClientCursor::erase(cursorid);
                    assert(ok);
                    cursorid = 0;
                    cc = 0;
                    break;
                }
                BSONObj js = c->current();

                bool deep;
                if ( !cc->matcher->matches(js, &deep) ) {
                }
                else {
                    //out() << "matches " << c->currLoc().toString() << ' ' << deep << '\n';
                    if ( deep && c->getsetdup(c->currLoc()) ) {
                        //out() << "  but it's a dup \n";
                    }
                    else {
                        bool ok = fillQueryResultFromObj(b, cc->filter.get(), js);
                        if ( ok ) {
                            n++;
                            if ( (ntoreturn>0 && (n >= ntoreturn || b.len() > MaxBytesToReturnToClientAtOnce)) ||
                                    (ntoreturn==0 && b.len()>1*1024*1024) ) {
                                c->advance();
                                if ( c->tailing() && !c->ok() )
                                    c->setAtTail();
                                cc->pos += n;
                                //cc->updateLocation();
                                break;
                            }
                        }
                    }
                }
                c->advance();
            }
            if ( cc )
                cc->updateLocation();
        }

        QueryResult *qr = (QueryResult *) b.buf();
        qr->len = b.len();
        qr->setOperation(opReply);
        qr->resultFlags() = resultFlags;
        qr->cursorId = cursorid;
        qr->startingFrom = start;
        qr->nReturned = n;
        b.decouple();

        return qr;
    }

} // namespace mongo
