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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/exec/update.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/exec/write_stage_common.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/update_lifecycle.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;
using stdx::make_unique;

namespace mb = mutablebson;
namespace dps = ::mongo::dotted_path_support;

namespace {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

Status storageValid(const mb::Document&, const bool = true);
Status storageValid(const mb::ConstElement&, const bool = true);
Status storageValidChildren(const mb::ConstElement&, const bool = true);

/**
 * mutable::document storageValid check -- like BSONObj::_okForStorage
 */
Status storageValid(const mb::Document& doc, const bool deep) {
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
    LOG(5) << "validateDollarPrefixElement -- validating field '" << currName << "'";
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
    } else {
        // not an okay, $ prefixed field name.
        return Status(ErrorCodes::DollarPrefixedFieldName,
                      str::stream() << "The dollar ($) prefixed field '" << elem.getFieldName()
                                    << "' in '"
                                    << mb::getFullName(elem)
                                    << "' is not valid for storage.");
    }

    return Status::OK();
}

/**
 * Checks that all parents, of the element passed in, are valid for storage
 *
 * Note: The elem argument must be in a valid state when using this function
 */
Status storageValidParents(const mb::ConstElement& elem) {
    const mb::ConstElement& root = elem.getDocument().root();
    if (elem != root) {
        const mb::ConstElement& parent = elem.parent();
        if (parent.ok() && parent != root) {
            Status s = storageValid(parent, false);
            if (s.isOK()) {
                s = storageValidParents(parent);
            }

            return s;
        }
    }
    return Status::OK();
}

Status storageValid(const mb::ConstElement& elem, const bool deep) {
    if (!elem.ok())
        return Status(ErrorCodes::BadValue, "Invalid elements cannot be stored.");

    // Field names of elements inside arrays are not meaningful in mutable bson,
    // so we do not want to validate them.
    //
    // TODO: Revisit how mutable handles array field names. We going to need to make
    // this better if we ever want to support ordered updates that can alter the same
    // element repeatedly; see SERVER-12848.
    const mb::ConstElement& parent = elem.parent();
    const bool childOfArray = parent.ok() ? (parent.getType() == mongo::Array) : false;

    if (!childOfArray) {
        StringData fieldName = elem.getFieldName();
        // Cannot start with "$", unless dbref
        if (fieldName[0] == '$') {
            Status status = validateDollarPrefixElement(elem, deep);
            if (!status.isOK())
                return status;
        } else if (fieldName.find(".") != string::npos) {
            // Field name cannot have a "." in it.
            return Status(ErrorCodes::DottedFieldName,
                          str::stream() << "The dotted field '" << elem.getFieldName() << "' in '"
                                        << mb::getFullName(elem)
                                        << "' is not valid for storage.");
        }
    }

    if (deep) {
        // Check children if there are any.
        Status s = storageValidChildren(elem, deep);
        if (!s.isOK())
            return s;
    }

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
           << " updatedFields: " << updatedFields << " immutableAndSingleValueFields.size:"
           << (immutableAndSingleValueFields ? immutableAndSingleValueFields->size() : 0)
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
    } else {
        // TODO: Change impl so we don't need to create a new FieldRefSet
        //       -- move all conflict logic into static function on FieldRefSet?
        FieldRefSet immutableFieldRef;
        if (immutableAndSingleValueFields)
            immutableFieldRef.fillFrom(*immutableAndSingleValueFields);

        FieldRefSet::const_iterator where = updatedFields.begin();
        const FieldRefSet::const_iterator end = updatedFields.end();
        for (; where != end; ++where) {
            const FieldRef& current = **where;

            // Find the updated field in the updated document.
            mutablebson::ConstElement newElem = updated.root();
            size_t currentPart = 0;
            while (newElem.ok() && currentPart < current.numParts())
                newElem = newElem[current.getPart(currentPart++)];

            // newElem might be missing if $unset/$renamed-away
            if (newElem.ok()) {
                // Check element, and its children
                Status s = storageValid(newElem, true);
                if (!s.isOK())
                    return s;

                // Check parents to make sure they are valid as well.
                s = storageValidParents(newElem);
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
    for (; where != end; ++where) {
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
                                  mongoutils::str::stream() << "After applying the update, the new"
                                                            << " document was missing the '"
                                                            << current.dottedField()
                                                            << "' (required and immutable) field.");

            } else {
                if (current.dottedField() != idFieldName)
                    return Status(ErrorCodes::ImmutableField,
                                  mongoutils::str::stream()
                                      << "After applying the update to the document with "
                                      << newIdElem.toString()
                                      << ", the '"
                                      << current.dottedField()
                                      << "' (required and immutable) field was "
                                         "found to have been removed --"
                                      << original);
            }
        } else {
            // Find the potentially affected field in the original document.
            const BSONElement oldElem = dps::extractElementAtPath(original, current.dottedField());
            const BSONElement oldIdElem = original.getField(idFieldName);

            // Ensure no arrays since neither _id nor shard keys can be in an array, or one.
            mb::ConstElement currElem = newElem;
            while (currElem.ok()) {
                if (currElem.getType() == Array) {
                    return Status(
                        ErrorCodes::NotSingleValueField,
                        mongoutils::str::stream()
                            << "After applying the update to the document {"
                            << (oldIdElem.ok() ? oldIdElem.toString() : newIdElem.toString())
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
                                  << oldElem.toString()
                                  << " , ...}, the (immutable) field '"
                                  << current.dottedField()
                                  << "' was found to have been altered to "
                                  << newElem.toString());
            }
        }
    }

    return Status::OK();
}

