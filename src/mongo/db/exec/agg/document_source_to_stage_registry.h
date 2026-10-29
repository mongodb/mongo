// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/init.h"
#include "mongo/db/exec/agg/stage.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <absl/container/inlined_vector.h>
#include <boost/intrusive_ptr.hpp>

namespace mongo {
namespace exec {
namespace agg {

/**
 * Register a function that builds an aggregation 'Stage' from a 'DocumentSource'.
 *
 * 'name' is a unique name to give to the initializer function that does the
 * registration.
 *
 * 'documentSourceId' is a unique DocumentSource::Id that is assigned to the
 * DocumentSource class.
 *
 * 'documentSourceToStageFn' is a function that accepts pointer to the DocumentSource
 * and returns pointer to the Stage.
 */
#define REGISTER_AGG_STAGE_MAPPING(name, documentSourceId, documentSourceToStageFn) \
    namespace {                                                                     \
    MONGO_INITIALIZER_GENERAL(registerAggStageMapping_##name,                       \
                              ("BeginDocumentSourceStageRegistration"),             \
                              ("EndDocumentSourceStageRegistration"))               \
    (InitializerContext*) {                                                         \
        registerDocumentSourceToStageFn(documentSourceId, documentSourceToStageFn); \
    }                                                                               \
    }

using DocumentSourceToStageFn =
    std::function<StagePtr(const boost::intrusive_ptr<DocumentSource>&)>;

/**
 * Like REGISTER_AGG_STAGE_MAPPING but for DocumentSources that expand to more than one
 * exec::agg Stage. 'documentSourceToStagesFn' must have signature:
 *   StageExpansion(const boost::intrusive_ptr<DocumentSource>&)
 */
#define REGISTER_AGG_STAGES_MAPPING(name, documentSourceId, documentSourceToStagesFn) \
    namespace {                                                                       \
    MONGO_INITIALIZER_GENERAL(registerAggStagesMapping_##name,                        \
                              ("BeginDocumentSourceStageRegistration"),               \
                              ("EndDocumentSourceStageRegistration"))                 \
    (InitializerContext*) {                                                           \
        registerDocumentSourceToStagesFn(documentSourceId, documentSourceToStagesFn); \
    }                                                                                 \
    }

/**
 * Holds the exec::agg stages produced by a single DocumentSource during translation.
 * Inline storage for one element covers the common 1:1 case with no heap allocation.
 */
using StageExpansion = absl::InlinedVector<StagePtr, 1>;

using DocumentSourceToStagesFn =
    std::function<StageExpansion(const boost::intrusive_ptr<DocumentSource>&)>;

/**
 * Registers a DocumentSource with a function that builds one or more exec::agg stages.
 * Used by REGISTER_AGG_STAGES_MAPPING. Do not call directly.
 */
void registerDocumentSourceToStagesFn(DocumentSource::Id dsid, DocumentSourceToStagesFn fn);

/**
 * Creates the exec::agg stages for the given DocumentSource.
 * Returns a StageExpansion with one element for 1:1 mappings and N elements for 1:N mappings.
 */
StageExpansion buildStages(const boost::intrusive_ptr<DocumentSource>& ds);

// Needed so buildPipeline can wire expansion stages without being a friend of Stage.
void stitchStage(Stage& stage, Stage* prior);

/**
 * Registers a DocumentSource with a function that builds an aggregation 'Stage' from
 * a 'DocumentSource'.
 *
 * DO NOT call this function directly. Instead, use the REGISTER_AGG_STAGE_MAPPING
 * macro defined in this file.
 *
 * TODO SERVER-112775: Remove 'server_backup_restore' dependency on the 'REGISTER_AGG_STAGE_MAPPING'
 * macro that references this function.
 * TODO SERVER-112776: Remove 'data_movement' dependency on the 'REGISTER_AGG_STAGE_MAPPING' macro
 * that references this function.
 * TODO SERVER-112777: Remove 'atlas_streams' dependency on the 'REGISTER_AGG_STAGE_MAPPING' macro
 * that references this function.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void registerDocumentSourceToStageFn(DocumentSource::Id dsid,
                                                                     DocumentSourceToStageFn fn);

/**
 * Create the corresponding 'Stage' object for the given instance of 'DocumentSource'.
 * TODO SERVER-112775: Remove 'server_backup_restore' dependency on this function.
 * TODO SERVER-112777: Remove 'atlas_streams' dependency on this function.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] StagePtr buildStage(const boost::intrusive_ptr<DocumentSource>& ds);

/**
 * Create the corresponding 'Stage' object for the given instance of 'DocumentSource'. Attach the
 * given 'sourceStage' as the source for the newly created 'Stage' object.
 */
StagePtr buildStageAndStitch(const boost::intrusive_ptr<DocumentSource>& ds,
                             const StagePtr& sourceStage);

}  // namespace agg
}  // namespace exec
}  // namespace mongo
