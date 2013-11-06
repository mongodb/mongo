//@file update.cpp

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

#include "mongo/db/ops/update.h"

#include <cstring>  // for memcpy

#include "mongo/bson/mutable/damage_vector.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/client/dbclientinterface.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/index_set.h"
#include "mongo/db/namespace_details.h"
#include "mongo/db/ops/update_driver.h"
#include "mongo/db/pagefault.h"
#include "mongo/db/pdfile.h"
#include "mongo/db/query/new_find.h"
#include "mongo/db/query/query_planner_common.h"
#include "mongo/db/query/runner_yield_policy.h"
#include "mongo/db/query_optimizer.h"
#include "mongo/db/query_runner.h"
#include "mongo/db/queryutil.h"
#include "mongo/db/storage/record.h"
#include "mongo/db/structure/collection.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/platform/unordered_set.h"

namespace mongo {

    namespace {

        // TODO: Make this a function on NamespaceString, or make it cleaner.
        inline void validateUpdate( const char* ns , const BSONObj& updateobj, const BSONObj& patternOrig ) {
            uassert( 10155 , "cannot update reserved $ collection", strchr(ns, '$') == 0 );
            if ( strstr(ns, ".system.") ) {
                /* dm: it's very important that system.indexes is never updated as IndexDetails
                   has pointers into it */
                uassert( 10156,
                         str::stream() << "cannot update system collection: "
                         << ns << " q: " << patternOrig << " u: " << updateobj,
                         legalClientSystemNS( ns , true ) );
            }
        }

        /**
         * return a BSONObj with the _id field of the doc passed in. If no _id and multi, error.
         */
        BSONObj makeOplogEntryQuery(const BSONObj doc, bool multi) {
            BSONObjBuilder idPattern;
            BSONElement id;
            // NOTE: If the matching object lacks an id, we'll log
            // with the original pattern.  This isn't replay-safe.
            // It might make sense to suppress the log instead
            // if there's no id.
            if ( doc.getObjectID( id ) ) {
                idPattern.append( id );
                return idPattern.obj();
            }
            else {
                uassert( 10157, "multi-update requires all modified objects to have an _id" , ! multi );
                return doc;
            }
        }

    } // namespace

    UpdateResult update(const UpdateRequest& request, OpDebug* opDebug) {

        // Should the modifiers validate their embedded docs via okForStorage
        // Only user updates should be checked. Any system or replication stuff should pass through.
        // Config db docs shouldn't get checked for valid field names since the shard key can have
        // a dot (".") in it.
        bool shouldValidate = !(request.isFromReplication() ||
                                request.getNamespaceString().isConfigDB());

        // TODO: Consider some sort of unification between the UpdateDriver, ModifierInterface
        // and UpdateRequest structures.
        UpdateDriver::Options opts;
        opts.multi = request.isMulti();
        opts.upsert = request.isUpsert();
        opts.logOp = request.shouldUpdateOpLog();
        opts.modOptions = ModifierInterface::Options( request.isFromReplication(), shouldValidate );
        UpdateDriver driver( opts );

        Status status = driver.parse( request.getUpdates() );
        if ( !status.isOK() ) {
            uasserted( 16840, status.reason() );
        }

        return update(request, opDebug, &driver);
    }