Status ensureIdFieldIsFirst(mb::Document* doc) {
    mb::Element idElem = mb::findFirstChildNamed(doc->root(), idFieldName);

    if (!idElem.ok()) {
        return {ErrorCodes::InvalidIdField, "_id field is missing"};
    }

    if (idElem.leftSibling().ok()) {
        // Move '_id' to be the first element
        Status s = idElem.remove();
        if (!s.isOK())
            return s;
        s = doc->root().pushFront(idElem);
        if (!s.isOK())
            return s;
    }

    return Status::OK();
}

Status addObjectIDIdField(mb::Document* doc) {
    const auto idElem = doc->makeElementNewOID(idFieldName);
    if (!idElem.ok())
        return {ErrorCodes::BadValue, "Could not create new ObjectId '_id' field.", 17268};

    const auto s = doc->root().pushFront(idElem);
    if (!s.isOK())
        return s;

    return Status::OK();
}

/**
 * Returns true if we should throw a WriteConflictException in order to retry the operation in the
 * case of a conflict. Returns false if we should skip the document and keep going.
 */
bool shouldRestartUpdateIfNoLongerMatches(const UpdateStageParams& params) {
    // When we're doing a findAndModify with a sort, the sort will have a limit of 1, so it will not
    // produce any more results even if there is another matching document. Throw a WCE here so that
    // these operations get another chance to find a matching document. The findAndModify command
    // should automatically retry if it gets a WCE.
    return params.request->shouldReturnAnyDocs() && !params.request->getSort().isEmpty();
};

const std::vector<FieldRef*>* getImmutableFields(OperationContext* txn, const NamespaceString& ns) {
    auto metadata = CollectionShardingState::get(txn, ns)->getMetadata();
    if (metadata) {
        const std::vector<FieldRef*>& fields = metadata->getKeyPatternFields();
        // Return shard-keys as immutable for the update system.
        return &fields;
    }
    return NULL;
}

}  // namespace

const char* UpdateStage::kStageType = "UPDATE";

UpdateStage::UpdateStage(OperationContext* txn,
                         const UpdateStageParams& params,
                         WorkingSet* ws,
                         Collection* collection,
                         PlanStage* child)
    : PlanStage(kStageType, txn),
      _params(params),
      _ws(ws),
      _collection(collection),
      _idRetrying(WorkingSet::INVALID_ID),
      _idReturning(WorkingSet::INVALID_ID),
      _updatedRecordIds(params.request->isMulti() ? new RecordIdSet() : NULL),
      _doc(params.driver->getDocument()) {
    _children.emplace_back(child);
    // We are an update until we fall into the insert case.
    params.driver->setContext(ModifierInterface::ExecInfo::UPDATE_CONTEXT);

    // Before we even start executing, we know whether or not this is a replacement
    // style or $mod style update.
    _specificStats.isDocReplacement = params.driver->isDocReplacement();
}

