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
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"

namespace mongo {

class Desugarer {
public:
    using StageExpander = std::function<DocumentSourceContainer::iterator(
        Desugarer*, DocumentSourceContainer::iterator, const DocumentSource&)>;

    explicit Desugarer(Pipeline* pipeline) : _sources(pipeline->getSources()) {}

    // Desugars the pipeline.
    void operator()();

    static void registerStageExpander(DocumentSource::Id id, StageExpander stageExpander) {
        _stageExpanders[id] = std::move(stageExpander);
    }

    // Adds in newSources at position itr in _sources and returns the iterator *after* the sources
    // added.
    DocumentSourceContainer::iterator replaceStageWith(
        DocumentSourceContainer::iterator itr,
        std::list<boost::intrusive_ptr<DocumentSource>>&& newSources);

private:
    // Associate a stage expander for each stage that should desugar.
    // NOTE: this map is *not* thread safe. DocumentSources should register their stageExpander
    // using MONGO_INITIALIZER to ensure thread safety. See DocumentSourceExtensionExpandable for an
    // example.
    inline static stdx::unordered_map<DocumentSource::Id, StageExpander> _stageExpanders{};

    DocumentSourceContainer& _sources;
};

}  // namespace mongo
