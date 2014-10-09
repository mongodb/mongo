/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/exec/update.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/repl/repl_coordinator_global.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/util/log.h"

namespace mongo {

    namespace mb = mutablebson;

    namespace {

        const char idFieldName[] = "_id";
        const FieldRef idFieldRef(idFieldName);

        Status storageValid(const mb::Document&, const bool);
        Status storageValid(const mb::ConstElement&, const bool);
        Status storageValidChildren(const mb::ConstElement&, const bool);

        /**
         * mutable::document storageValid check -- like BSONObj::_okForStorage
         */
        Status storageValid(const mb::Document& doc, const bool deep = true) {
            mb::ConstElement currElem = doc.root().leftChild();
            while (currElem.ok()) {
                if (currElem.getFieldName() == idFieldName) {
                    switch (currElem.getType()) {
                        case RegEx:
                        case Array:
                        case Undefined:
                            return Status(ErrorCodes::InvalidIdField,
                                          str::stream() << "The '_id' value cannot be of type "
                                                        << typeName(currElem.getType()));
                        default:
                            break;
                    }
                }
                Status s = storageValid(currElem, deep);
                if (!s.isOK())
                    return s;
                currElem = currElem.rightSibling();
            }

            return Status::OK();
        }

        /**
         * Validates an element that has a field name which starts with a dollar sign ($).
         * In the case of a DBRef field ($id, $ref, [$db]) these fields may be valid in
         * the correct order/context only.
         */
        Status validateDollarPrefixElement(const mb::ConstElement elem, const bool deep) {
            mb::ConstElement curr = elem;
            StringData currName = elem.getFieldName();

            // Found a $db field
            if (currName == "$db") {
                if (curr.getType() != String) {
                    return Status(ErrorCodes::InvalidDBRef,
                                  str::stream() << "The DBRef $db field must be a String, not a "
                                  << typeName(curr.getType()));
                }
                curr = curr.leftSibling();

                if (!curr.ok() || (curr.getFieldName() != "$id"))
                    return Status(ErrorCodes::InvalidDBRef,
                                  "Found $db field without a $id before it, which is invalid.");

                currName = curr.getFieldName();
            }

            // Found a $id field
            if (currName == "$id") {
                Status s = storageValidChildren(curr, deep);
                if (!s.isOK())
                    return s;

                curr = curr.leftSibling();
                if (!curr.ok() || (curr.getFieldName() != "$ref")) {
                    return Status(ErrorCodes::InvalidDBRef,
                                  "Found $id field without a $ref before it, which is invalid.");
                }

                currName = curr.getFieldName();
            }

            if (currName == "$ref") {
                if (curr.getType() != String) {
                    return Status(ErrorCodes::InvalidDBRef,
                                  str::stream() << "The DBRef $ref field must be a String, not a "
                                  << typeName(curr.getType()));
                }

                if (!curr.rightSibling().ok() || curr.rightSibling().getFieldName() != "$id")
                    return Status(ErrorCodes::InvalidDBRef,
                                  str::stream() << "The DBRef $ref field must be "
                                  "following by a $id field");
            }
            else {
                // not an okay, $ prefixed field name.
                return Status(ErrorCodes::DollarPrefixedFieldName,
                              str::stream() << "The dollar ($) prefixed field '"
                                            << elem.getFieldName() << "' in '"
                                            << mb::getFullName(elem)
                                            << "' is not valid for storage.");

            }

            return Status::OK();
        }

        Status storageValid(const mb::ConstElement& elem, const bool deep = true) {
            if (!elem.ok())
                return Status(ErrorCodes::BadValue, "Invalid elements cannot be stored.");

            // Field names of elements inside arrays are not meaningful in mutable bson,
            // so we do not want to validate them.
            //
            // TODO: Revisit how mutable handles array field names. We going to need to make
            // this better if we ever want to support ordered updates that can alter the same
            // element repeatedly; see SERVER-12848.
            const bool childOfArray = elem.parent().ok() ?
                (elem.parent().getType() == mongo::Array) : false;

            if (!childOfArray) {
                StringData fieldName = elem.getFieldName();
                // Cannot start with "$", unless dbref
                if (fieldName[0] == '$') {
                    Status status = validateDollarPrefixElement(elem, deep);
                    if (!status.isOK())
                        return status;
                }
                else if (fieldName.find(".") != string::npos) {
                    // Field name cannot have a "." in it.
                    return Status(ErrorCodes::DottedFieldName,
                                  str::stream() << "The dotted field '"
                                                << elem.getFieldName() << "' in '"
                                                << mb::getFullName(elem)
                                                << "' is not valid for storage.");
                }
            }

            // Check children if there are any.
            Status s = storageValidChildren(elem, deep);
            if (!s.isOK())
                return s;

            return Status::OK();
        }

