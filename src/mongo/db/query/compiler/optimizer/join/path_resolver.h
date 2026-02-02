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

#include "mongo/db/query/compiler/optimizer/join/logical_defs.h"
#include "mongo/util/modules.h"

namespace mongo::join_ordering {
/**
 * PathResolver is designed to work with the $lookup stage. The $lookup stage introduces two path
 * contexts:
 * **Foreign context**: This refers to the input from the foreign collection during the '$lookup'.
 * In this context, all fields are known to belong to the foreign collection. To add a field from
 * the foreign collection, we call the 'addPath' function. The paths in '$lookup.foreignField' and
 * inside the subpipeline are examples of the foreign context paths.
 *
 * **Local context**: This refers to the intermediate results produced after processing previous
 * '$lookup' stages. Since the local context contains fields from multiple collections, resolving a
 * field in this context requires checking known prefixes introduced by previous $lookup stages. In
 * this case the 'resolve' function must be called. The paths in '$lookup.localField' and in the
 * main pipeline are examples of the local context paths.
 *
 * '$lookup' stages are expected to be processed in order they apper in the pipeline with every
 * $lookup introducing a new node. Adding the same node multiple times is UB.
 *
 * **Known Limitations**.
 * 1. The class is not designed to work with computed fieds and field aliases defined in the
 * '$project' and similar stages, instead of resolving a field as a computed field, or a field
 * belonging to a particular collection, it resolves them just to a join node. The fields which
 * comes with a join node could be collection fields, or computed fields. The PathResolver class has
 * no idea about it.
 * 2. To use the 'PathResolver' with '$lookup' subpipelines, a new instance of the class should be
 * created. To make sure that paths accross the main pipeline and subpipelines use unique path ids
 * it should use the same 'resolvedPaths' array (see the constructor's parameter
 * 'std::vector<ResolvedPath>& resolvedPaths') . After resolving symbols of a pipeline the scopes of
 * the subpipeline's 'PathResolver' should be merged into the main 'PathResolver' by prefixing the
 * value of the '$lookup.as' field to 'embedPaths' of the subpipeline scopes.
 * 3. After '$unionWith' stage the usefulness of'PathResolver' is limited since any path can come
 * from any collection.
 *
 * ** Join Node is not a Collection**.
 * Although a join node and a collection can be used interchangeably in an everyday conversation, it
 * is important to understand that a join node is not exactly a collection, e.g. the same collection
 * can be joined twice in different $lookup stages and, as a result, two nodes will be referenced to
 * the same collection. Consider the following example:
 * {$lookup: {from: "A", as: "foo", ...}},
 * ...
 * {$lookup: {from: "A", as: "bar", ...}},
 * ...
 * The paths "foo.a" and "bar.a" will be resolved to different nodes, even though they originate
 * from the same collection. The fact that they come from the same collection doesn't mean they have
 * the same values: their value depends on thir respective join predicates, subpipelines, etc.
 */

class PathResolver {
public:
    PathResolver(NodeId baseNode, std::vector<ResolvedPath>& resolvedPaths);


    /**
     * Introduce a new prefix 'embedPath' in the resolver that is associated with the node
     * 'nodeId'.
     */
    void addNode(NodeId nodeId, const FieldPath& embedPath);

    /**
     * Add a new path to the given node. The known node prefixes are not consulted.
     * This function should be used when it is know for sure that this particular fieldPath comes
     * from the node 'nodeId'.
     * Unlike the 'resolve' function, this function never removes prefixes of the matched node from
     * the 'fieldPath'. It means, that even after calling 'addNode(1, "a")', 'addNode(1, "a.b.c")'
     * resolves to the field "a.b.c" of node 1. However, 'resolve("a.b.c")' resolves to field "b.c"
     * of node 1 and 'resolve("a.a.b.c")' resolved to the field "a.b.c" of node 1.
     */
    PathId addPath(NodeId nodeId, FieldPath fieldPath);

    /**
     * The function checks for known prefixes and identifies the most recently added node which
     * 'embedPath' prefixes the 'path' is selected as the node of origin of the path.If the field is
     * resolved for the first time a new path id is created, assigned to the winning node and
     * returned from the function. It is important to understand that subsequent 'addNode' calls
     * change how a particular path is resolved. For example:
     * 1. Call for "a.b.c", no prefixes matched, the field is resolved to "a.b.c" of the base node.
     * 2. After 'addNode(1, "a.b")' is called, the field "a.b.c" is resolved to "c" of node 1.
     * 3. After 'addNode(2, "b.c")' is called, the field "a.b.c" is still resolved to "c" of
     * node 1, because embed path of Node 2 does not prefix the path.
     * 4. After 'addNode(3, "a")' is called, the field "a.b.c" is resolved to "b.c" of node 3,
     * because the node with prefix "a" was added more recently then the node with prefix "a.b".
     */
    PathId resolve(const FieldPath& path);

    const ResolvedPath& operator[](PathId PathId) const {
        return _resolvedPaths.at(PathId);
    }

private:
    /**
     * A scope/namespace of field paths added by '$lookup' stage.
     */
    struct Scope {
        /**
         * Base node constructor.
         */
        Scope(NodeId nodeId) : nodeId(nodeId) {}

        /**
         * A constructor for nodes with a prefix 'embedPath' (all but base node).
         */
        Scope(NodeId nodeId, FieldPath embedPath) : nodeId(nodeId), embedPath(embedPath) {}

        NodeId nodeId;
        boost::optional<FieldPath> embedPath;
        absl::flat_hash_map<FieldPath, PathId> paths;
    };

    /** Resolves the path by its longest prefix with the known node embedPaths and returns the
     * resolved node and the resolved field path without the embedPath of the node.
     */
    std::pair<NodeId, FieldPath> resolveNodeByEmbedPath(const FieldPath& fieldPath) const;

    boost::optional<Scope&> getScopeForNode(NodeId node);

    std::vector<ResolvedPath>& _resolvedPaths;
    std::vector<Scope> _scopes;
};
}  // namespace mongo::join_ordering
