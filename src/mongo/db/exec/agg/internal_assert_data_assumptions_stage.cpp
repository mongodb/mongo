// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
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
    std::string_view stageName,
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
