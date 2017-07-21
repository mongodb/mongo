/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#pragma once

#include "mongo/db/update/update_node.h"
#include "mongo/stdx/memory.h"

namespace mongo {

/**
 * An UpdateNode representing a replacement-style update.
 */
class ObjectReplaceNode : public UpdateNode {

public:
    /**
     * Initializes the node with the document to replace with. Any zero-valued timestamps (except
     * for the _id) are updated to the current time.
     */
    explicit ObjectReplaceNode(BSONObj val);

    std::unique_ptr<UpdateNode> clone() const final {
        return stdx::make_unique<ObjectReplaceNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {}

    /**
     * Replaces the document that 'element' belongs to with '_val'. If '_val' does not contain an
     * _id, the _id from the original document is preserved. 'element' must be the root of the
     * document. 'pathToCreate' and 'pathTaken' must be empty. If 'validateForStorage' is true, the
     * modified document is validated for storage. Throws if any path in 'immutablePaths' is
     * modified (but it may be created if it did not yet exist). Logs the update as a
     * replacement-style update. Always outputs that indexes are affected when the replacement is
     * not a noop.
     */
    void apply(mutablebson::Element element,
               FieldRef* pathToCreate,
               FieldRef* pathTaken,
               StringData matchedField,
               bool fromReplication,
               bool validateForStorage,
               const FieldRefSet& immutablePaths,
               const UpdateIndexData* indexData,
               LogBuilder* logBuilder,
               bool* indexesAffected,
               bool* noop) const final;

private:
    // Object to replace with.
    BSONObj _val;

    // True if _val contains an _id.
    bool _containsId;
};

}  // namespace mongo