    UpdateResult update(const UpdateRequest& request, OpDebug* opDebug, UpdateDriver* driver) {

        LOG(3) << "processing update : " << request;
        const NamespaceString& nsString = request.getNamespaceString();

        validateUpdate( nsString.ns().c_str(), request.getUpdates(), request.getQuery() );

        Collection* collection = cc().database()->getCollection( nsString.ns() );

        // TODO: This seems a bit circuitious.
        opDebug->updateobj = request.getUpdates();

        if ( collection )
            driver->refreshIndexKeys( collection->infoCache()->indexKeys() );

        CanonicalQuery* cq;
        // We pass -limit because a positive limit means 'batch size' but negative limit is a
        // hard limit.
        if (!CanonicalQuery::canonicalize(nsString, request.getQuery(), &cq).isOK()) {
            uasserted(17242, "could not canonicalize query " + request.getQuery().toString());
        }

        Runner* rawRunner;
        if (!getRunner(cq, &rawRunner).isOK()) {
            uasserted(17243, "could not get runner " + request.getQuery().toString());
        }

        auto_ptr<Runner> runner(rawRunner);
        RunnerYieldPolicy yieldPolicy;

        // If the update was marked with '$isolated' (a.k.a '$atomic'), we are not allowed to
        // yield while evaluating the update loop below.
        const bool isolated = QueryPlannerCommon::hasNode(cq->root(), MatchExpression::ATOMIC);

        //
        // We'll start assuming we have one or more documents for this update. (Otherwise,
        // we'll fallback to upserting.)
        //

        // We record that this will not be an upsert, in case a mod doesn't want to be applied
        // when in strict update mode.
        driver->setContext( ModifierInterface::ExecInfo::UPDATE_CONTEXT );

        int numMatched = 0;
        unordered_set<DiskLoc, DiskLoc::Hasher> updatedLocs;

        // Reset these counters on each call. We might re-enter this function to retry this
        // update if we throw a page fault exception below, and we rely on these counters
        // reflecting only the actions taken locally. In particlar, we must have the no-op
        // counter reset so that we can meaningfully comapre it with numMatched above.
        opDebug->nscanned = 0;
        opDebug->nupdateNoops = 0;

        mutablebson::Document doc;
        mutablebson::DamageVector damages;

        BSONObj oldObj;
        DiskLoc loc;
        Runner::RunnerState state;
        while (Runner::RUNNER_ADVANCED == (state = runner->getNext(&oldObj, &loc))) {
            if ( !isolated && opDebug->nscanned != 0 ) {
                if (yieldPolicy.shouldYield()) {
                    if (!yieldPolicy.yieldAndCheckIfOK(runner.get())) {
                        break;
                    }

                    // We yielded and recovered OK, and our cursor is still good. Details about
                    // our namespace may have changed while we were yielded, so we re-acquire
                    // them here. If we can't do so, escape the update loop. Otherwise, refresh
                    // the driver so that it knows about what is currently indexed.

                    collection = cc().database()->getCollection( nsString.ns() );
                    if (NULL == collection) {
                        break;
                    }

                    // TODO: This copies the index keys, but it may not need to do so.
                    driver->refreshIndexKeys( collection->infoCache()->indexKeys() );
                }
            }

            // We fill this with the new locs of updates so we don't double-update anything.
            if (updatedLocs.count(loc)) {
                continue;
            }

            // We count how many documents we scanned even though we may skip those that are
            // deemed duplicated. The final 'numUpdated' and 'nscanned' numbers may differ for
            // that reason.
            // XXX: pull this out of the plan.
            opDebug->nscanned++;

            // Found a matching document
            numMatched++;

            // Ask the driver to apply the mods. It may be that the driver can apply those "in
            // place", that is, some values of the old document just get adjusted without any
            // change to the binary layout on the bson layer. It may be that a whole new
            // document is needed to accomodate the new bson layout of the resulting document.
            doc.reset( oldObj, mutablebson::Document::kInPlaceEnabled );
            BSONObj logObj;

            // If there was a matched field, obtain it.
            // XXX: do we always want to do this additional match?
            MatchDetails matchDetails;
            matchDetails.requestElemMatchKey();
            verify(cq->root()->matchesBSON(oldObj, &matchDetails));

            string matchedField;
            if (matchDetails.hasElemMatchKey())
                matchedField = matchDetails.elemMatchKey();

            Status status = driver->update( matchedField, &doc, &logObj );
            if ( !status.isOK() ) {
                uasserted( 16837, status.reason() );
            }

            // If the driver applied the mods in place, we can ask the mutable for what
            // changed. We call those changes "damages". :) We use the damages to inform the
            // journal what was changed, and then apply them to the original document
            // ourselves. If, however, the driver applied the mods out of place, we ask it to
            // generate a new, modified document for us. In that case, the file manager will
            // take care of the journaling details for us.
            //
            // This code flow is admittedly odd. But, right now, journaling is baked in the file
            // manager. And if we aren't using the file manager, we have to do jounaling
            // ourselves.
            bool objectWasChanged = false;
            BSONObj newObj;
            const char* source = NULL;
            bool inPlace = doc.getInPlaceUpdates(&damages, &source);

            // If something changed in the document, verify that no shard keys were altered.
            if ((!inPlace || !damages.empty()) && driver->modsAffectShardKeys())
                uassertStatusOK( driver->checkShardKeysUnaltered (oldObj, doc ) );

            runner->saveState();

            if ( inPlace && !driver->modsAffectIndices() ) {
                // If a set of modifiers were all no-ops, we are still 'in place', but there is
                // no work to do, in which case we want to consider the object unchanged.
                if (!damages.empty() ) {
                    collection->details()->paddingFits();

                    // All updates were in place. Apply them via durability and writing pointer.
                    mutablebson::DamageVector::const_iterator where = damages.begin();
                    const mutablebson::DamageVector::const_iterator end = damages.end();
                    for( ; where != end; ++where ) {
                        const char* sourcePtr = source + where->sourceOffset;
                        void* targetPtr = getDur().writingPtr(
                            const_cast<char*>(oldObj.objdata()) + where->targetOffset,
                            where->size);
                        std::memcpy(targetPtr, sourcePtr, where->size);
                    }
                    objectWasChanged = true;
                    opDebug->fastmod = true;
                }
                newObj = oldObj;
            }
            else {

                // The updates were not in place. Apply them through the file manager.
                newObj = doc.getObject();
                StatusWith<DiskLoc> res = collection->updateDocument( loc,
                                                                      newObj,
                                                                      true,
                                                                      opDebug );
                uassertStatusOK( res.getStatus() );
                DiskLoc newLoc = res.getValue();

                // If we've moved this object to a new location, make sure we don't apply
                // that update again if our traversal picks the object again.
                //
                // We also take note that the diskloc if the updates are affecting indices.
                // Chances are that we're traversing one of them and they may be multi key and
                // therefore duplicate disklocs.
                if ( newLoc != loc || driver->modsAffectIndices()  ) {
                    updatedLocs.insert( newLoc );
                }

                objectWasChanged = true;
            }

            // Log Obj
            if ( request.shouldUpdateOpLog() ) {
                if ( driver->isDocReplacement() || !logObj.isEmpty() ) {
                    BSONObj idQuery = driver->makeOplogEntryQuery(newObj, request.isMulti());
                    logOp("u", nsString.ns().c_str(), logObj , &idQuery,
                          NULL, request.isFromMigration(), &newObj);
                }
            }

            // If it was noop since the document didn't change, record that.
            if (!objectWasChanged)
                opDebug->nupdateNoops++;

            if (!request.isMulti()) {
                break;
            }

            getDur().commitIfNeeded();

            if (!runner->restoreState()) {
                break;
            }
        }

        // TODO: Can this be simplified?
        if ((numMatched > 0) || (numMatched == 0 && !request.isUpsert()) ) {
            opDebug->nupdated = numMatched;
            return UpdateResult( numMatched > 0 /* updated existing object(s) */,
                                 !driver->isDocReplacement() /* $mod or obj replacement */,
                                 numMatched /* # of docments update, even no-ops */,
                                 BSONObj() );
        }

        //
        // We haven't found any existing document so an insert is done
        // (upsert is true).
        //
        opDebug->upsert = true;

        // Since this is an insert (no docs found and upsert:true), we will be logging it
        // as an insert in the oplog. We don't need the driver's help to build the
        // oplog record, then. We also set the context of the update driver to the INSERT_CONTEXT.
        // Some mods may only work in that context (e.g. $setOnInsert).
        driver->setLogOp( false );
        driver->setContext( ModifierInterface::ExecInfo::INSERT_CONTEXT );

        // Reset the document we will be writing to
        doc.reset();
        if ( request.getQuery().hasElement("_id") ) {
            uassertStatusOK(doc.root().appendElement(request.getQuery().getField("_id")));
        }


        // This remains the empty object in the case of an object replacement, but in the case
        // of an upsert where we are creating a base object from the query and applying mods,
        // we capture the query as the original so that we can detect shard key mutations.
        BSONObj original = BSONObj();

        // If this is a $mod base update, we need to generate a document by examining the
        // query and the mods. Otherwise, we can use the object replacement sent by the user
        // update command that was parsed by the driver before.
        // In the following block we handle the query part, and then do the regular mods after.
        if ( *request.getUpdates().firstElementFieldName() == '$' ) {
            original = request.getQuery();
            uassertStatusOK(UpdateDriver::createFromQuery(original, doc));
            opDebug->fastmodinsert = true;
        }

        // Apply the update modifications and then log the update as an insert manually.
        Status status = driver->update( StringData(), &doc, NULL /* no oplog record */);
        if ( !status.isOK() ) {
            uasserted( 16836, status.reason() );
        }

        // Validate that the object replacement or modifiers resulted in a document
        // that contains all the shard keys.
        uassertStatusOK( driver->checkShardKeysUnaltered(original, doc) );

        BSONObj newObj = doc.getObject();

        theDataFileMgr.insertWithObjMod( nsString.ns().c_str(), newObj, false, request.isGod() );
        if ( request.shouldUpdateOpLog() ) {
            logOp( "i", nsString.ns().c_str(), newObj,
                   NULL, NULL, request.isFromMigration(), &newObj );
        }

        opDebug->nupdated = 1;
        return UpdateResult( false /* updated a non existing document */,
                             !driver->isDocReplacement() /* $mod or obj replacement? */,
                             1 /* count of updated documents */,
                             newObj /* object that was upserted */ );
    }

    BSONObj applyUpdateOperators( const BSONObj& from, const BSONObj& operators ) {
        UpdateDriver::Options opts;
        opts.multi = false;
        opts.upsert = false;
        UpdateDriver driver( opts );
        Status status = driver.parse( operators );
        if ( !status.isOK() ) {
            uasserted( 16838, status.reason() );
        }

        mutablebson::Document doc( from, mutablebson::Document::kInPlaceDisabled );
        status = driver.update( StringData(), &doc, NULL /* not oplogging */ );
        if ( !status.isOK() ) {
            uasserted( 16839, status.reason() );
        }

        return doc.getObject();
    }

}  // namespace mongo
