/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/db/update/document_diff_serialization.h"
#include "mongo/db/update/log_builder_interface.h"
#include "mongo/db/update/runtime_update_path.h"

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