        Status storageValidChildren(const mb::ConstElement& elem, const bool deep) {
            if (!elem.hasChildren())
                return Status::OK();

            mb::ConstElement curr = elem.leftChild();
            while (curr.ok()) {
                Status s = storageValid(curr, deep);
                if (!s.isOK())
                    return s;
                curr = curr.rightSibling();
            }

            return Status::OK();
        }

        /**
         * This will verify that all updated fields are
         *   1.) Valid for storage (checking parent to make sure things like DBRefs are valid)
         *   2.) Compare updated immutable fields do not change values
         *
         * If updateFields is empty then it was replacement and/or we need to check all fields
         */
        inline Status validate(const BSONObj& original,
                               const FieldRefSet& updatedFields,
                               const mb::Document& updated,
                               const std::vector<FieldRef*>* immutableAndSingleValueFields,
                               const ModifierInterface::Options& opts) {

            LOG(3) << "update validate options -- "
                   << " updatedFields: " << updatedFields
                   << " immutableAndSingleValueFields.size:"
                   << (immutableAndSingleValueFields ? immutableAndSingleValueFields->size() : 0)
                   << " fromRepl: " << opts.fromReplication
                   << " validate:" << opts.enforceOkForStorage;

            // 1.) Loop through each updated field and validate for storage
            // and detect immutable field updates

            // The set of possibly changed immutable fields -- we will need to check their vals
            FieldRefSet changedImmutableFields;

            // Check to see if there were no fields specified or if we are not validating
            // The case if a range query, or query that didn't result in saved fields
            if (updatedFields.empty() || !opts.enforceOkForStorage) {
                if (opts.enforceOkForStorage) {
                    // No specific fields were updated so the whole doc must be checked
                    Status s = storageValid(updated, true);
                    if (!s.isOK())
                        return s;
                }

                // Check all immutable fields
                if (immutableAndSingleValueFields)
                    changedImmutableFields.fillFrom(*immutableAndSingleValueFields);
            }
            else {

                // TODO: Change impl so we don't need to create a new FieldRefSet
                //       -- move all conflict logic into static function on FieldRefSet?
                FieldRefSet immutableFieldRef;
                if (immutableAndSingleValueFields)
                    immutableFieldRef.fillFrom(*immutableAndSingleValueFields);

                FieldRefSet::const_iterator where = updatedFields.begin();
                const FieldRefSet::const_iterator end = updatedFields.end();
                for( ; where != end; ++where) {
                    const FieldRef& current = **where;

                    // Find the updated field in the updated document.
                    mutablebson::ConstElement newElem = updated.root();
                    size_t currentPart = 0;
                    while (newElem.ok() && currentPart < current.numParts())
                        newElem = newElem[current.getPart(currentPart++)];

                    // newElem might be missing if $unset/$renamed-away
                    if (newElem.ok()) {
                        Status s = storageValid(newElem, true);
                        if (!s.isOK())
                            return s;
                    }
                    // Check if the updated field conflicts with immutable fields
                    immutableFieldRef.findConflicts(&current, &changedImmutableFields);
                }
            }

            const bool checkIdField = (updatedFields.empty() && !original.isEmpty()) ||
                                      updatedFields.findConflicts(&idFieldRef, NULL);

            // Add _id to fields to check since it too is immutable
            if (checkIdField)
                changedImmutableFields.keepShortest(&idFieldRef);
            else if (changedImmutableFields.empty()) {
                // Return early if nothing changed which is immutable
                return Status::OK();
            }

            LOG(4) << "Changed immutable fields: " << changedImmutableFields;
            // 2.) Now compare values of the changed immutable fields (to make sure they haven't)

            const mutablebson::ConstElement newIdElem = updated.root()[idFieldName];

            FieldRefSet::const_iterator where = changedImmutableFields.begin();
            const FieldRefSet::const_iterator end = changedImmutableFields.end();
            for( ; where != end; ++where ) {
                const FieldRef& current = **where;

                // Find the updated field in the updated document.
                mutablebson::ConstElement newElem = updated.root();
                size_t currentPart = 0;
                while (newElem.ok() && currentPart < current.numParts())
                    newElem = newElem[current.getPart(currentPart++)];

                if (!newElem.ok()) {
                    if (original.isEmpty()) {
                        // If the _id is missing and not required, then skip this check
                        if (!(current.dottedField() == idFieldName))
                            return Status(ErrorCodes::NoSuchKey,
                                          mongoutils::str::stream()
                                          << "After applying the update, the new"
                                          << " document was missing the '"
                                          << current.dottedField()
                                          << "' (required and immutable) field.");

                    }
                    else {
                        if (current.dottedField() != idFieldName)
                            return Status(ErrorCodes::ImmutableField,
                                          mongoutils::str::stream()
                                          << "After applying the update to the document with "
                                          << newIdElem.toString()
                                          << ", the '" << current.dottedField()
                                          << "' (required and immutable) field was "
                                             "found to have been removed --"
                                          << original);
                    }
                }
                else {

                    // Find the potentially affected field in the original document.
                    const BSONElement oldElem = original.getFieldDotted(current.dottedField());
                    const BSONElement oldIdElem = original.getField(idFieldName);

                    // Ensure no arrays since neither _id nor shard keys can be in an array, or one.
                    mb::ConstElement currElem = newElem;
                    while (currElem.ok()) {
                        if (currElem.getType() == Array) {
                            return Status(ErrorCodes::NotSingleValueField,
                                          mongoutils::str::stream()
                                          << "After applying the update to the document {"
                                          << (oldIdElem.ok() ? oldIdElem.toString() :
                                                               newIdElem.toString())
                                          << " , ...}, the (immutable) field '"
                                          << current.dottedField()
                                          << "' was found to be an array or array descendant.");
                        }
                        currElem = currElem.parent();
                    }

                    // If we have both (old and new), compare them. If we just have new we are good
                    if (oldElem.ok() && newElem.compareWithBSONElement(oldElem, false) != 0) {
                        return Status(ErrorCodes::ImmutableField,
                                      mongoutils::str::stream()
                                      << "After applying the update to the document {"
                                      << (oldIdElem.ok() ? oldIdElem.toString() :
                                                           newIdElem.toString())
                                      << " , ...}, the (immutable) field '" << current.dottedField()
                                      << "' was found to have been altered to "
                                      << newElem.toString());
                    }
                }
            }

            return Status::OK();
        }