BSONObj UpdateStage::transformAndUpdate(const Snapshotted<BSONObj>& oldObj, RecordId& recordId) {
    const UpdateRequest* request = _params.request;
    UpdateDriver* driver = _params.driver;
    CanonicalQuery* cq = _params.canonicalQuery;
    UpdateLifecycle* lifecycle = request->getLifecycle();

    // If asked to return new doc, default to the oldObj, in case nothing changes.
    BSONObj newObj = oldObj.value();

    // Ask the driver to apply the mods. It may be that the driver can apply those "in
    // place", that is, some values of the old document just get adjusted without any
    // change to the binary layout on the bson layer. It may be that a whole new document
    // is needed to accomodate the new bson layout of the resulting document. In any event,
    // only enable in-place mutations if the underlying storage engine offers support for
    // writing damage events.
    _doc.reset(oldObj.value(),
               (_collection->updateWithDamagesSupported()
                    ? mutablebson::Document::kInPlaceEnabled
                    : mutablebson::Document::kInPlaceDisabled));

    BSONObj logObj;

    FieldRefSet updatedFields;
    bool docWasModified = false;

    Status status = Status::OK();
    if (!driver->needMatchDetails()) {
        // If we don't need match details, avoid doing the rematch
        status = driver->update(StringData(), &_doc, &logObj, &updatedFields, &docWasModified);
    } else {
        // If there was a matched field, obtain it.
        MatchDetails matchDetails;
        matchDetails.requestElemMatchKey();

        dassert(cq);
        verify(cq->root()->matchesBSON(oldObj.value(), &matchDetails));

        // If we have matched more than one array position, we cannot perform a positional update
        // operation.
        uassert(34412, "ambiguous positional update operation", matchDetails.isValid());

        string matchedField;
        if (matchDetails.hasElemMatchKey())
            matchedField = matchDetails.elemMatchKey();

        // TODO: Right now, each mod checks in 'prepare' that if it needs positional
        // data, that a non-empty StringData() was provided. In principle, we could do
        // that check here in an else clause to the above conditional and remove the
        // checks from the mods.

        status = driver->update(matchedField, &_doc, &logObj, &updatedFields, &docWasModified);
    }

    if (!status.isOK()) {
        uasserted(16837, status.reason());
    }

    // Skip adding _id field if the collection is capped (since capped collection documents can
    // neither grow nor shrink).
    const auto createIdField = !_collection->isCapped();

    // Ensure if _id exists it is first
    status = ensureIdFieldIsFirst(&_doc);
    if (status.code() == ErrorCodes::InvalidIdField) {
        // Create ObjectId _id field if we are doing that
        if (createIdField) {
            uassertStatusOK(addObjectIDIdField(&_doc));
        }
    } else {
        uassertStatusOK(status);
    }

    // See if the changes were applied in place
    const char* source = NULL;
    const bool inPlace = _doc.getInPlaceUpdates(&_damages, &source);

    if (inPlace && _damages.empty()) {
        // An interesting edge case. A modifier didn't notice that it was really a no-op
        // during its 'prepare' phase. That represents a missed optimization, but we still
        // shouldn't do any real work. Toggle 'docWasModified' to 'false'.
        //
        // Currently, an example of this is '{ $pushAll : { x : [] } }' when the 'x' array
        // exists.
        docWasModified = false;
    }

    if (docWasModified) {
        // Verify that no immutable fields were changed and data is valid for storage.

        if (!(!getOpCtx()->writesAreReplicated() || request->isFromMigration())) {
            const std::vector<FieldRef*>* immutableFields = NULL;
            if (lifecycle)
                immutableFields = getImmutableFields(getOpCtx(), request->getNamespaceString());

            uassertStatusOK(validate(
                oldObj.value(), updatedFields, _doc, immutableFields, driver->modOptions()));
        }

        // Prepare to write back the modified document
        WriteUnitOfWork wunit(getOpCtx());

        RecordId newRecordId;

        if (inPlace) {
            // Don't actually do the write if this is an explain.
            if (!request->isExplain()) {
                invariant(_collection);
                newObj = oldObj.value();
                const RecordData oldRec(oldObj.value().objdata(), oldObj.value().objsize());
                BSONObj idQuery = driver->makeOplogEntryQuery(newObj, request->isMulti());
                OplogUpdateEntryArgs args;
                args.ns = _collection->ns().ns();
                args.update = logObj;
                args.criteria = idQuery;
                args.fromMigrate = request->isFromMigration();
                StatusWith<RecordData> newRecStatus = _collection->updateDocumentWithDamages(
                    getOpCtx(),
                    recordId,
                    Snapshotted<RecordData>(oldObj.snapshotId(), oldRec),
                    source,
                    _damages,
                    &args);
                newObj = uassertStatusOK(std::move(newRecStatus)).releaseToBson();
            }

            newRecordId = recordId;
        } else {
            // The updates were not in place. Apply them through the file manager.

            newObj = _doc.getObject();
            uassert(17419,
                    str::stream() << "Resulting document after update is larger than "
                                  << BSONObjMaxUserSize,
                    newObj.objsize() <= BSONObjMaxUserSize);

            // Don't actually do the write if this is an explain.
            if (!request->isExplain()) {
                invariant(_collection);
                BSONObj idQuery = driver->makeOplogEntryQuery(newObj, request->isMulti());
                OplogUpdateEntryArgs args;
                args.ns = _collection->ns().ns();
                args.update = logObj;
                args.criteria = idQuery;
                args.fromMigrate = request->isFromMigration();
                StatusWith<RecordId> res = _collection->updateDocument(getOpCtx(),
                                                                       recordId,
                                                                       oldObj,
                                                                       newObj,
                                                                       true,
                                                                       driver->modsAffectIndices(),
                                                                       _params.opDebug,
                                                                       &args);
                uassertStatusOK(res.getStatus());
                newRecordId = res.getValue();
            }
        }

        invariant(oldObj.snapshotId() == getOpCtx()->recoveryUnit()->getSnapshotId());
        wunit.commit();

        // If the document moved, we might see it again in a collection scan (maybe it's
        // a document after our current document).
        //
        // If the document is indexed and the mod changes an indexed value, we might see
        // it again.  For an example, see the comment above near declaration of
        // updatedRecordIds.
        //
        // This must be done after the wunit commits so we are sure we won't be rolling back.
        if (_updatedRecordIds && (newRecordId != recordId || driver->modsAffectIndices())) {
            _updatedRecordIds->insert(newRecordId);
        }
    }

    // Only record doc modifications if they wrote (exclude no-ops). Explains get
    // recorded as if they wrote.
    if (docWasModified || request->isExplain()) {
        _specificStats.nModified++;
    }

    return newObj;
}

