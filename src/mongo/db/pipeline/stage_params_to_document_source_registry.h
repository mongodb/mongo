// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/init.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo {

class DocumentSource;
class LiteParsedDocumentSource;
class ExpressionContext;

using StageParamsToDocumentSourceFn = std::function<std::list<boost::intrusive_ptr<DocumentSource>>(
    const std::unique_ptr<StageParams>&, const boost::intrusive_ptr<ExpressionContext>&)>;

/**
 * Register a function that builds a 'DocumentSource' from 'StageParams'.
 *
 * 'registrationName' is a unique name to give to the initializer function that does the
 * registration.
 *
 * 'stageParamsId' is a unique StageParams::Id that is assigned to the
 * StageParams class.
 *
 * 'stageParamsToDocumentSourceFn' is a function that accepts StageParams and
 * ExpressionContext, and returns a DocumentSource.
 */
#define REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(                                    \
    registrationName, stageParamsId, stageParamsToDocumentSourceFn)                          \
    namespace {                                                                              \
    MONGO_INITIALIZER_GENERAL(registerStageParamsToDocumentSourceMapping_##registrationName, \
                              ("BeginStageParamsToDocumentSourceRegistration"),              \
                              ("EndStageParamsToDocumentSourceRegistration"))                \
    (InitializerContext*) {                                                                  \
        registerStageParamsToDocumentSourceFn(stageParamsId, stageParamsToDocumentSourceFn); \
    }                                                                                        \
    }

/**
 * Registers StageParams with a function that builds a 'DocumentSource' from
 * 'StageParams'.
 *
 * DO NOT call this function directly. Instead, use the
 * REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING macro defined in this file.
 */
[[MONGO_MOD_PUBLIC]]  // Needed by enterprise hot backup registrations.
    void
    registerStageParamsToDocumentSourceFn(StageParams::Id stageParamsId,
                                          StageParamsToDocumentSourceFn fn);

/**
 * Create the corresponding 'DocumentSource' object for the given instance of
 * 'LiteParsedDocumentSource' using its associated 'StageParams'.
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildDocumentSource(
    const LiteParsedDocumentSource& liteParsed,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

/**
 * Create the corresponding 'DocumentSource' object directly from a pre-computed 'StageParams',
 * bypassing the LiteParsedDocumentSource::getStageParams() call. Used when StageParams have
 * already been collected (e.g., from a subpipeline stored in a parent stage's params).
 */
std::list<boost::intrusive_ptr<DocumentSource>> buildDocumentSource(
    const std::unique_ptr<StageParams>& stageParams,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

}  // namespace mongo