        Status ensureIdAndFirst(mb::Document& doc) {
            mb::Element idElem = mb::findFirstChildNamed(doc.root(), idFieldName);

            // Move _id as first element if it exists
            if (idElem.ok()) {
                if (idElem.leftSibling().ok()) {
                    Status s = idElem.remove();
                    if (!s.isOK())
                        return s;
                    s = doc.root().pushFront(idElem);
                    if (!s.isOK())
                        return s;
                }
            }
            else {
                // Create _id if the document does not currently have one.
                idElem = doc.makeElementNewOID(idFieldName);
                if (!idElem.ok())
                    return Status(ErrorCodes::BadValue,
                                  "Could not create new _id ObjectId element.",
                                  17268);
                Status s = doc.root().pushFront(idElem);
                if (!s.isOK())
                    return s;
            }

            return Status::OK();

        }
    } // namespace

    // static
    const char* UpdateStage::kStageType = "UPDATE";

    UpdateStage::UpdateStage(const UpdateStageParams& params,
                             WorkingSet* ws,
                             Database* db,
                             PlanStage* child)
        : _params(params),
          _ws(ws),
          _db(db),
          _child(child),
          _commonStats(kStageType),
          _updatedLocs(params.request->isMulti() ? new DiskLocSet() : NULL),
          _doc(params.driver->getDocument()) {
        // We are an update until we fall into the insert case.
        params.driver->setContext(ModifierInterface::ExecInfo::UPDATE_CONTEXT);

        _collection = db->getCollection(params.request->getOpCtx(),
                                        params.request->getNamespaceString().ns());
    }

