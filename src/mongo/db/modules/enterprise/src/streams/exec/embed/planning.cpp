/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 */

#include "mongo/db/modules/enterprise/src/streams/exec/embed/planning.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/pipeline/expression.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/str.h"

namespace mongo::streams::embed {

namespace {

EmbeddingProvider providerFromConnectionType(StringData type) {
    if (type == "voyage"_sd)
        return EmbeddingProvider::kVoyage;
    if (type == "openai"_sd)
        return EmbeddingProvider::kOpenAi;
    if (type == "bedrock"_sd)
        return EmbeddingProvider::kBedrock;
    if (type == "azureOpenai"_sd)
        return EmbeddingProvider::kAzureOpenAi;
    if (type == "vertexAi"_sd)
        return EmbeddingProvider::kVertexAi;
    uasserted(ErrorCodes::BadValue,
              str::stream() << "$embed model.connectionName resolves to unsupported "
                               "connection type '"
                            << type << "'");
}

EmbeddingConnection resolveConnection(Context* ctx, StringData connectionName) {
    auto record = ctx->connectionRegistry()->lookup(connectionName);
    uassert(ErrorCodes::BadValue,
            str::stream() << "$embed model.connectionName '" << connectionName
                          << "' not found in connection registry",
            record.has_value());

    EmbeddingConnection out;
    out.provider = providerFromConnectionType(record->type);
    out.endpoint = record->endpoint;
    out.apiKey = record->apiKey;
    out.region = record->region;
    out.deployment = record->deployment;
    return out;
}

}  // namespace

std::unique_ptr<Operator> makeEmbedOperator(Context* ctx, const EmbedStageSpec& spec) {
    // Compile input expression. Stored as IDLAnyType in the IDL so we get the raw
    // BSON element and hand it to Expression::parseOperand — same flow as $project
    // expressions elsewhere in the engine.
    auto inputElt = spec.getInput().getElement();
    auto inputExpr = Expression::parseOperand(ctx->expCtx().get(),
                                              inputElt,
                                              ctx->expCtx()->variablesParseState);

    FieldPath into{spec.getInto()};
    uassert(ErrorCodes::BadValue, "$embed.into must not be empty", into.getPathLength() > 0);

    const auto& model = spec.getModel();
    auto conn = resolveConnection(ctx, model.getConnectionName());

    boost::optional<int> dims;
    if (auto d = model.getDimensions()) {
        uassert(ErrorCodes::BadValue, "$embed model.dimensions must be > 0", *d > 0);
        dims = *d;
    }

    int maxBatchSize = 96;
    Milliseconds maxWait{50};
    if (auto batch = spec.getBatch()) {
        maxBatchSize = batch->getMaxSize();
        maxWait = Milliseconds(batch->getMaxWaitMs());
    }

    bool cacheEnabled = false;
    int cacheMax = 10000;
    if (auto cache = spec.getCache()) {
        cacheEnabled = cache->getEnabled();
        cacheMax = cache->getMaxEntries();
    }

    return std::make_unique<EmbedOperator>(ctx,
                                           std::move(inputExpr),
                                           std::move(into),
                                           std::move(conn),
                                           std::string{model.getName()},
                                           dims,
                                           maxBatchSize,
                                           maxWait,
                                           cacheEnabled,
                                           cacheMax,
                                           spec.getOnError());
}

}  // namespace mongo::streams::embed