Status UpdateStage::applyUpdateOpsForInsert(OperationContext* txn,
                                            const CanonicalQuery* cq,
                                            const BSONObj& query,
                                            UpdateDriver* driver,
                                            mutablebson::Document* doc,
                                            bool isInternalRequest,
                                            const NamespaceString& ns,
                                            UpdateStats* stats,
                                            BSONObj* out) {
    // Since this is an insert (no docs found and upsert:true), we will be logging it
    // as an insert in the oplog. We don't need the driver's help to build the
    // oplog record, then. We also set the context of the update driver to the INSERT_CONTEXT.
    // Some mods may only work in that context (e.g. $setOnInsert).
    driver->setLogOp(false);
    driver->setContext(ModifierInterface::ExecInfo::INSERT_CONTEXT);

    const vector<FieldRef*>* immutablePaths = NULL;
    if (!isInternalRequest)
        immutablePaths = getImmutableFields(txn, ns);

    // The original document we compare changes to - immutable paths must not change
    BSONObj original;

    if (cq) {
        Status status = driver->populateDocumentWithQueryFields(*cq, immutablePaths, *doc);
        if (!status.isOK()) {
            return status;
        }

        if (driver->isDocReplacement())
            stats->fastmodinsert = true;
        original = doc->getObject();
    } else {
        fassert(17354, CanonicalQuery::isSimpleIdQuery(query));
        BSONElement idElt = query[idFieldName];
        original = idElt.wrap();
        fassert(17352, doc->root().appendElement(idElt));
    }

    // Apply the update modifications here.
    Status updateStatus = driver->update(StringData(), doc);
    if (!updateStatus.isOK()) {
        return Status(updateStatus.code(), updateStatus.reason(), 16836);
    }

    // Ensure _id exists and is first
    auto idAndFirstStatus = ensureIdFieldIsFirst(doc);
    if (idAndFirstStatus.code() == ErrorCodes::InvalidIdField) {  // _id field is missing
        idAndFirstStatus = addObjectIDIdField(doc);
    }

    if (!idAndFirstStatus.isOK()) {
        return idAndFirstStatus;
    }

    // Validate that the object replacement or modifiers resulted in a document
    // that contains all the immutable keys and can be stored if it isn't coming
    // from a migration or via replication.
    if (!isInternalRequest) {
        FieldRefSet noFields;
        // This will only validate the modified fields if not a replacement.
        Status validateStatus =
            validate(original, noFields, *doc, immutablePaths, driver->modOptions());
        if (!validateStatus.isOK()) {
            return validateStatus;
        }
    }

    BSONObj newObj = doc->getObject();
    if (newObj.objsize() > BSONObjMaxUserSize) {
        return Status(ErrorCodes::InvalidBSON,
                      str::stream() << "Document to upsert is larger than " << BSONObjMaxUserSize,
                      17420);
    }

    *out = newObj;
    return Status::OK();
}

