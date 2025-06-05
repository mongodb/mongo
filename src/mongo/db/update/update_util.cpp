/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/update/update_util.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/algorithm.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/legacy_runtime_constants_gen.h"
#include "mongo/db/pipeline/variables.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/write_ops/write_ops_parsers.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <map>
#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace update {

const char idFieldName[] = "_id";
const FieldRef idFieldRef(idFieldName);

void addObjectIDIdField(mutablebson::Document* doc) {
    const auto idElem = doc->makeElementNewOID(idFieldName);
    uassert(17268, "Could not create new ObjectId '_id' field.", idElem.ok());
    uassertStatusOK(doc->root().pushFront(idElem));
}

void generateNewDocumentFromUpdateOp(OperationContext* opCtx,
                                     const FieldRefSet& immutablePaths,
                                     UpdateDriver* driver,
                                     mutablebson::Document& document) {
    // Use the UpdateModification from the original request to generate a new document by running
    // the update over the empty (except for fields extracted from the query) document. We do not
    // validate for storage until later, but we do ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(
        driver->update(opCtx, {}, &document, validateForStorage, immutablePaths, isInsert));
};

void generateNewDocumentFromSuppliedDoc(OperationContext* opCtx,
                                        const FieldRefSet& immutablePaths,
                                        const UpdateRequest* request,
                                        mutablebson::Document& document) {
    // We should never call this method unless the request has a set of update constants.
    invariant(request->shouldUpsertSuppliedDocument());
    invariant(request->getUpdateConstants());

    // Extract the supplied document from the constants and validate that it is an object.
    auto suppliedDocElt = request->getUpdateConstants()->getField("new"_sd);
    invariant(suppliedDocElt.type() == BSONType::object);
    auto suppliedDoc = suppliedDocElt.embeddedObject();

    // The supplied doc is functionally a replacement update. We need a new driver to apply it.
    UpdateDriver replacementDriver(nullptr);

    // Create a new replacement-style update from the supplied document.
    replacementDriver.parse(
        write_ops::UpdateModification(suppliedDoc, write_ops::UpdateModification::ReplacementTag{}),
        {});
    replacementDriver.setLogOp(false);
    replacementDriver.setBypassEmptyTsReplacement(
        static_cast<bool>(request->getBypassEmptyTsReplacement()));

    // We do not validate for storage, as we will validate the full document before inserting.
    // However, we ensure that no immutable fields are modified.
    const bool validateForStorage = false;
    const bool isInsert = true;
    uassertStatusOK(replacementDriver.update(
        opCtx, {}, &document, validateForStorage, immutablePaths, isInsert));
}

void produceDocumentForUpsert(OperationContext* opCtx,
                              const UpdateRequest* request,
                              UpdateDriver* driver,
                              const CanonicalQuery* canonicalQuery,
                              const FieldRefSet& immutablePaths,
                              mutablebson::Document& doc) {
    // Reset the document into which we will be writing.
    doc.reset();

    // First: populate the document's immutable paths with equality predicate values from the query,
    // if available. This generates the pre-image document that we will run the update against.
    if (auto* cq = canonicalQuery) {
        uassertStatusOK(driver->populateDocumentWithQueryFields(
            *cq->getPrimaryMatchExpression(), immutablePaths, doc));
    } else {
        fassert(17354, isSimpleIdQuery(request->getQuery()));
        // IDHACK path allows for queries of the shape {_id: 123} and {_id: {$eq: 123}}. Neither
        // case will have generated a CanonicalQuery earlier, so we have to figure out which value
        // should be in the created document here, since we cannot insert a document that looks like
        // {_id: {$eq: 123}}.
        const BSONElement& idVal = request->getQuery()[idFieldName];
        if (idVal.isABSONObj() && idVal.Obj().hasField("$eq")) {
            // We append an element of the shape {_id: 123}.
            mutablebson::Element newElement =
                doc.makeElementWithNewFieldName(idFieldName, idVal["$eq"]);
            fassert(9248800, newElement.ok());
            fassert(9248803, doc.root().pushBack(newElement));
        } else {
            fassert(17352, doc.root().appendElement(idVal));
        }
    }

    // Second: run the appropriate document generation strategy over the document to generate the
    // post-image. If the update operation modifies any of the immutable paths, this will throw.
    if (request->shouldUpsertSuppliedDocument()) {
        generateNewDocumentFromSuppliedDoc(opCtx, immutablePaths, request, doc);
    } else {
        generateNewDocumentFromUpdateOp(opCtx, immutablePaths, driver, doc);
    }

    // Third: ensure _id is first if it exists, and generate a new OID otherwise.
    ensureIdFieldIsFirst(&doc, true);
}

void assertPathsNotArray(const mutablebson::Document& document, const FieldRefSet& paths) {
    for (const auto& path : paths) {
        auto elem = document.root();
        // If any path component does not exist, we stop checking for arrays along the path.
        for (size_t i = 0; elem.ok() && i < (*path).numParts(); ++i) {
            elem = elem[(*path).getPart(i)];
            uassert(ErrorCodes::NotSingleValueField,
                    str::stream() << "After applying the update to the document, the field '"
                                  << (*path).dottedField()
                                  << "' was found to be an array or array descendant.",
                    !elem.ok() || elem.getType() != BSONType::array);
        }
    }
}

void ensureIdFieldIsFirst(mutablebson::Document* doc, bool generateOIDIfMissing) {
    mutablebson::Element idElem = mutablebson::findFirstChildNamed(doc->root(), idFieldName);

    // If the document has no _id and the caller has requested that we generate one, do so.
    if (!idElem.ok() && generateOIDIfMissing) {
        addObjectIDIdField(doc);
    } else if (idElem.ok() && idElem.leftSibling().ok()) {
        // If the document does have an _id but it is not the first element, move it to the front.
        uassertStatusOK(idElem.remove());
        uassertStatusOK(doc->root().pushFront(idElem));
    }
}

void makeUpdateRequest(OperationContext* opCtx,
                       const write_ops::FindAndModifyCommandRequest& request,
                       boost::optional<ExplainOptions::Verbosity> explain,
                       UpdateRequest* requestOut) {
    requestOut->setQuery(request.getQuery());
    requestOut->setProj(request.getFields().value_or(BSONObj()));
    invariant(request.getUpdate());
    requestOut->setUpdateModification(*request.getUpdate());
    requestOut->setLegacyRuntimeConstants(
        request.getLegacyRuntimeConstants().value_or(Variables::generateRuntimeConstants(opCtx)));
    requestOut->setLetParameters(request.getLet());
    requestOut->setSort(request.getSort().value_or(BSONObj()));
    requestOut->setHint(request.getHint());
    requestOut->setCollation(request.getCollation().value_or(BSONObj()));
    requestOut->setArrayFilters(request.getArrayFilters().value_or(std::vector<BSONObj>()));
    requestOut->setUpsert(request.getUpsert().value_or(false));
    requestOut->setReturnDocs((request.getNew().value_or(false)) ? UpdateRequest::RETURN_NEW
                                                                 : UpdateRequest::RETURN_OLD);
    requestOut->setMulti(false);
    requestOut->setExplain(explain);

    requestOut->setYieldPolicy(PlanYieldPolicy::YieldPolicy::YIELD_AUTO);
    requestOut->setIsTimeseriesNamespace(request.getIsTimeseriesNamespace());
    requestOut->setBypassEmptyTsReplacement(request.getBypassEmptyTsReplacement());
}

}  // namespace update
}  // namespace mongo