    void UpdateStage::transformAndUpdate(BSONObj& oldObj, DiskLoc& loc) {
        const UpdateRequest* request = _params.request;
        UpdateDriver* driver = _params.driver;
        CanonicalQuery* cq = _params.canonicalQuery;
        UpdateLifecycle* lifecycle = request->getLifecycle();

        // Ask the driver to apply the mods. It may be that the driver can apply those "in
        // place", that is, some values of the old document just get adjusted without any
        // change to the binary layout on the bson layer. It may be that a whole new
        // document is needed to accomodate the new bson layout of the resulting document.
        _doc.reset(oldObj, mutablebson::Document::kInPlaceEnabled);
        BSONObj logObj;

        FieldRefSet updatedFields;

        Status status = Status::OK();
        if (!driver->needMatchDetails()) {
            // If we don't need match details, avoid doing the rematch
            status = driver->update(StringData(), &_doc, &logObj, &updatedFields);
        }
        else {
            // If there was a matched field, obtain it.
            MatchDetails matchDetails;
            matchDetails.requestElemMatchKey();

            dassert(cq);
            verify(cq->root()->matchesBSON(oldObj, &matchDetails));

            string matchedField;
            if (matchDetails.hasElemMatchKey())
                matchedField = matchDetails.elemMatchKey();

            // TODO: Right now, each mod checks in 'prepare' that if it needs positional
            // data, that a non-empty StringData() was provided. In principle, we could do
            // that check here in an else clause to the above conditional and remove the
            // checks from the mods.

            status = driver->update(matchedField, &_doc, &logObj, &updatedFields);
        }

        if (!status.isOK()) {
            uasserted(16837, status.reason());
        }

        // Ensure _id exists and is first
        uassertStatusOK(ensureIdAndFirst(_doc));

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
        bool docWasModified = false;
        BSONObj newObj;
        const char* source = NULL;
        bool inPlace = _doc.getInPlaceUpdates(&_damages, &source);

        // If something changed in the document, verify that no immutable fields were changed
        // and data is valid for storage.
        if ((!inPlace || !_damages.empty()) ) {
            if (!(request->isFromReplication() || request->isFromMigration())) {
                const std::vector<FieldRef*>* immutableFields = NULL;
                if (lifecycle)
                    immutableFields = lifecycle->getImmutableFields();

                uassertStatusOK(validate(oldObj,
                                         updatedFields,
                                         _doc,
                                         immutableFields,
                                         driver->modOptions()) );
            }
        }


        // Save state before making changes
        saveState();

        {
            WriteUnitOfWork wunit(request->getOpCtx());

            if (inPlace && !driver->modsAffectIndices()) {
                // If a set of modifiers were all no-ops, we are still 'in place', but there
                // is no work to do, in which case we want to consider the object unchanged.
                if (!_damages.empty() ) {
                    // Don't actually do the write if this is an explain.
                    if (!request->isExplain()) {
                        invariant(_collection);
                        _collection->updateDocumentWithDamages(request->getOpCtx(), loc, source,
                                                               _damages);
                    }
                    docWasModified = true;
                    _specificStats.fastmod = true;
                }

                newObj = oldObj;
            }
            else {
                // The updates were not in place. Apply them through the file manager.

                newObj = _doc.getObject();
                uassert(17419,
                        str::stream() << "Resulting document after update is larger than "
                        << BSONObjMaxUserSize,
                        newObj.objsize() <= BSONObjMaxUserSize);
                docWasModified = true;

                // Don't actually do the write if this is an explain.
                if (!request->isExplain()) {
                    invariant(_collection);
                    StatusWith<DiskLoc> res = _collection->updateDocument(request->getOpCtx(),
                                                                          loc,
                                                                          newObj,
                                                                          true,
                                                                          _params.opDebug);
                    uassertStatusOK(res.getStatus());
                    DiskLoc newLoc = res.getValue();

                    // If the document moved, we might see it again in a collection scan (maybe it's
                    // a document after our current document).
                    //
                    // If the document is indexed and the mod changes an indexed value, we might see
                    // it again.  For an example, see the comment above near declaration of
                    // updatedLocs.
                    if (_updatedLocs && (newLoc != loc || driver->modsAffectIndices())) {
                        _updatedLocs->insert(newLoc);
                    }
                }
            }

            // Call logOp if requested, and we're not an explain.
            if (request->shouldCallLogOp() && !logObj.isEmpty() && !request->isExplain()) {
                BSONObj idQuery = driver->makeOplogEntryQuery(newObj, request->isMulti());
                repl::logOp(request->getOpCtx(),
                            "u",
                            request->getNamespaceString().ns().c_str(),
                            logObj,
                            &idQuery,
                            NULL,
                            request->isFromMigration());
            }
            wunit.commit();
        }


        // Restore state after modification

        // As restoreState may restore (recreate) cursors, make sure to restore the
        // state outside of the WritUnitOfWork.

        restoreState(request->getOpCtx());

        // Only record doc modifications if they wrote (exclude no-ops). Explains get
        // recorded as if they wrote.
        if (docWasModified) {
            _specificStats.nModified++;
        }
    }

