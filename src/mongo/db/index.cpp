/** @file index.cpp */

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

#include "mongo/pch.h"

#include "mongo/db/index.h"

#include <boost/checked_delete.hpp>

#include "mongo/db/auth/action_type.h"
#include "mongo/db/background.h"
#include "mongo/db/btree.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/index_names.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_cursor.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_update.h"
#include "mongo/db/ops/delete.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/repl/rs.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    // What's the default version of our indices?
    const int DefaultIndexVersionNumber = 1;

    int removeFromSysIndexes(const char *ns, const char *idxName) {
        string system_indexes = cc().database()->name() + ".system.indexes";
        BSONObjBuilder b;
        b.append("ns", ns);
        b.append("name", idxName); // e.g.: { name: "ts_1", ns: "foo.coll" }
        BSONObj cond = b.done();
        return (int) deleteObjects(system_indexes.c_str(), cond, false, false, true);
    }

    /* this is just an attempt to clean up old orphaned stuff on a delete all indexes
       call. repair database is the clean solution, but this gives one a lighter weight
       partial option.  see dropIndexes()
    */
    int assureSysIndexesEmptied(const char *ns, IndexDetails *idIndex) {
        string system_indexes = cc().database()->name() + ".system.indexes";
        BSONObjBuilder b;
        b.append("ns", ns);
        if( idIndex ) {
            b.append("name", BSON( "$ne" << idIndex->indexName().c_str() ));
        }
        BSONObj cond = b.done();
        int n = (int) deleteObjects(system_indexes.c_str(), cond, false, false, true);
        if( n ) {
            log() << "info: assureSysIndexesEmptied cleaned up " << n << " entries" << endl;
        }
        return n;
    }

    int IndexDetails::keyPatternOffset( const string& key ) const {
        BSONObjIterator i( keyPattern() );
        int n = 0;
        while ( i.more() ) {
            BSONElement e = i.next();
            if ( key == e.fieldName() )
                return n;
            n++;
        }
        return -1;
    }

    /* delete this index.  does NOT clean up the system catalog
       (system.indexes or system.namespaces) -- only NamespaceIndex.
       TOOD: above comment is wrong, also, document durability assumptions
    */
    void IndexDetails::kill_idx() {
        string ns = indexNamespace(); // e.g. foo.coll.$ts_1
        try {

            string pns = parentNS(); // note we need a copy, as parentNS() won't work after the drop() below

            // clean up parent namespace index cache
            NamespaceDetailsTransient::get( pns.c_str() ).deletedIndex();

            string name = indexName();

            /* important to catch exception here so we can finish cleanup below. */
            try {
                dropNS(ns.c_str());
            }
            catch(DBException& ) {
                LOG(2) << "IndexDetails::kill(): couldn't drop ns " << ns << endl;
            }
            head.setInvalid();
            info.setInvalid();

            // clean up in system.indexes.  we do this last on purpose.
            int n = removeFromSysIndexes(pns.c_str(), name.c_str());
            wassert( n == 1 );

        }
        catch ( DBException &e ) {
            log() << "exception in kill_idx: " << e << ", ns: " << ns << endl;
        }
    }

    // should be { <something> : <simpletype[1|-1]>, .keyp.. }
    static bool validKeyPattern(BSONObj kp) {
        BSONObjIterator i(kp);
        while( i.moreWithEOO() ) {
            BSONElement e = i.next();
            if( e.type() == Object || e.type() == Array )
                return false;
        }
        return true;
    }

    static bool needToUpgradeMinorVersion(const string& newPluginName) {
        if (IndexNames::existedBefore24(newPluginName))
            return false;

        DataFileHeader* dfh = cc().database()->getFile(0)->getHeader();
        if (dfh->versionMinor == PDFILE_VERSION_MINOR_24_AND_NEWER)
            return false; // these checks have already been done

        fassert(16737, dfh->versionMinor == PDFILE_VERSION_MINOR_22_AND_OLDER);

        return true;
    }

    static void upgradeMinorVersionOrAssert(const string& newPluginName) {
        const string systemIndexes = cc().database()->name() + ".system.indexes";
        auto_ptr<Runner> runner(InternalPlanner::collectionScan(systemIndexes));
        BSONObj index;
        Runner::RunnerState state;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&index, NULL))) {
            const BSONObj key = index.getObjectField("key");
            const string plugin = IndexNames::findPluginName(key);
            if (IndexNames::existedBefore24(plugin))
                continue;

            const string errmsg = str::stream()
                << "Found pre-existing index " << index << " with invalid type '" << plugin << "'. "
                << "Disallowing creation of new index type '" << newPluginName << "'. See "
                << "http://dochub.mongodb.org/core/index-type-changes"
                ;

            error() << errmsg << endl;
            uasserted(16738, errmsg);
        }

        if (Runner::RUNNER_EOF != state) {
            warning() << "Internal error while reading collection " << systemIndexes << endl;
        }

        DataFileHeader* dfh = cc().database()->getFile(0)->getHeader();
        getDur().writingInt(dfh->versionMinor) = PDFILE_VERSION_MINOR_24_AND_NEWER;
    }

    /**
     * @param newSpec the new index specification to check.
     * @param existingDetails the currently existing index details to compare with.
     *
     * @return true if the given newSpec has the same options as the
     *     existing index assuming the key spec matches.
     */
    bool areIndexOptionsEquivalent(const BSONObj& newSpec,
                                   const IndexDetails& existingDetails) {
        if (existingDetails.dropDups() != newSpec["dropDups"].trueValue()) {
            return false;
        }

        const BSONElement sparseSpecs =
                existingDetails.info.obj().getField("sparse");

        if (sparseSpecs.trueValue() != newSpec["sparse"].trueValue()) {
            return false;
        }

        // Note: { _id: 1 } or { _id: -1 } implies unique: true.
        if (!existingDetails.isIdIndex() &&
                existingDetails.unique() != newSpec["unique"].trueValue()) {
            return false;
        }

        const BSONElement existingExpireSecs =
                existingDetails.info.obj().getField("expireAfterSeconds");
        const BSONElement newExpireSecs = newSpec["expireAfterSeconds"];

        return existingExpireSecs == newExpireSecs;
    }

    bool prepareToBuildIndex(const BSONObj& io,
                             bool mayInterrupt,
                             bool god,
                             string& sourceNS,
                             NamespaceDetails*& sourceCollection,
                             BSONObj& fixedIndexObject) {
        sourceCollection = 0;

        // the collection for which we are building an index
        sourceNS = io.getStringField("ns");
        NamespaceString nss(sourceNS);
        uassert(10096, "invalid ns to index", sourceNS.find( '.' ) != string::npos);
        uassert(17072, "cannot create indexes on the system.indexes collection",
                !nss.isSystemDotIndexes());
        massert(10097, str::stream() << "bad table to index name on add index attempt current db: "
                << cc().database()->name() << "  source: " << sourceNS,
                cc().database()->name() == nss.db());

        // logical name of the index.  todo: get rid of the name, we don't need it!
        const char *name = io.getStringField("name");
        uassert(12523, "no index name specified", *name);
        string indexNamespace = IndexDetails::indexNamespaceFromObj(io);
        uassert(16829, str::stream() << "namespace name generated from index name \"" <<
                                     indexNamespace << "\" is too long (128 char max)",
                indexNamespace.length() <= 128);

        BSONObj key = io.getObjectField("key");
        uassert(12524, "index key pattern too large", key.objsize() <= 2048);
        if( !validKeyPattern(key) ) {
            string s = string("bad index key pattern ") + key.toString();
            uasserted(10098 , s.c_str());
        }

        if ( sourceNS.empty() || key.isEmpty() ) {
            LOG(2) << "bad add index attempt name:" << (name?name:"") << "\n  ns:" <<
                   sourceNS << "\n  idxobj:" << io.toString() << endl;
            string s = "bad add index attempt " + sourceNS + " key:" + key.toString();
            uasserted(12504, s);
        }

        sourceCollection = nsdetails(sourceNS);
        if( sourceCollection == 0 ) {
            // try to create it
            string err;
            if ( !userCreateNS(sourceNS.c_str(), BSONObj(), err, false) ) {
                problem() << "ERROR: failed to create collection while adding its index. " << sourceNS << endl;
                return false;
            }
            sourceCollection = nsdetails(sourceNS);
            MONGO_TLOG(0) << "info: creating collection " << sourceNS << " on add index" << endl;
            verify( sourceCollection );
        }

        {
            // Check both existing and in-progress indexes (2nd param = true)
            const int idx = sourceCollection->findIndexByName(name, true);
            if (idx >= 0) {
                // index already exists.
                const IndexDetails& indexSpec(sourceCollection->idx(idx));
                BSONObj existingKeyPattern(indexSpec.keyPattern());
                uassert(16850, str::stream() << "Trying to create an index "
                        << "with same name " << name
                        << " with different key spec " << key
                        << " vs existing spec " << existingKeyPattern,
                        existingKeyPattern.equal(key));

                uassert(16851, str::stream() << "Index with name: " << name
                        << " already exists with different options",
                        areIndexOptionsEquivalent(io, indexSpec));

                // Index already exists with the same options, so no need to build a new
                // one (not an error). Most likely requested by a client using ensureIndex.
                return false;
            }
        }

        {
            // Check both existing and in-progress indexes (2nd param = true)
            const int idx = sourceCollection->findIndexByKeyPattern(key, true);
            if (idx >= 0) {
                LOG(2) << "index already exists with diff name " << name
                        << ' ' << key.toString() << endl;

                const IndexDetails& indexSpec(sourceCollection->idx(idx));
                uassert(16852, str::stream() << "Index with pattern: " << key
                        << " already exists with different options",
                        areIndexOptionsEquivalent(io, indexSpec));

                return false;
            }
        }

        if ( sourceCollection->getTotalIndexCount() >= NamespaceDetails::NIndexesMax ) {
            stringstream ss;
            ss << "add index fails, too many indexes for " << sourceNS << " key:" << key.toString();
            string s = ss.str();
            log() << s << endl;
            uasserted(12505,s);
        }

        /* this is because we want key patterns like { _id : 1 } and { _id : <someobjid> } to
           all be treated as the same pattern.
        */
        if ( IndexDetails::isIdIndexPattern(key) ) {
            if( !god ) {
                ensureHaveIdIndex( sourceNS.c_str(), mayInterrupt );
                return false;
            }
        }
        else {
            /* is buildIndexes:false set for this replica set member?
               if so we don't build any indexes except _id
            */
            if( theReplSet && !theReplSet->buildIndexes() )
                return false;
        }

        string pluginName = IndexNames::findPluginName( key );
        if (pluginName.size()) {
            uassert(16734, str::stream() << "Unknown index plugin '" << pluginName << "' "
                                         << "in index "<< key,
                    IndexNames::isKnownName(pluginName));

            if (needToUpgradeMinorVersion(pluginName))
                upgradeMinorVersionOrAssert(pluginName);
        }

        { 
            BSONObj o = io;
            o = IndexLegacy::adjustIndexSpecObject(o);
            BSONObjBuilder b;
            int v = DefaultIndexVersionNumber;
            if( !o["v"].eoo() ) {
                double vv = o["v"].Number();
                // note (one day) we may be able to fresh build less versions than we can use
                // isASupportedIndexVersionNumber() is what we can use
                uassert(14803, str::stream() << "this version of mongod cannot build new indexes of version number " << vv, 
                    vv == 0 || vv == 1);
                v = (int) vv;
            }
            // idea is to put things we use a lot earlier
            b.append("v", v);
            b.append(o["key"]);
            if( o["unique"].trueValue() )
                b.appendBool("unique", true); // normalize to bool true in case was int 1 or something...
            b.append(o["ns"]);

            {
                // stripping _id
                BSONObjIterator i(o);
                while ( i.more() ) {
                    BSONElement e = i.next();
                    string s = e.fieldName();
                    if( s != "_id" && s != "v" && s != "ns" && s != "unique" && s != "key" )
                        b.append(e);
                }
            }
        
            fixedIndexObject = b.obj();
        }

        return true;
    }
}
