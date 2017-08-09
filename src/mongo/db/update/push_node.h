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

#include <boost/optional.hpp>
#include <limits>
#include <vector>

#include "mongo/db/update/path_creating_node.h"
#include "mongo/db/update/push_sorter.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class PushNode final : public PathCreatingNode {
public:
    PushNode()
        : _slice(std::numeric_limits<long long>::max()),
          _position(std::numeric_limits<long long>::max()) {}
    Status init(BSONElement modExpr, const CollatorInterface* collator) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return stdx::make_unique<PushNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {
        if (_sort) {
            invariant(!_sort->collator);
            _sort->collator = collator;
        }
    }

protected:
    UpdateExistingElementResult updateExistingElement(mutablebson::Element* element,
                                                      std::shared_ptr<FieldRef> elementPath,
                                                      LogBuilder* logBuilder) const final;
    void setValueForNewElement(mutablebson::Element* element) const final;

private:
    /**
     * Used to describe the result of the PerformPush operation. Note that appending to any empty
     * array is always considered kModifyArray. That's because we want $push onto an empty to array
     * to trigger a log entry with a $set on the entire array.
     */
    enum class PushResult {
        kNoOp,                // The array is left exactly as it was.
        kAppendToEndOfArray,  // The only change to the array is items appended to the end.
        kModifyArray          // Any other modification of the array.
    };

    static PushResult insertElementsWithPosition(mutablebson::Element* array,
                                                 long long position,
                                                 const std::vector<BSONElement> valuesToPush);
    PushResult performPush(mutablebson::Element* element, FieldRef* elementPath) const;

    static const StringData kEachClauseName;
    static const StringData kSliceClauseName;
    static const StringData kSortClauseName;
    static const StringData kPositionClauseName;

    std::vector<BSONElement> _valuesToPush;
    long long _slice;
    long long _position;
    boost::optional<PatternElementCmp> _sort;
};

}  // namespace mongo
