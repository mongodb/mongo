// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/document_tree_generator.h"

#include <algorithm>
#include <string>

namespace mongo::sbe {

namespace {

/**
 * Renders the spec vector into BSON. Maintains a position over the spec and builds nodes
 * recursively, each node consuming the specs of its subtree, so siblings resume where the last
 * subtree ended.
 */
struct Renderer {
    const DocumentSpec& spec;
    size_t pos = 0;

    bool done() {
        return pos >= spec.size();
    }

    /**
     * Overloads for appending to the different kinds of container builders.
     */
    void appendNull(BSONObjBuilder& builder, const std::string_view& fieldName) {
        builder.appendNull(fieldName);
    }
    void appendNull(BSONArrayBuilder& builder, const std::string_view&) {
        builder.appendNull();
    }
    void appendValue(BSONObjBuilder& builder,
                     const std::string_view& fieldName,
                     const auto& value) {
        builder.append(fieldName, value);
    }
    void appendValue(BSONArrayBuilder& builder, const std::string_view&, const auto& value) {
        builder.append(value);
    }

    template <typename Builder>
    bool appendScalar(Builder& builder, const std::string_view& fieldName, const ScalarSpec& node) {
        return std::visit(
            [&](auto&& v) -> bool {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, NothingSpec>) {
                    return false;
                } else if constexpr (std::is_same_v<T, NullSpec>) {
                    appendNull(builder, fieldName);
                } else {
                    appendValue(builder, fieldName, v);
                }
                return true;
            },
            node);
    }

    /**
     * Appends child nodes from the spec to a container builder for an object or array.
     */
    template <typename SubBuilder>
    void appendChildren(SubBuilder& subBuilder, unsigned depth, const auto& node) {
        if (depth == kMaxTreeDepth) {
            // A container at max depth is forced empty and consumes no child specs.
            return;
        }
        const unsigned childCount = std::min(node.childCount, kMaxTreeBranching);
        unsigned numChildren = 0, fieldIdx = 0;
        while (!done() && numChildren < childCount) {
            if (appendNode(subBuilder, positionalName(fieldIdx++, depth + 1), depth + 1)) {
                ++numChildren;
            }
        }
    }

    /*
     * Builds the document for the next node in the spec and appends it to the builder. Returns true
     * if a node was added to the builder, false if the node was Nothing.
     */
    template <typename Builder>
    bool appendNode(Builder& builder, std::string_view fieldName, unsigned depth) {
        if (done()) {
            return false;  // spec is out of nodes
        }
        const NodeSpec& node = spec[pos++];
        if (auto* scalar = std::get_if<ScalarSpec>(&node)) {
            return appendScalar(builder, fieldName, *scalar);
        } else if (auto* object = std::get_if<ObjectSpec>(&node)) {
            BSONObjBuilder subBuilder;
            appendChildren(subBuilder, depth, *object);
            appendValue(builder, fieldName, subBuilder.obj());
        } else {
            BSONArrayBuilder subBuilder;
            appendChildren(subBuilder, depth, std::get<ArraySpec>(node));
            appendValue(builder, fieldName, subBuilder.arr());
        }
        return true;
    }
};

}  // namespace

BSONObj renderDocumentFields(const DocumentSpec& spec) {
    BSONObjBuilder b;
    Renderer renderer{spec};
    // At the root level, don't enforce the branching factor and consume the whole spec
    unsigned fieldIdx = 0;
    while (!renderer.done()) {
        renderer.appendNode(b, positionalName(fieldIdx++), 0);
    }
    return b.obj();
}

BSONObj renderDocument(const DocumentSpec& spec, long long id) {
    BSONObjBuilder b;
    b.appendNumber("_id", id);
    b.appendElements(renderDocumentFields(spec));
    return b.obj();
}

}  // namespace mongo::sbe
