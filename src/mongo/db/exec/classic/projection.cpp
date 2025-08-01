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

#include "mongo/db/exec/classic/projection.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"
#include "mongo/db/record_id.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <memory>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

void transitionMemberToOwnedObj(Document&& doc, WorkingSetMember* member) {
    member->keyData.clear();
    member->recordId = {};
    member->doc = {{}, std::move(doc)};
    member->transitionToOwnedObj();
}

void transitionMemberToOwnedObj(const BSONObj& bo, WorkingSetMember* member) {
    // Use the DocumentStorage that already exists on the WorkingSetMember's document
    // field if possible.
    MutableDocument md(std::move(member->doc.value()));
    md.reset(bo, false);
    transitionMemberToOwnedObj(md.freeze(), member);
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
        const auto& keyElt = keyIter.next();
        auto fieldName = keyElt.fieldNameStringData();
        auto value = valueIter.next();

        // Skip the $** index virtual field, as it's not part of the actual index key.
        if (fieldName == "$_path") {
            continue;
        }

        // Skip hashed index fields. Rehydrating of index keys is used for covered projections.
        // Rehydrating of hashed field value is pointless on its own. The query planner dependency
        // analysis should make sure that a covered projection can only be generated for non-hashed
        // fields.
        if (keyElt.type() == BSONType::string && keyElt.valueStringData() == IndexNames::HASHED) {
            continue;
        }

        md.setNestedField(fieldName, Value{value});
    }

    tassert(
        7241729, "must iterate through all field names specified in keyPattern", !keyIter.more());
    tassert(7241730, "must iterate through all index keys with no field names", !valueIter.more());

    return md.freeze();
}
}  // namespace

ProjectionStage::ProjectionStage(ExpressionContext* expCtx,
                                 const BSONObj& projObj,
                                 WorkingSet* ws,
                                 std::unique_ptr<PlanStage> child,
                                 const char* stageType)
    : PlanStage{expCtx, std::move(child), stageType},
      _projObj{expCtx->getExplain() ? boost::make_optional(projObj.getOwned()) : boost::none},
      _ws{*ws} {}

bool ProjectionStage::isEOF() const {
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
        transform(member);
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
    projStats->projObj = _projObj.value_or(BSONObj{});
    ret->specific = std::move(projStats);

    ret->children.emplace_back(child()->getStats());
    return ret;
}

ProjectionStageDefault::ProjectionStageDefault(boost::intrusive_ptr<ExpressionContext> expCtx,
                                               const BSONObj& projObj,
                                               const projection_ast::Projection* projection,
                                               WorkingSet* ws,
                                               std::unique_ptr<PlanStage> child)
    : ProjectionStage{expCtx.get(), projObj, ws, std::move(child), "PROJECTION_DEFAULT"},
      _requestedMetadata{projection->metadataDeps()},
      _projectType{projection->type()},
      _executor{projection_executor::buildProjectionExecutor(
          expCtx, projection, {}, projection_executor::kDefaultBuilderParams)} {}

void ProjectionStageDefault::transform(WorkingSetMember* member) const {
    Document input;

    // Most metadata should have already been stored within the WSM when we project out a document.
    // The recordId metadata is different though, because it's a fundamental part of the WSM and
    // we store it within the WSM itself rather than WSM metadata, so we need to transfer it into
    // the metadata object if the projection has a recordId $meta expression.
    if (_requestedMetadata[DocumentMetadataFields::kRecordId] &&
        !member->metadata().hasRecordId()) {
        member->metadata().setRecordId(member->recordId);
    }

    if (member->hasObj()) {
        input = std::move(member->doc.value());
    } else {
        // We have a covered projection, which is only supported in inclusion mode.
        tassert(7241731,
                "covered projections are only supported in inclusion mode",
                _projectType == projection_ast::ProjectType::kInclusion);
        // We're pulling data from an index key, so there must be exactly one key entry in the WSM
        // as the planner guarantees that it will never generate a covered plan in the case of index
        // intersection.
        tassert(7241732,
                "covered plan cannot be generated if there is an index intersection",
                member->keyData.size() == 1);

        // For covered projection we will rehydrate in index key into a Document and then pass it
        // through the projection executor to include only required fields, including metadata
        // fields.
        input = rehydrateIndexKey(member->keyData[0].indexKeyPattern, member->keyData[0].keyData);
    }

    // If the projection doesn't need any metadata, then we'll just apply the projection to the
    // input document. Otherwise, before applying the projection, we will move document metadata
    // from the WSM into the document itself, and will move it back to the WSM once the projection
    // has been applied.
    auto projected = _requestedMetadata.any()
        ? attachMetadataToWorkingSetMember(
              _executor->applyTransformation(attachMetadataToDocument(std::move(input), member)),
              member)
        : _executor->applyTransformation(input);

    // An exclusion projection can return an unowned object since the output document is
    // constructed from the input one backed by BSON which is owned by the storage system, so we
    // need to  make sure we transition an owned document.
    transitionMemberToOwnedObj(projected.getOwned(), member);
}

