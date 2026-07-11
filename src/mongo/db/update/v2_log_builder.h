// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/log_builder_interface.h"
#include "mongo/db/update/runtime_update_path.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace v2_log_builder {
/**
 * A log builder which can produce $v:2 oplog entries.
 *
 * This log builder accumulates updates, creates and deletes, and stores them in a tree. When the
 * update is done and serialize() is called, the tree is converted into a $v:2 oplog entry. Note
 * that we don't need a pre-image for building the oplog.
 */
class V2LogBuilder : public LogBuilderInterface {
public:
    /**
     * Overload methods from the LogBuilder interface.
     */
    Status logUpdatedField(const RuntimeUpdatePath& path, mutablebson::Element elt) override;
    Status logCreatedField(const RuntimeUpdatePath& path,
                           int idxOfFirstNewComponent,
                           mutablebson::Element elt) override;
    Status logCreatedField(const RuntimeUpdatePath& path,
                           int idxOfFirstNewComponent,
                           BSONElement elt) override;
    Status logDeletedField(const RuntimeUpdatePath& path) override;

    /**
     * Converts the in-memory tree to a $v:2 delta oplog entry.
     */
    BSONObj serialize() const override;

private:
    // Helpers for maintaining/updating the tree.
    diff_tree::Node* createInternalNode(diff_tree::InternalNode* parent,
                                        const RuntimeUpdatePath& fullPath,
                                        size_t pathIdx,
                                        bool newPath);

    // Helpers for adding nodes at a certain path. Returns false if the path was invalid/did
    // not exist.
    void addNodeAtPathHelper(const RuntimeUpdatePath& path,
                             size_t pathIdx,
                             diff_tree::Node* root,
                             std::unique_ptr<diff_tree::Node> nodeToAdd,
                             boost::optional<size_t> idxOfFirstNewComponent);

    void addNodeAtPath(const RuntimeUpdatePath& path,
                       diff_tree::Node* root,
                       std::unique_ptr<diff_tree::Node> nodeToAdd,
                       boost::optional<size_t> idxOfFirstNewComponent);

    // Root of the tree.
    diff_tree::DocumentSubDiffNode _root;
};
}  // namespace v2_log_builder
}  // namespace mongo