    void UpdateStage::doInsert() {
        _specificStats.inserted = true;

        const UpdateRequest* request = _params.request;
        UpdateDriver* driver = _params.driver;
        CanonicalQuery* cq = _params.canonicalQuery;
        UpdateLifecycle* lifecycle = request->getLifecycle();

        // Since this is an insert (no docs found and upsert:true), we will be logging it
        // as an insert in the oplog. We don't need the driver's help to build the
        // oplog record, then. We also set the context of the update driver to the INSERT_CONTEXT.
        // Some mods may only work in that context (e.g. $setOnInsert).
        driver->setLogOp(false);
        driver->setContext(ModifierInterface::ExecInfo::INSERT_CONTEXT);

        // Reset the document we will be writing to
        _doc.reset();

        // This remains the empty object in the case of an object replacement, but in the case
        // of an upsert where we are creating a base object from the query and applying mods,
        // we capture the query as the original so that we can detect immutable field mutations.
        BSONObj original = BSONObj();

        // Calling populateDocumentWithQueryFields will populate the '_doc' with fields from the
        // query which creates the base of the update for the inserted doc (because upsert
        // was true).
        if (cq) {
            uassertStatusOK(driver->populateDocumentWithQueryFields(cq, _doc));
            if (!driver->isDocReplacement()) {
                _specificStats.fastmodinsert = true;
                // We need all the fields from the query to compare against for validation below.
                original = _doc.getObject();
            }
            else {
                original = request->getQuery();
            }
        }
        else {
            fassert(17354, CanonicalQuery::isSimpleIdQuery(request->getQuery()));
            BSONElement idElt = request->getQuery()[idFieldName];
            original = idElt.wrap();
            fassert(17352, _doc.root().appendElement(idElt));
        }

        // Apply the update modifications and then log the update as an insert manually.
        Status status = driver->update(StringData(), &_doc);
        if (!status.isOK()) {
            uasserted(16836, status.reason());
        }

        // Ensure _id exists and is first
        uassertStatusOK(ensureIdAndFirst(_doc));

        // Validate that the object replacement or modifiers resulted in a document
        // that contains all the immutable keys and can be stored if it isn't coming
        // from a migration or via replication.
        if (!(request->isFromReplication() || request->isFromMigration())){
            const std::vector<FieldRef*>* immutableFields = NULL;
            if (lifecycle)
                immutableFields = lifecycle->getImmutableFields();

            FieldRefSet noFields;
            // This will only validate the modified fields if not a replacement.
            uassertStatusOK(validate(original,
                                     noFields,
                                     _doc,
                                     immutableFields,
                                     driver->modOptions()) );
        }

        // Insert the doc
        BSONObj newObj = _doc.getObject();
        uassert(17420,
                str::stream() << "Document to upsert is larger than " << BSONObjMaxUserSize,
                newObj.objsize() <= BSONObjMaxUserSize);

        _specificStats.objInserted = newObj;

        // If this is an explain, bail out now without doing the insert.
        if (request->isExplain()) {
            return;
        }

        WriteUnitOfWork wunit(request->getOpCtx());
        invariant(_collection);
        StatusWith<DiskLoc> newLoc = _collection->insertDocument(request->getOpCtx(),
                                                                 newObj,
                                                                 !request->isGod()/*enforceQuota*/);
        uassertStatusOK(newLoc.getStatus());
        if (request->shouldCallLogOp()) {
            repl::logOp(request->getOpCtx(),
                        "i",
                        request->getNamespaceString().ns().c_str(),
                        newObj,
                        NULL,
                        NULL,
                        request->isFromMigration());
        }

        wunit.commit();
    }

    bool UpdateStage::doneUpdating() {
        // We're done updating if either the child has no more results to give us, or we've
        // already gotten a result back and we're not a multi-update.
        return _child->isEOF() || (_specificStats.nMatched > 0 && !_params.request->isMulti());
    }

    bool UpdateStage::needInsert() {
        // We need to insert if
        //  1) we haven't inserted already,
        //  2) the child stage returned zero matches, and
        //  3) the user asked for an upsert.
        return !_specificStats.inserted
               && _specificStats.nMatched == 0
               && _params.request->isUpsert();
    }