ProjectionStageCovered::ProjectionStageCovered(ExpressionContext* expCtx,
                                               const BSONObj& projObj,
                                               const projection_ast::Projection* projection,
                                               WorkingSet* ws,
                                               std::unique_ptr<PlanStage> child,
                                               const BSONObj& coveredKeyObj)
    : ProjectionStage{expCtx, projObj, ws, std::move(child), "PROJECTION_COVERED"},
      _coveredKeyObj{coveredKeyObj} {
    tassert(7241733,
            "covered projections must be simple and only consist of inclusions",
            projection->isSimple() && projection->isInclusionOnly());

    // If we're pulling data out of one index we can pre-compute the indices of the fields
    // in the key that we pull data from and avoid looking up the field name each time.

    // Sanity-check.
    invariant(_coveredKeyObj.isOwned());

    _includedFields = {projection->getRequiredFields().begin(),
                       projection->getRequiredFields().end()};
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
            _keyFieldNames.push_back(*fieldIt);
            _includeKey.push_back(true);
        }
    }
}

void ProjectionStageCovered::transform(WorkingSetMember* member) const {
    BSONObjBuilder bob;

    // We're pulling data out of the key.
    tassert(
        7241734, "covered projections must be covered by one index", 1 == member->keyData.size());
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
}

ProjectionStageSimple::ProjectionStageSimple(ExpressionContext* expCtx,
                                             const BSONObj& projObj,
                                             const projection_ast::Projection* projection,
                                             WorkingSet* ws,
                                             std::unique_ptr<PlanStage> child)
    : ProjectionStage{expCtx, projObj, ws, std::move(child), "PROJECTION_SIMPLE"},
      _projectType(projection->type()) {
    tassert(7241735,
            "the projection must be simple to create a simple projection stage",
            projection->isSimple());
    if (_projectType == projection_ast::ProjectType::kInclusion) {
        _fields = {projection->getRequiredFields().begin(), projection->getRequiredFields().end()};
    } else {
        _fields = {projection->getExcludedPaths().begin(), projection->getExcludedPaths().end()};
    }
}

template <typename Container>
BSONObj ProjectionStageSimple::transform(const BSONObj& doc,
                                         const Container& projFields,
                                         projection_ast::ProjectType projectType) {
    BSONObjBuilder bob;
    auto nFieldsLeft = projFields.size();

    if (projectType == projection_ast::ProjectType::kInclusion) {

        for (const auto& elt : doc) {
            if (projFields.count(elt.fieldNameStringData()) > 0) {
                bob.append(elt);
                if (--nFieldsLeft == 0) {
                    break;
                }
            }
        }
    } else {

        for (const auto& elt : doc) {
            if (nFieldsLeft == 0 || projFields.count(elt.fieldNameStringData()) == 0) {
                bob.append(elt);
            } else {
                --nFieldsLeft;
            }
        }
    }
    return bob.obj();
}
template BSONObj ProjectionStageSimple::transform<StringSet>(
    const BSONObj& doc, const StringSet& fields, projection_ast::ProjectType projectType);
template BSONObj ProjectionStageSimple::transform<OrderedPathSet>(
    const BSONObj& doc, const OrderedPathSet& fields, projection_ast::ProjectType projectType);

void ProjectionStageSimple::transform(WorkingSetMember* member) const {
    // SIMPLE_DOC implies that we expect an object so it's kind of redundant.
    // If we got here because of SIMPLE_DOC the planner shouldn't have messed up.
    tassert(7241736, "simple projections must have an object", member->hasObj());

    // Apply the SIMPLE_DOC projection: look at every top level field in the source document and
    // see if we should keep it.
    auto objToProject = member->doc.value().toBson();

    transitionMemberToOwnedObj(transform(objToProject, _fields, _projectType), member);
}

}  // namespace mongo
