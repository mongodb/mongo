// dbcommands_admin.cpp

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

/**
   this file has dbcommands that are for dba type administration
   mostly around dbs and collections
   NOT system stuff
*/


#include "mongo/pch.h"

#include <boost/filesystem/convenience.hpp>
#include <boost/filesystem/operations.hpp>
#include <string>
#include <vector>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/cmdline.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop-inl.h"
#include "mongo/db/index/catalog_hack.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/kill_current_op.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/scripting/engine.h"
#include "mongo/util/alignedbuilder.h"
#include "mongo/util/background.h"
#include "mongo/util/logfile.h"
#include "mongo/util/paths.h"
#include "mongo/util/timer.h"

namespace mongo {

    namespace dur {
        boost::filesystem::path getJournalDir();
    }

    // Testing-only, enabled via command line
    class JournalLatencyTestCmd : public Command {
    public:
        JournalLatencyTestCmd() : Command( "journalLatencyTest" ) {}

        virtual bool slaveOk() const { return true; }
        virtual LockType locktype() const { return NONE; }
        virtual bool adminOnly() const { return true; }
        virtual void help(stringstream& h) const { h << "test how long to write and fsync to a test file in the journal/ directory"; }
        // No auth needed because it only works when enabled via command line.
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {}
        bool run(const string& dbname, BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            boost::filesystem::path p = dur::getJournalDir();
            p /= "journalLatencyTest";
        
            // remove file if already present
            try { 
                boost::filesystem::remove(p);
            }
            catch(...) { }

            BSONObjBuilder bb[2];
            for( int pass = 0; pass < 2; pass++ ) {
                LogFile f(p.string());
                AlignedBuilder b(1024 * 1024);
                {
                    Timer t;
                    for( int i = 0 ; i < 100; i++ ) { 
                        f.synchronousAppend(b.buf(), 8192);
                    }
                    bb[pass].append("8KB", t.millis() / 100.0);
                }
                {
                    const int N = 50;
                    Timer t2;
                    long long x = 0;
                    for( int i = 0 ; i < N; i++ ) { 
                        Timer t;
                        f.synchronousAppend(b.buf(), 8192);
                        x += t.micros();
                        sleepmillis(4);
                    }
                    long long y = t2.micros() - 4*N*1000;
                    // not really trusting the timer granularity on all platforms so whichever is higher of x and y
                    bb[pass].append("8KBWithPauses", max(x,y) / (N*1000.0));
                }
                {
                    Timer t;
                    for( int i = 0 ; i < 20; i++ ) { 
                        f.synchronousAppend(b.buf(), 1024 * 1024);
                    }
                    bb[pass].append("1MB", t.millis() / 20.0);
                }
                // second time around, we are prealloced.
            }
            result.append("timeMillis", bb[0].obj());
            result.append("timeMillisWithPrealloc", bb[1].obj());

            try { 
                remove(p);
            }
            catch(...) { }

            try {
                result.append("onSamePartition", onSamePartition(dur::getJournalDir().string(), dbpath));
            }
            catch(...) { }

            return 1;
        }
    };
    MONGO_INITIALIZER(RegisterJournalLatencyTestCmd)(InitializerContext* context) {
        if (Command::testCommandsEnabled) {
            // Leaked intentionally: a Command registers itself when constructed.
            new JournalLatencyTestCmd();
        }
        return Status::OK();
    }

    class ValidateCmd : public Command {
    public:
        ValidateCmd() : Command( "validate" ) {}

        virtual bool slaveOk() const {
            return true;
        }

        virtual void help(stringstream& h) const { h << "Validate contents of a namespace by scanning its data structures for correctness.  Slow.\n"
                                                        "Add full:true option to do a more thorough check"; }

        virtual LockType locktype() const { return READ; }
        virtual void addRequiredPrivileges(const std::string& dbname,
                                           const BSONObj& cmdObj,
                                           std::vector<Privilege>* out) {
            ActionSet actions;
            actions.addAction(ActionType::validate);
            out->push_back(Privilege(parseNs(dbname, cmdObj), actions));
        }
        //{ validate: "collectionnamewithoutthedbpart" [, scandata: <bool>] [, full: <bool> } */

