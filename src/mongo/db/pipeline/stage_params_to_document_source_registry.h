/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/init.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <boost/intrusive_ptr.hpp>

namespace mongo {

using StageParamsToDocumentSourceFn = std::function<boost::intrusive_ptr<DocumentSource>(
    const std::unique_ptr<StageParams>&, const boost::intrusive_ptr<ExpressionContext>&)>;

/**
 * Register a function that builds a 'DocumentSource' from 'StageParams'.
 *
 * 'registrationName' is a unique name to give to the initializer function that does the
 * registration.
 *
 * TODO SERVER-114343: Remove stageName once parserMap no longer exists.
 * 'stageName' is the name of the stage (e.g., "$limit") used for validation that
 * the stage is not also registered in the old parserMap.
 *
 * 'stageParamsId' is a unique StageParams::Id that is assigned to the
 * StageParams class.
 *
 * 'stageParamsToDocumentSourceFn' is a function that accepts StageParams and
 * ExpressionContext, and returns a DocumentSource.
 */
#define REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING(                                    \
    registrationName, stageName, stageParamsId, stageParamsToDocumentSourceFn)               \
    namespace {                                                                              \
    MONGO_INITIALIZER_GENERAL(registerStageParamsToDocumentSourceMapping_##registrationName, \
                              ("BeginStageParamsToDocumentSourceRegistration"),              \
                              ("EndStageParamsToDocumentSourceRegistration"))                \
    (InitializerContext*) {                                                                  \
        registerStageParamsToDocumentSourceFn(                                               \
            stageName, stageParamsId, stageParamsToDocumentSourceFn);                        \
    }                                                                                        \
    }

/**
 * Registers StageParams with a function that builds a 'DocumentSource' from
 * 'StageParams'.
 *
 * DO NOT call this function directly. Instead, use the
 * REGISTER_STAGE_PARAMS_TO_DOCUMENT_SOURCE_MAPPING macro defined in this file.
 */
void registerStageParamsToDocumentSourceFn(StringData stageName,
                                           StageParams::Id stageParamsId,
                                           StageParamsToDocumentSourceFn fn);

/**
 * Create the corresponding 'DocumentSource' object for the given instance of
 * 'LiteParsedDocumentSource' using its associated 'StageParams'.
 *
 * TODO SERVER-114343: Remove optional return value once all stages are migrated.
 */
boost::optional<boost::intrusive_ptr<DocumentSource>> buildDocumentSource(
    const LiteParsedDocumentSource& liteParsed,
    const boost::intrusive_ptr<ExpressionContext>& expCtx);

}  // namespace mongo
