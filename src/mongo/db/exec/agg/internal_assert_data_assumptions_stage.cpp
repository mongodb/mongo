/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
#include "mongo/db/exec/agg/internal_assert_data_assumptions_stage.h"

#include "mongo/db/exec/agg/document_source_to_stage_registry.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_internal_assert_data_assumptions.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/str.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

boost::intrusive_ptr<exec::agg::Stage> documentSourceInternalAssertDataAssumptionsToStageFn(
    const boost::intrusive_ptr<DocumentSource>& documentSource) {
    auto* dsInternalAssertDataAssumptions =
        dynamic_cast<DocumentSourceInternalAssertDataAssumptions*>(documentSource.get());

    tassert(12508301,
            "expected 'DocumentSourceInternalAssertDataAssumptions' type",
            dsInternalAssertDataAssumptions);

    return make_intrusive<exec::agg::InternalAssertDataAssumptionsStage>(
        dsInternalAssertDataAssumptions->kStageName,
        dsInternalAssertDataAssumptions->getExpCtx(),
        dsInternalAssertDataAssumptions->getNonArrayPaths());
}

namespace exec::agg {

REGISTER_AGG_STAGE_MAPPING(internalAssertDataAssumptionsStage,
                           DocumentSourceInternalAssertDataAssumptions::id,
                           documentSourceInternalAssertDataAssumptionsToStageFn);

InternalAssertDataAssumptionsStage::InternalAssertDataAssumptionsStage(
    StringData stageName,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    std::set<FieldPath> nonArrayPaths)
    : Stage(stageName, expCtx), _nonArrayPaths(std::move(nonArrayPaths)) {}

GetNextResult InternalAssertDataAssumptionsStage::doGetNext() {
    auto nextResult = pSource->getNext();

    if (!nextResult.isAdvanced()) {
        return nextResult;
    }

    const Document& doc = nextResult.getDocument();

    // Validate that none of the specified fields contain arrays
    for (const auto& fieldPath : _nonArrayPaths) {
        Value fieldValue = doc.getNestedField(fieldPath);

        if (!fieldValue.missing() && fieldValue.isArray()) {
            // Found an array in a field that the dependency graph claimed could not be an array
            uasserted(12508302,
                      str::stream()
                          << "Dependency graph arrayness validation failed: field '"
                          << fieldPath.fullPath() << "' contains an array but canPathBeArray()"
                          << " returned false. Document: " << doc.toString()
                          << ". This indicates a bug in the dependency graph analysis.");
        }
    }

    return nextResult;
}

}  // namespace exec::agg
}  // namespace mongo
