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
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/stage_params.h"
#include "mongo/util/modules.h"

namespace mongo {

class MONGO_MOD_PUBLIC LiteParsedDesugarer {
public:
    using StageExpander =
        std::function<size_t(LiteParsedPipeline*, size_t index, LiteParsedDocumentSource&)>;

    // Desugars the LiteParsedPipeline and returns whether the pipeline was modified or not.
    static bool desugar(LiteParsedPipeline* pipeline);

    static void registerStageExpander(StageParams::Id id, StageExpander stageExpander) {
        _stageExpanders[id] = std::move(stageExpander);
    }

private:
    // Associate a stage expander for each stage that should desugar.
    // NOTE: this map is *not* thread safe. LiteParsedDocumentSources should register their
    // stageExpander using MONGO_INITIALIZER to ensure thread safety. See
    // DocumentSourceExtension::LiteParsedExpandable for an example.
    inline static stdx::unordered_map<StageParams::Id, StageExpander> _stageExpanders{};
};

}  // namespace mongo
