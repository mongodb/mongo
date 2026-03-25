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


#include "mongo/base/string_data.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo::pipeline::dependency_graph {

using PathRef = StringData;

using CanPathBeArray = std::function<bool(StringData)>;

bool defaultCanPathBeArray(StringData path);

/**
 * Represents dependencies between fields and stages in a pipeline. Can be partially rebuilt when
 * the pipeline changes.
 */
class DependencyGraph {
public:
    explicit DependencyGraph(const DocumentSourceContainer& container,
                             CanPathBeArray canMainCollPathBeArray = defaultCanPathBeArray);
    ~DependencyGraph();
    DependencyGraph(DependencyGraph&&) noexcept;
    DependencyGraph& operator=(DependencyGraph&&) noexcept;

    /**
     * Return the stage which last modified the path visible from the given DocumentSource. The
     * stage must have either declared, modified or removed the path. If nullptr, the path is
     * unmodified and assumed to originate from the pipeline input.
     *
     * For example, the following stages all modify the path 'a'.
     * - {$set: {a: 1}}
     * - {$set: {a.b: 1}}
     * - {$project: {a: 0}}
     * - {$group: {_id: ...}}
     */
    boost::intrusive_ptr<mongo::DocumentSource> getDeclaringStage(DocumentSource* stage,
                                                                  PathRef path) const;

    /**
     * Returns false if the path visible from the given DocumentSource can be assumed to not contain
     * arrays. If nullptr, the path is assumed to originate from the pipeline input.
     */
    bool canPathBeArray(DocumentSource* stage, PathRef path) const;

    /**
     * Invalidate and recompute the subgraph starting from the earliest nodes which correspond to
     * the stage pointed to by 'stageIt'.
     */
    void recompute(const DocumentSourceContainer& container,
                   boost::optional<DocumentSourceContainer::const_iterator> stageIt = {});

    std::string toDebugString() const;
    BSONObj toBSON() const;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;
};

}  // namespace mongo::pipeline::dependency_graph
