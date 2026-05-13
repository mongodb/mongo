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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/lite_parsed_document_source.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"

#include <vector>

namespace mongo {

/**
 * Wraps a LiteParsedPipeline together with the BSON that the pipeline's stages were parsed from.
 * Use this whenever the backing BSON is a temporary that would otherwise die before the parsed
 * stages do — most commonly, subpipelines constructed inside a parent stage's lite parser.
 *
 * '_ownedStages' is declared before '_pipeline'. C++ initializes members in declaration order,
 * so the owned BSON copies exist before any BSONElement inside '_pipeline' references them.
 */
class OwnedLiteParsedPipeline {
public:
    OwnedLiteParsedPipeline(const NamespaceString& nss,
                            const std::vector<BSONObj>& pipelineStages,
                            const LiteParserOptions& options = {});

    OwnedLiteParsedPipeline(OwnedLiteParsedPipeline&&) noexcept = default;
    OwnedLiteParsedPipeline& operator=(OwnedLiteParsedPipeline&&) noexcept = default;

    OwnedLiteParsedPipeline(const OwnedLiteParsedPipeline& other);
    // Copy-assignment is not provided; construct a new OwnedLiteParsedPipeline explicitly.
    OwnedLiteParsedPipeline& operator=(const OwnedLiteParsedPipeline&) = delete;

    // Prefer operator-> or operator*. This is for when a caller has an OwnedLiteParsedPipeline
    // surrounded in a pointer, such that the caller doesn't need to write something like
    // (*_parsedPipeline).operator->() or &**_parsedPipeline.
    LiteParsedPipeline& pipeline() {
        return _pipeline;
    }

    // Prefer operator-> or operator*. This is for when a caller has an OwnedLiteParsedPipeline
    // surrounded in a pointer, such that the caller doesn't need to write something like
    // (*_parsedPipeline).operator->() or &**_parsedPipeline.
    const LiteParsedPipeline& pipeline() const {
        return _pipeline;
    }

    LiteParsedPipeline* operator->() {
        return &_pipeline;
    }

    const LiteParsedPipeline* operator->() const {
        return &_pipeline;
    }

    LiteParsedPipeline& operator*() {
        return _pipeline;
    }

    const LiteParsedPipeline& operator*() const {
        return _pipeline;
    }

private:
    static std::vector<BSONObj> _makeStagesOwned(const std::vector<BSONObj>& pipelineStages);

    // Must be declared before '_pipeline': initializer-list order follows declaration order,
    // so the owned copies must exist before '_pipeline' parses them.
    std::vector<BSONObj> _ownedStages;
    LiteParsedPipeline _pipeline;
};

}  // namespace mongo
