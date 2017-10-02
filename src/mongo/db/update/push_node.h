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

#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/push_sorter.h"
#include "mongo/stdx/memory.h"

namespace mongo {

class PushNode final : public ModifierNode {
public:
    PushNode()
        : _slice(std::numeric_limits<long long>::max()),
          _position(std::numeric_limits<long long>::max()) {}
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

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
    ModifyResult updateExistingElement(mutablebson::Element* element,
                                       std::shared_ptr<FieldRef> elementPath) const final;
    void setValueForNewElement(mutablebson::Element* element) const final;
    void logUpdate(LogBuilder* logBuilder,
                   StringData pathTaken,
                   mutablebson::Element element,
                   ModifyResult modifyResult) const final;

    bool allowCreation() const final {
        return true;
    }


private:
    // A helper for performPush().
    static ModifyResult insertElementsWithPosition(mutablebson::Element* array,
                                                   long long position,
                                                   const std::vector<BSONElement>& valuesToPush);

    /**
     * Inserts the elements from '_valuesToPush' in the 'element' array using '_position' to
     * determine where to insert. This function also applies any '_slice' and or '_sort' that is
     * specified. The return value of this function will indicate to logUpdate() what kind of oplog
     * entries should be generated.
     *
     * Returns:
     *   - ModifyResult::kNoOp if '_valuesToPush' is empty and no slice or sort gets performed;
     *   - ModifyResult::kArrayAppendUpdate if the 'elements' array is initially non-empty, all
     *     inserted values are appended to the end, and no slice or sort gets performed; or
     *   - ModifyResult::kNormalUpdate if 'elements' is initially an empty array, values get
     *     inserted at the beginning or in the middle of the array, or a slice or sort gets
     *     performed.
     */
    ModifyResult performPush(mutablebson::Element* element, FieldRef* elementPath) const;

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
