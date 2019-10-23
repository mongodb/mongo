/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/db/exec/projection.h"

#include <boost/optional.hpp>
#include <memory>

#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/scoped_timer.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/record_id.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {

static const char* kIdField = "_id";

namespace {

void transitionMemberToOwnedObj(Document&& doc, WorkingSetMember* member) {
    member->keyData.clear();
    member->recordId = {};
    member->doc = {{}, std::move(doc)};
    member->transitionToOwnedObj();
}

void transitionMemberToOwnedObj(const BSONObj& bo, WorkingSetMember* member) {
    transitionMemberToOwnedObj(Document{bo}, member);
}

/**
 * Moves document metadata fields from the WSM into the given document 'doc', and returns the same
 * document but with populated metadata.
 */
auto attachMetadataToDocument(Document&& doc, WorkingSetMember* member) {
    MutableDocument md{std::move(doc)};
    md.setMetadata(member->releaseMetadata());
    return md.freeze();
}

/**
 * Moves document metadata fields from the document 'doc' into the WSM, and returns the same
 * document but without metadata.
 */
auto attachMetadataToWorkingSetMember(Document&& doc, WorkingSetMember* member) {
    MutableDocument md{std::move(doc)};
    member->setMetadata(md.releaseMetadata());
    return md.freeze();
}

/**
 * Given an index key 'dehyratedKey' with no field names, returns a new Document representing the
 * index key after adding field names according to 'keyPattern'.
 *
 * For example, given:
 *    - the 'keyPatern' of {'a.b': 1, c: 1}
 *    - the 'dehydratedKey' of {'': 'abc', '': 10}
 *
 * The resulting document will be: {a: {b: 'abc'}, c: 10}
 */
auto rehydrateIndexKey(const BSONObj& keyPattern, const BSONObj& dehydratedKey) {
    MutableDocument md;
    BSONObjIterator keyIter{keyPattern};
    BSONObjIterator valueIter{dehydratedKey};

    while (keyIter.more() && valueIter.more()) {
        auto fieldName = keyIter.next().fieldNameStringData();
        auto value = valueIter.next();

        // Skip the $** index virtual field, as it's not part of the actual index key.
        if (fieldName == "$_path") {
            continue;
        }

        md.setNestedField(fieldName, Value{value});
    }

    invariant(!keyIter.more());
    invariant(!valueIter.more());

    return md.freeze();
}
}  // namespace

ProjectionStage::ProjectionStage(OperationContext* opCtx,
                                 const BSONObj& projObj,
                                 WorkingSet* ws,
                                 std::unique_ptr<PlanStage> child,
                                 const char* stageType)
    : PlanStage(opCtx, std::move(child), stageType), _projObj(projObj), _ws(*ws) {}

// static
void ProjectionStage::getSimpleInclusionFields(const BSONObj& projObj, FieldSet* includedFields) {
    // The _id is included by default.
    bool includeId = true;

    // Figure out what fields are in the projection. We could eventually do this using the
    // Projection AST.
    BSONObjIterator projObjIt(projObj);
    while (projObjIt.more()) {
        BSONElement elt = projObjIt.next();
        // Must deal with the _id case separately as there is an implicit _id: 1 in the
        // projection.
        if ((elt.fieldNameStringData() == kIdField) && !elt.trueValue()) {
            includeId = false;
            continue;
        }
        (*includedFields)[elt.fieldNameStringData()] = true;
    }

    if (includeId) {
        (*includedFields)[kIdField] = true;
    }
}

bool ProjectionStage::isEOF() {
    return child()->isEOF();
}

PlanStage::StageState ProjectionStage::doWork(WorkingSetID* out) {
    WorkingSetID id = WorkingSet::INVALID_ID;
    StageState status = child()->work(&id);

    // Note that we don't do the normal if isEOF() return EOF thing here.  Our child might be a
    // tailable cursor and isEOF() would be true even if it had more data...
    if (PlanStage::ADVANCED == status) {
        WorkingSetMember* member = _ws.get(id);
        // Punt to our specific projection impl.
        Status projStatus = transform(member);
        if (!projStatus.isOK()) {
            warning() << "Couldn't execute projection, status = " << redact(projStatus);
            *out = WorkingSetCommon::allocateStatusMember(&_ws, projStatus);
            return PlanStage::FAILURE;
        }

        *out = id;
    } else if (PlanStage::FAILURE == status) {
        // The stage which produces a failure is responsible for allocating a working set member
        // with error details.
        invariant(WorkingSet::INVALID_ID != id);
        *out = id;
    } else if (PlanStage::NEED_YIELD == status) {
        *out = id;
    }

    return status;
}

std::unique_ptr<PlanStageStats> ProjectionStage::getStats() {
    _commonStats.isEOF = isEOF();
    auto ret = std::make_unique<PlanStageStats>(_commonStats, stageType());

    auto projStats = std::make_unique<ProjectionStats>(_specificStats);
    projStats->projObj = _projObj;
    ret->specific = std::move(projStats);

    ret->children.emplace_back(child()->getStats());
    return ret;
}

ProjectionStageDefault::ProjectionStageDefault(boost::intrusive_ptr<ExpressionContext> expCtx,
                                               const BSONObj& projObj,
                                               const projection_ast::Projection* projection,
                                               WorkingSet* ws,
                                               std::unique_ptr<PlanStage> child)
    : ProjectionStage{expCtx->opCtx, projObj, ws, std::move(child), "PROJECTION_DEFAULT"},
      _wantRecordId{projection->metadataDeps()[DocumentMetadataFields::kRecordId]},
      _projectType{projection->type()},
      _executor{projection_executor::buildProjectionExecutor(expCtx, projection, {})} {}