void UpdateStage::doInsert() {
    _specificStats.inserted = true;

    const UpdateRequest* request = _params.request;
    bool isInternalRequest = !getOpCtx()->writesAreReplicated() || request->isFromMigration();

    // Reset the document we will be writing to.
    _doc.reset();

    BSONObj newObj;
    uassertStatusOK(applyUpdateOpsForInsert(getOpCtx(),
                                            _params.canonicalQuery,
                                            request->getQuery(),
                                            _params.driver,
                                            &_doc,
                                            isInternalRequest,
                                            request->getNamespaceString(),
                                            &_specificStats,
                                            &newObj));

    _specificStats.objInserted = newObj;

    // If this is an explain, bail out now without doing the insert.
    if (request->isExplain()) {
        return;
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
        WriteUnitOfWork wunit(getOpCtx());
        invariant(_collection);
        const bool enforceQuota = !request->isGod();
        uassertStatusOK(_collection->insertDocument(
            getOpCtx(), newObj, _params.opDebug, enforceQuota, request->isFromMigration()));

        // Technically, we should save/restore state here, but since we are going to return
        // immediately after, it would just be wasted work.
        wunit.commit();
    }
    MONGO_WRITE_CONFLICT_RETRY_LOOP_END(getOpCtx(), "upsert", _collection->ns().ns());
}

bool UpdateStage::doneUpdating() {
    // We're done updating if either the child has no more results to give us, or we've
    // already gotten a result back and we're not a multi-update.
    return _idRetrying == WorkingSet::INVALID_ID && _idReturning == WorkingSet::INVALID_ID &&
        (child()->isEOF() || (_specificStats.nMatched > 0 && !_params.request->isMulti()));
}

bool UpdateStage::needInsert() {
    // We need to insert if
    //  1) we haven't inserted already,
    //  2) the child stage returned zero matches, and
    //  3) the user asked for an upsert.
    return !_specificStats.inserted && _specificStats.nMatched == 0 && _params.request->isUpsert();
}

bool UpdateStage::isEOF() {
    return doneUpdating() && !needInsert();
}

