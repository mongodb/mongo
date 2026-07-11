// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/field_ref.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/update/log_builder_interface.h"
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/pattern_cmp.h"
#include "mongo/db/update/runtime_update_path.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class PushNode final : public ModifierNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<PushNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final {
        if (_sort) {
            invariant(!_sort->collator);
            _sort->collator = collator;
        }
    }

    void acceptVisitor(UpdateNodeVisitor* visitor) final {
        visitor->visit(this);
    }

protected:
    ModifyResult updateExistingElement(mutablebson::Element* element,
                                       const FieldRef& elementPath) const final;
    void setValueForNewElement(mutablebson::Element* element) const final;
    void logUpdate(LogBuilderInterface* logBuilder,
                   const RuntimeUpdatePath& pathTaken,
                   mutablebson::Element element,
                   ModifyResult modifyResult,
                   boost::optional<int> createdFieldIdx) const final;

    bool allowCreation() const final {
        return true;
    }


private:
    std::string_view operatorName() const final {
        return "$push";
    }

    BSONObj operatorValue(const query_shape::SerializationOptions& opts) const final;

    // A helper for performPush().
    static ModifyResult insertElementsWithPosition(mutablebson::Element* array,
                                                   boost::optional<long long> position,
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
    ModifyResult performPush(mutablebson::Element* element, const FieldRef* elementPath) const;

    static const std::string_view kEachClauseName;
    static const std::string_view kSliceClauseName;
    static const std::string_view kSortClauseName;
    static const std::string_view kPositionClauseName;

    std::vector<BSONElement> _valuesToPush;
    boost::optional<long long> _slice;
    boost::optional<long long> _position;
    boost::optional<PatternElementCmp> _sort;
};

}  // namespace mongo
