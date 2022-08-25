/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/optimizer/node.h"
#include "mongo/db/query/optimizer/syntax/syntax.h"
#include "mongo/db/query/optimizer/utils/utils.h"

namespace mongo::optimizer {

/**
 * Used to track information including the current root node of the ABT and the current projection
 * representing output documents.
 */
class AlgebrizerContext {
public:
    struct NodeWithRootProjection {
        NodeWithRootProjection(ProjectionName rootProjection, ABT node)
            : _rootProjection(std::move(rootProjection)), _node(std::move(node)) {}

        ProjectionName _rootProjection;
        ABT _node;
    };

    AlgebrizerContext(PrefixId& prefixId, NodeWithRootProjection node)
        : _node(std::move(node)), _scanProjName(_node._rootProjection), _prefixId(prefixId) {
        assertNodeSort(_node._node);
    }

    template <typename T, typename... Args>
    inline auto setNode(ProjectionName rootProjection, Args&&... args) {
        setNode(std::move(rootProjection), std::move(ABT::make<T>(std::forward<Args>(args)...)));
    }

    void setNode(ProjectionName rootProjection, ABT node) {
        assertNodeSort(node);
        _node._node = std::move(node);
        _node._rootProjection = std::move(rootProjection);
    }

    NodeWithRootProjection& getNode() {
        return _node;
    }

    std::string getNextId(const std::string& key) {
        return _prefixId.getNextId(key);
    }

    PrefixId& getPrefixId() {
        return _prefixId;
    }

    const ProjectionName& getScanProjName() const {
        return _scanProjName;
    }

private:
    NodeWithRootProjection _node;
    ProjectionName _scanProjName;

    PrefixId& _prefixId;
};

}  // namespace mongo::optimizer