Status ProjectionStageDefault::transform(WorkingSetMember* member) const {
    Document input;

    // Most metadata should have already been stored within the WSM when we project out a document.
    // The recordId metadata is different though, because it's a fundamental part of the WSM and
    // we store it within the WSM itself rather than WSM metadata, so we need to transfer it into
    // the metadata object if the projection has a recordId $meta expression.
    if (_wantRecordId && !member->metadata().hasRecordId()) {
        member->metadata().setRecordId(member->recordId);
    }

    if (member->hasObj()) {
        input = std::move(member->doc.value());
    } else {
        // We have a covered projection, which is only supported in inclusion mode.
        invariant(_projectType == projection_ast::ProjectType::kInclusion);
        // We're pulling data from an index key, so there must be exactly one key entry in the WSM
        // as the planner guarantees that it will never generate a covered plan in the case of index
        // intersection.
        invariant(member->keyData.size() == 1);

        // For covered projection we will rehydrate in index key into a Document and then pass it
        // through the projection executor to include only required fields, including metadata
        // fields.
        input = rehydrateIndexKey(member->keyData[0].indexKeyPattern, member->keyData[0].keyData);
    }

    // Before applying the projection we will move document metadata from the WSM into the document
    // itself, in case the projection contains $meta expressions and needs this data, and will move
    // it back to the WSM once the projection has been applied.
    auto projected = attachMetadataToWorkingSetMember(
        _executor->applyTransformation(attachMetadataToDocument(std::move(input), member)), member);
    // An exclusion projection can return an unowned object since the output document is
    // constructed from the input one backed by BSON which is owned by the storage system, so we
    // need to  make sure we transition an owned document.
    transitionMemberToOwnedObj(projected.getOwned(), member);

    return Status::OK();
}

ProjectionStageCovered::ProjectionStageCovered(OperationContext* opCtx,
                                               const BSONObj& projObj,
                                               WorkingSet* ws,
                                               std::unique_ptr<PlanStage> child,
                                               const BSONObj& coveredKeyObj)
    : ProjectionStage(opCtx, projObj, ws, std::move(child), "PROJECTION_COVERED"),
      _coveredKeyObj(coveredKeyObj) {
    invariant(projObjHasOwnedData());
    // Figure out what fields are in the projection.
    getSimpleInclusionFields(_projObj, &_includedFields);

    // If we're pulling data out of one index we can pre-compute the indices of the fields
    // in the key that we pull data from and avoid looking up the field name each time.

    // Sanity-check.
    invariant(_coveredKeyObj.isOwned());

    BSONObjIterator kpIt(_coveredKeyObj);
    while (kpIt.more()) {
        BSONElement elt = kpIt.next();
        auto fieldIt = _includedFields.find(elt.fieldNameStringData());
        if (_includedFields.end() == fieldIt) {
            // Push an unused value on the back to keep _includeKey and _keyFieldNames
            // in sync.
            _keyFieldNames.push_back(StringData());
            _includeKey.push_back(false);
        } else {
            // If we are including this key field store its field name.
            _keyFieldNames.push_back(fieldIt->first);
            _includeKey.push_back(true);
        }
    }
}

Status ProjectionStageCovered::transform(WorkingSetMember* member) const {
    BSONObjBuilder bob;

    // We're pulling data out of the key.
    invariant(1 == member->keyData.size());
    size_t keyIndex = 0;

    // Look at every key element...
    BSONObjIterator keyIterator(member->keyData[0].keyData);
    while (keyIterator.more()) {
        BSONElement elt = keyIterator.next();
        // If we're supposed to include it...
        if (_includeKey[keyIndex]) {
            // Do so.
            bob.appendAs(elt, _keyFieldNames[keyIndex]);
        }
        ++keyIndex;
    }
    transitionMemberToOwnedObj(bob.obj(), member);
    return Status::OK();
}

ProjectionStageSimple::ProjectionStageSimple(OperationContext* opCtx,
                                             const BSONObj& projObj,
                                             WorkingSet* ws,
                                             std::unique_ptr<PlanStage> child)
    : ProjectionStage(opCtx, projObj, ws, std::move(child), "PROJECTION_SIMPLE") {
    invariant(projObjHasOwnedData());
    // Figure out what fields are in the projection.
    getSimpleInclusionFields(_projObj, &_includedFields);
}

Status ProjectionStageSimple::transform(WorkingSetMember* member) const {
    BSONObjBuilder bob;
    // SIMPLE_DOC implies that we expect an object so it's kind of redundant.
    // If we got here because of SIMPLE_DOC the planner shouldn't have messed up.
    invariant(member->hasObj());

    // Apply the SIMPLE_DOC projection.
    // Look at every field in the source document and see if we're including it.
    auto objToProject = member->doc.value().toBson();
    for (auto&& elt : objToProject) {
        auto fieldIt = _includedFields.find(elt.fieldNameStringData());
        if (_includedFields.end() != fieldIt) {
            bob.append(elt);
        }
    }

    transitionMemberToOwnedObj(bob.obj(), member);
    return Status::OK();
}

}  // namespace mongo