PlanStage::StageState UpdateStage::doWork(WorkingSetID* out) {
    if (isEOF()) {
        return PlanStage::IS_EOF;
    }

    if (doneUpdating()) {
        // Even if we're done updating, we may have some inserting left to do.
        if (needInsert()) {
            // TODO we may want to handle WriteConflictException here. Currently we bounce it
            // out to a higher level since if this WCEs it is likely that we raced with another
            // upsert that may have matched our query, and therefore this may need to perform an
            // update rather than an insert. Bouncing to the higher level allows restarting the
            // query in this case.
            doInsert();

            invariant(isEOF());
            if (_params.request->shouldReturnNewDocs()) {
                // Want to return the document we just inserted, create it as a WorkingSetMember
                // so that we can return it.
                BSONObj newObj = _specificStats.objInserted;
                *out = _ws->allocate();
                WorkingSetMember* member = _ws->get(*out);
                member->obj = Snapshotted<BSONObj>(getOpCtx()->recoveryUnit()->getSnapshotId(),
                                                   newObj.getOwned());
                member->transitionToOwnedObj();
                return PlanStage::ADVANCED;
            }
        }

        // At this point either we're done updating and there was no insert to do,
        // or we're done updating and we're done inserting. Either way, we're EOF.
        invariant(isEOF());
        return PlanStage::IS_EOF;
    }

    // If we're here, then we still have to ask for results from the child and apply
    // updates to them. We should only get here if the collection exists.
    invariant(_collection);

    // It is possible that after an update was applied, a WriteConflictException
    // occurred and prevented us from returning ADVANCED with the requested version
    // of the document.
    if (_idReturning != WorkingSet::INVALID_ID) {
        // We should only get here if we were trying to return something before.
        invariant(_params.request->shouldReturnAnyDocs());

        WorkingSetMember* member = _ws->get(_idReturning);
        invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

        *out = _idReturning;
        _idReturning = WorkingSet::INVALID_ID;
        return PlanStage::ADVANCED;
    }

    // Either retry the last WSM we worked on or get a new one from our child.
    WorkingSetID id;
    StageState status;
    if (_idRetrying == WorkingSet::INVALID_ID) {
        status = child()->work(&id);
    } else {
        status = ADVANCED;
        id = _idRetrying;
        _idRetrying = WorkingSet::INVALID_ID;
    }

    if (PlanStage::ADVANCED == status) {
        // Need to get these things from the result returned by the child.
        RecordId recordId;

        WorkingSetMember* member = _ws->get(id);

        // We want to free this member when we return, unless we need to retry updating or returning
        // it.
        ScopeGuard memberFreer = MakeGuard(&WorkingSet::free, _ws, id);

        if (!member->hasRecordId()) {
            // We expect to be here because of an invalidation causing a force-fetch.
            ++_specificStats.nInvalidateSkips;
            return PlanStage::NEED_TIME;
        }
        recordId = member->recordId;

        // Updates can't have projections. This means that covering analysis will always add
        // a fetch. We should always get fetched data, and never just key data.
        invariant(member->hasObj());

        // We fill this with the new RecordIds of moved doc so we don't double-update.
        if (_updatedRecordIds && _updatedRecordIds->count(recordId) > 0) {
            // Found a RecordId that refers to a document we had already updated. Note that
            // we can never remove from _updatedRecordIds because updates by other clients
            // could cause us to encounter a document again later.
            return PlanStage::NEED_TIME;
        }

        bool docStillMatches;
        try {
            docStillMatches = write_stage_common::ensureStillMatches(
                _collection, getOpCtx(), _ws, id, _params.canonicalQuery);
        } catch (const WriteConflictException& wce) {
            // There was a problem trying to detect if the document still exists, so retry.
            memberFreer.Dismiss();
            return prepareToRetryWSM(id, out);
        }

        if (!docStillMatches) {
            // Either the document has been deleted, or it has been updated such that it no longer
            // matches the predicate.
            if (shouldRestartUpdateIfNoLongerMatches(_params)) {
                throw WriteConflictException();
            }
            return PlanStage::NEED_TIME;
        }

        // Ensure that the BSONObj underlying the WorkingSetMember is owned because saveState()
        // is allowed to free the memory.
        member->makeObjOwnedIfNeeded();

        // Save state before making changes
        WorkingSetCommon::prepareForSnapshotChange(_ws);
        try {
            child()->saveState();
        } catch (const WriteConflictException& wce) {
            std::terminate();
        }

        // If we care about the pre-updated version of the doc, save it out here.
        BSONObj oldObj;
        if (_params.request->shouldReturnOldDocs()) {
            oldObj = member->obj.value().getOwned();
        }

        BSONObj newObj;
        try {
            // Do the update, get us the new version of the doc.
            newObj = transformAndUpdate(member->obj, recordId);
        } catch (const WriteConflictException& wce) {
            memberFreer.Dismiss();  // Keep this member around so we can retry updating it.
            return prepareToRetryWSM(id, out);
        }

        // Set member's obj to be the doc we want to return.
        if (_params.request->shouldReturnAnyDocs()) {
            if (_params.request->shouldReturnNewDocs()) {
                member->obj = Snapshotted<BSONObj>(getOpCtx()->recoveryUnit()->getSnapshotId(),
                                                   newObj.getOwned());
            } else {
                invariant(_params.request->shouldReturnOldDocs());
                member->obj.setValue(oldObj);
            }
            member->recordId = RecordId();
            member->transitionToOwnedObj();
        }

        // This should be after transformAndUpdate to make sure we actually updated this doc.
        ++_specificStats.nMatched;

        // Restore state after modification

        // As restoreState may restore (recreate) cursors, make sure to restore the
        // state outside of the WritUnitOfWork.
        try {
            child()->restoreState();
        } catch (const WriteConflictException& wce) {
            // Note we don't need to retry updating anything in this case since the update
            // already was committed. However, we still need to return the updated document
            // (if it was requested).
            if (_params.request->shouldReturnAnyDocs()) {
                // member->obj should refer to the document we want to return.
                invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

                _idReturning = id;
                // Keep this member around so that we can return it on the next work() call.
                memberFreer.Dismiss();
            }
            *out = WorkingSet::INVALID_ID;
            return NEED_YIELD;
        }

        if (_params.request->shouldReturnAnyDocs()) {
            // member->obj should refer to the document we want to return.
            invariant(member->getState() == WorkingSetMember::OWNED_OBJ);

            memberFreer.Dismiss();  // Keep this member around so we can return it.
            *out = id;
            return PlanStage::ADVANCED;
        }

        return PlanStage::NEED_TIME;
    } else if (PlanStage::IS_EOF == status) {
        // The child is out of results, but we might not be done yet because we still might
        // have to do an insert.
        return PlanStage::NEED_TIME;
    } else if (PlanStage::FAILURE == status) {
        *out = id;
        // If a stage fails, it may create a status WSM to indicate why it failed, in which case
        // 'id' is valid.  If ID is invalid, we create our own error message.
        if (WorkingSet::INVALID_ID == id) {
            const std::string errmsg = "update stage failed to read in results from child";
            *out = WorkingSetCommon::allocateStatusMember(
                _ws, Status(ErrorCodes::InternalError, errmsg));
            return PlanStage::FAILURE;
        }
        return status;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

Status UpdateStage::restoreUpdateState() {
    const UpdateRequest& request = *_params.request;
    const NamespaceString& nsString(request.getNamespaceString());

    // We may have stepped down during the yield.
    bool userInitiatedWritesAndNotPrimary = getOpCtx()->writesAreReplicated() &&
        !repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(nsString);

    if (userInitiatedWritesAndNotPrimary) {
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Demoted from primary while performing update on "
                                    << nsString.ns());
    }

    if (request.getLifecycle()) {
        UpdateLifecycle* lifecycle = request.getLifecycle();
        lifecycle->setCollection(_collection);

        if (!lifecycle->canContinue()) {
            return Status(ErrorCodes::IllegalOperation,
                          "Update aborted due to invalid state transitions after yield.",
                          17270);
        }

        _params.driver->refreshIndexKeys(lifecycle->getIndexKeys(getOpCtx()));
    }

    return Status::OK();
}

void UpdateStage::doRestoreState() {
    uassertStatusOK(restoreUpdateState());
}

unique_ptr<PlanStageStats> UpdateStage::getStats() {
    _commonStats.isEOF = isEOF();
    unique_ptr<PlanStageStats> ret = make_unique<PlanStageStats>(_commonStats, STAGE_UPDATE);
    ret->specific = make_unique<UpdateStats>(_specificStats);
    ret->children.emplace_back(child()->getStats());
    return ret;
}

const SpecificStats* UpdateStage::getSpecificStats() const {
    return &_specificStats;
}

const UpdateStats* UpdateStage::getUpdateStats(const PlanExecutor* exec) {
    invariant(exec->getRootStage()->isEOF());
    invariant(exec->getRootStage()->stageType() == STAGE_UPDATE);
    UpdateStage* updateStage = static_cast<UpdateStage*>(exec->getRootStage());
    return static_cast<const UpdateStats*>(updateStage->getSpecificStats());
}

void UpdateStage::recordUpdateStatsInOpDebug(const UpdateStats* updateStats, OpDebug* opDebug) {
    invariant(opDebug);
    opDebug->nMatched = updateStats->nMatched;
    opDebug->nModified = updateStats->nModified;
    opDebug->upsert = updateStats->inserted;
    opDebug->fastmodinsert = updateStats->fastmodinsert;
}

UpdateResult UpdateStage::makeUpdateResult(const UpdateStats* updateStats) {
    return UpdateResult(updateStats->nMatched > 0 /* Did we update at least one obj? */,
                        !updateStats->isDocReplacement /* $mod or obj replacement */,
                        updateStats->nModified /* number of modified docs, no no-ops */,
                        updateStats->nMatched /* # of docs matched/updated, even no-ops */,
                        updateStats->objInserted);
};

PlanStage::StageState UpdateStage::prepareToRetryWSM(WorkingSetID idToRetry, WorkingSetID* out) {
    _idRetrying = idToRetry;
    *out = WorkingSet::INVALID_ID;
    return NEED_YIELD;
}

}  // namespace mongo