    bool UpdateStage::isEOF() {
        return doneUpdating() && !needInsert();
    }

    PlanStage::StageState UpdateStage::work(WorkingSetID* out) {
        ++_commonStats.works;

        // Adds the amount of time taken by work() to executionTimeMillis.
        ScopedTimer timer(&_commonStats.executionTimeMillis);

        if (isEOF()) { return PlanStage::IS_EOF; }

        if (doneUpdating()) {
            // Even if we're done updating, we may have some inserting left to do.
            if (needInsert()) {
                doInsert();
            }

            // At this point either we're done updating and there was no insert to do,
            // or we're done updating and we're done inserting. Either way, we're EOF.
            invariant(isEOF());
            return PlanStage::IS_EOF;
        }

        // If we're here, then we still have to ask for results from the child and apply
        // updates to them. We should only get here if the collection exists.
        invariant(_collection);

        WorkingSetID id = WorkingSet::INVALID_ID;
        StageState status = _child->work(&id);

        if (PlanStage::ADVANCED == status) {
            // Need to get these things from the result returned by the child.
            DiskLoc loc;
            BSONObj oldObj;

            WorkingSetMember* member = _ws->get(id);

            if (!member->hasLoc()) {
                _ws->free(id);
                const std::string errmsg = "update stage failed to read member w/ loc from child";
                *out = WorkingSetCommon::allocateStatusMember(_ws, Status(ErrorCodes::InternalError,
                                                                          errmsg));
                return PlanStage::FAILURE;
            }
            loc = member->loc;

            // Updates can't have projections. This means that covering analysis will always add
            // a fetch. We should always get fetched data, and never just key data.
            invariant(member->hasObj());
            oldObj = member->obj;

            // If we're here, then we have retrieved both a DiskLoc and the corresponding
            // unowned object from the child stage. Since we have the object and the diskloc,
            // we can free the WSM.
            _ws->free(id);

            // We fill this with the new locs of moved doc so we don't double-update.
            if (_updatedLocs && _updatedLocs->count(loc) > 0) {
                // Found a loc that we already updated.
                ++_commonStats.needTime;
                return PlanStage::NEED_TIME;
            }

            ++_specificStats.nMatched;

            // Do the update and return.
            transformAndUpdate(oldObj, loc);
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::IS_EOF == status) {
            // The child is out of results, but we might not be done yet because we still might
            // have to do an insert.
            ++_commonStats.needTime;
            return PlanStage::NEED_TIME;
        }
        else if (PlanStage::FAILURE == status) {
            *out = id;
            // If a stage fails, it may create a status WSM to indicate why it failed, in which case
            // 'id' is valid.  If ID is invalid, we create our own error message.
            if (WorkingSet::INVALID_ID == id) {
                const std::string errmsg = "delete stage failed to read in results from child";
                *out = WorkingSetCommon::allocateStatusMember(_ws, Status(ErrorCodes::InternalError,
                                                                          errmsg));
                return PlanStage::FAILURE;
            }
            return status;
        }
        else {
            if (PlanStage::NEED_TIME == status) {
                ++_commonStats.needTime;
            }
            return status;
        }
    }

    void UpdateStage::saveState() {
        ++_commonStats.yields;
        _child->saveState();
    }

    void UpdateStage::restoreState(OperationContext* opCtx) {
        ++_commonStats.unyields;
        _child->restoreState(opCtx);
    }

    void UpdateStage::invalidate(const DiskLoc& dl, InvalidationType type) {
        ++_commonStats.invalidates;
        _child->invalidate(dl, type);
    }

    vector<PlanStage*> UpdateStage::getChildren() const {
        vector<PlanStage*> children;
        children.push_back(_child.get());
        return children;
    }

    PlanStageStats* UpdateStage::getStats() {
        _commonStats.isEOF = isEOF();
        auto_ptr<PlanStageStats> ret(new PlanStageStats(_commonStats, STAGE_UPDATE));
        ret->specific.reset(new UpdateStats(_specificStats));
        ret->children.push_back(_child->getStats());
        return ret.release();
    }

    const CommonStats* UpdateStage::getCommonStats() {
        return &_commonStats;
    }

    const SpecificStats* UpdateStage::getSpecificStats() {
        return &_specificStats;
    }

} // namespace mongo