        bool run(const string& dbname , BSONObj& cmdObj, int, string& errmsg, BSONObjBuilder& result, bool fromRepl ) {
            string ns = dbname + "." + cmdObj.firstElement().valuestrsafe();
            NamespaceDetails * d = nsdetails( ns );
            if ( !cmdLine.quiet ) {
                MONGO_TLOG(0) << "CMD: validate " << ns << endl;
            }

            if ( ! d ) {
                errmsg = "ns not found";
                return false;
            }

            result.append( "ns", ns );
            validateNS( ns.c_str() , d, cmdObj, result);
            return true;
        }

    private:
        void validateNS(const char *ns,
                        NamespaceDetails *d,
                        const BSONObj& cmdObj,
                        BSONObjBuilder& result) {
            const bool full = cmdObj["full"].trueValue();
            const bool scanData = full || cmdObj["scandata"].trueValue();

            bool valid = true;
            BSONArrayBuilder errors; // explanation(s) for why valid = false
            if ( d->isCapped() ){
                result.append("capped", d->isCapped());
                result.appendNumber("max", d->maxCappedDocs());
            }

            result.append( "firstExtent", str::stream() << d->firstExtent().toString()
                           << " ns:" << d->firstExtent().ext()->nsDiagnostic.toString());
            result.append( "lastExtent", str::stream() <<  d->lastExtent().toString()
                           << " ns:" <<  d->lastExtent().ext()->nsDiagnostic.toString());

            BSONArrayBuilder extentData;
            int extentCount = 0;
            try {
                d->firstExtent().ext()->assertOk();
                d->lastExtent().ext()->assertOk();

                DiskLoc extentDiskLoc = d->firstExtent();
                while (!extentDiskLoc.isNull()) {
                    Extent* thisExtent = extentDiskLoc.ext();
                    if (full) {
                        extentData << thisExtent->dump();
                    }
                    if (!thisExtent->validates(extentDiskLoc, &errors)) {
                        valid = false;
                    }
                    DiskLoc nextDiskLoc = thisExtent->xnext;
                    if (extentCount > 0 && !nextDiskLoc.isNull()
                                        &&  nextDiskLoc.ext()->xprev != extentDiskLoc) {
                        StringBuilder sb;
                        sb << "'xprev' pointer " << nextDiskLoc.ext()->xprev.toString()
                           << " in extent " << nextDiskLoc.toString()
                           << " does not point to extent " << extentDiskLoc.toString();
                        errors << sb.str();
                        valid = false;
                    }
                    if (nextDiskLoc.isNull() && extentDiskLoc != d->lastExtent()) {
                        StringBuilder sb;
                        sb << "'lastExtent' pointer " << d->lastExtent().toString()
                           << " does not point to last extent in list " << extentDiskLoc.toString();
                        errors << sb.str();
                        valid = false;
                    }
                    extentDiskLoc = nextDiskLoc;
                    extentCount++;
                    killCurrentOp.checkForInterrupt();
                }
            }
            catch (const DBException& e) {
                StringBuilder sb;
                sb << "exception validating extent " << extentCount
                   << ": " << e.what();
                errors << sb.str();
                valid = false;
            }
            result.append("extentCount", extentCount);

            if ( full )
                result.appendArray( "extents" , extentData.arr() );

            result.appendNumber("datasize", d->dataSize());
            result.appendNumber("nrecords", d->numRecords());
            result.appendNumber("lastExtentSize", d->lastExtentSize());
            result.appendNumber("padding", d->paddingFactor());

            try {

                bool testingLastExtent = false;
                try {
                    if (d->firstExtent().isNull()) {
                        errors << "'firstExtent' pointer is null";
                        valid=false;
                    }
                    else {
                        result.append("firstExtentDetails", d->firstExtent().ext()->dump());
                        if (!d->firstExtent().ext()->xprev.isNull()) {
                            StringBuilder sb;
                            sb << "'xprev' pointer in 'firstExtent' " << d->firstExtent().toString()
                               << " is " << d->firstExtent().ext()->xprev.toString()
                               << ", should be null";
                            errors << sb.str();
                            valid=false;
                        }
                    }
                    testingLastExtent = true;
                    if (d->lastExtent().isNull()) {
                        errors << "'lastExtent' pointer is null";
                        valid=false;
                    }
                    else {
                        if (d->firstExtent() != d->lastExtent()) {
                            result.append("lastExtentDetails", d->lastExtent().ext()->dump());
                            if (!d->lastExtent().ext()->xnext.isNull()) {
                                StringBuilder sb;
                                sb << "'xnext' pointer in 'lastExtent' " << d->lastExtent().toString()
                                   << " is " << d->lastExtent().ext()->xnext.toString()
                                   << ", should be null";
                                errors << sb.str();
                                valid = false;
                            }
                        }
                    }
                }
                catch (const DBException& e) {
                    StringBuilder sb;
                    sb << "exception processing '"
                       << (testingLastExtent ? "lastExtent" : "firstExtent")
                       << "': " << e.what();
                    errors << sb.str();
                    valid = false;
                }

                set<DiskLoc> recs;
                if( scanData ) {
                    int n = 0;
                    int nInvalid = 0;
                    long long nQuantizedSize = 0;
                    long long nPowerOf2QuantizedSize = 0;
                    long long len = 0;
                    long long nlen = 0;
                    long long bsonLen = 0;
                    int outOfOrder = 0;
                    DiskLoc cl_last;

                    DiskLoc cl;
                    Runner::RunnerState state;
                    auto_ptr<Runner> runner(InternalPlanner::collectionScan(ns));
                    while (Runner::RUNNER_ADVANCED == (state = runner->getNext(NULL, &cl))) {
                        n++;

                        if ( n < 1000000 )
                            recs.insert(cl);
                        if ( d->isCapped() ) {
                            if ( cl < cl_last )
                                outOfOrder++;
                            cl_last = cl;
                        }

                        Record *r = cl.rec();
                        len += r->lengthWithHeaders();
                        nlen += r->netLength();
                        
                        if ( r->lengthWithHeaders() ==
                                NamespaceDetails::quantizeAllocationSpace
                                    ( r->lengthWithHeaders() ) ) {
                            // Count the number of records having a size consistent with
                            // the quantizeAllocationSpace quantization implementation.
                            ++nQuantizedSize;
                        }

                        if ( r->lengthWithHeaders() ==
                                NamespaceDetails::quantizePowerOf2AllocationSpace
                                    ( r->lengthWithHeaders() - 1 ) ) {
                            // Count the number of records having a size consistent with the
                            // quantizePowerOf2AllocationSpace quantization implementation.
                            // Because of SERVER-8311, power of 2 quantization is not idempotent and
                            // r->lengthWithHeaders() - 1 must be checked instead of the record
                            // length itself.
                            ++nPowerOf2QuantizedSize;
                        }

                        if (full){
                            BSONObj obj = BSONObj::make(r);
                            if (!obj.isValid() || !obj.valid()){ // both fast and deep checks
                                valid = false;
                                if (nInvalid == 0) // only log once;
                                    errors << "invalid bson object detected (see logs for more info)";

                                nInvalid++;
                                if (strcmp("_id", obj.firstElementFieldName()) == 0){
                                    try {
                                        obj.firstElement().validate(); // throws on error
                                        log() << "Invalid bson detected in " << ns << " with _id: " << obj.firstElement().toString(false) << endl;
                                    }
                                    catch(...){
                                        log() << "Invalid bson detected in " << ns << " with corrupt _id" << endl;
                                    }
                                }
                                else {
                                    log() << "Invalid bson detected in " << ns << " and couldn't find _id" << endl;
                                }
                            }
                            else {
                                bsonLen += obj.objsize();
                            }
                        }
                    }
                    if (Runner::RUNNER_EOF != state) {
                        // TODO: more descriptive logging.
                        warning() << "Internal error while reading collection " << ns << endl;
                    }
                    if ( d->isCapped() && !d->capLooped() ) {
                        result.append("cappedOutOfOrder", outOfOrder);
                        if ( outOfOrder > 1 ) {
                            valid = false;
                            errors << "too many out of order records";
                        }
                    }
                    result.append("objectsFound", n);

                    if (full) {
                        result.append("invalidObjects", nInvalid);
                    }

                    result.appendNumber("nQuantizedSize", nQuantizedSize);
                    result.appendNumber("nPowerOf2QuantizedSize", nPowerOf2QuantizedSize);
                    result.appendNumber("bytesWithHeaders", len);
                    result.appendNumber("bytesWithoutHeaders", nlen);

                    if (full) {
                        result.appendNumber("bytesBson", bsonLen);
                    }
                }

                BSONArrayBuilder deletedListArray;
                for ( int i = 0; i < Buckets; i++ ) {
                    deletedListArray << d->deletedListEntry(i).isNull();
                }

                int ndel = 0;
                long long delSize = 0;
                BSONArrayBuilder delBucketSizes;
                int incorrect = 0;
                for ( int i = 0; i < Buckets; i++ ) {
                    DiskLoc loc = d->deletedListEntry(i);
                    try {
                        int k = 0;
                        while ( !loc.isNull() ) {
                            if ( recs.count(loc) )
                                incorrect++;
                            ndel++;

                            if ( loc.questionable() ) {
                                if( d->isCapped() && !loc.isValid() && i == 1 ) {
                                    /* the constructor for NamespaceDetails intentionally sets deletedList[1] to invalid
                                       see comments in namespace.h
                                    */
                                    break;
                                }

                                string err( str::stream() << "bad pointer in deleted record list: "
                                                          << loc.toString()
                                                          << " bucket: " << i
                                                          << " k: " << k );
                                errors << err;
                                valid = false;
                                break;
                            }

                            DeletedRecord *d = loc.drec();
                            delSize += d->lengthWithHeaders();
                            loc = d->nextDeleted();
                            k++;
                            killCurrentOp.checkForInterrupt();
                        }
                        delBucketSizes << k;
                    }
                    catch (...) {
                        errors << ("exception in deleted chain for bucket " + BSONObjBuilder::numStr(i));
                        valid = false;
                    }
                }
                result.appendNumber("deletedCount", ndel);
                result.appendNumber("deletedSize", delSize);
                if ( full ) {
                    result << "delBucketSizes" << delBucketSizes.arr();
                }

                if ( incorrect ) {
                    errors << (BSONObjBuilder::numStr(incorrect) + " records from datafile are in deleted list");
                    valid = false;
                }

                int idxn = 0;
                try  {
                    result.append("nIndexes", d->getCompletedIndexCount());
                    BSONObjBuilder indexes; // not using subObjStart to be exception safe
                    NamespaceDetails::IndexIterator i = d->ii();
                    while( i.more() ) {
                        IndexDetails& id = i.next();
                        log() << "validating index " << idxn << ": " << id.indexNamespace() << endl;
                        auto_ptr<IndexDescriptor> descriptor(CatalogHack::getDescriptor(d, idxn));
                        auto_ptr<IndexAccessMethod> iam(CatalogHack::getIndex(descriptor.get()));
                        int64_t keys;
                        iam->validate(&keys);
                        indexes.appendNumber(id.indexNamespace(), static_cast<long long>(keys));
                        idxn++;
                    }
                    result.append("keysPerIndex", indexes.done());
                }
                catch (...) {
                    errors << ("exception during index validate idxn " + BSONObjBuilder::numStr(idxn));
                    valid=false;
                }

            }
            catch (AssertionException) {
                errors << "exception during validate";
                valid = false;
            }

            result.appendBool("valid", valid);
            result.append("errors", errors.arr());

            if ( !full ){
                result.append("warning", "Some checks omitted for speed. use {full:true} option to do more thorough scan.");
            }
            
            if ( !valid ) {
                result.append("advice", "ns corrupt, requires repair");
            }

        }
    } validateCmd;

}
