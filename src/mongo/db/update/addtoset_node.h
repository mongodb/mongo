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
#include "mongo/db/update/modifier_node.h"
#include "mongo/db/update/update_node.h"
#include "mongo/db/update/update_node_visitor.h"
#include "mongo/util/modules.h"

#include <memory>
#include <string_view>
#include <vector>

#include <absl/container/btree_set.h>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

/**
 * Represents the application of an $addToSet to the value at the end of a path.
 */
class AddToSetNode : public ModifierNode {
public:
    Status init(BSONElement modExpr, const boost::intrusive_ptr<ExpressionContext>& expCtx) final;

    std::unique_ptr<UpdateNode> clone() const final {
        return std::make_unique<AddToSetNode>(*this);
    }

    void setCollator(const CollatorInterface* collator) final;

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
        return "$addToSet";
    }

    BSONObj operatorValue(const query_shape::SerializationOptions& opts) const final {
        if (!_useEachElem) {
            tassert(11034401,
                    "We should only be serializing one element max if $each is not specified.",
                    _elements.size() == 1);
            return BSON("" << opts.serializeLiteral(_elements[0]));
        } else {
            BSONArrayBuilder valueArrayBuilder;
            for (const auto& element : _elements)
                valueArrayBuilder << element;
            return BSON("" << BSON("$each" << opts.serializeLiteral(valueArrayBuilder.arr())));
        }
    }

    struct BSONElementAndIndex {
        BSONElement element;
        size_t index;
    };

    class BSONElementAndIndexComparator {
    public:
        using is_transparent = void;

        explicit BSONElementAndIndexComparator(const CollatorInterface* collator = nullptr)
            : _collator(collator) {}

        bool operator()(const BSONElementAndIndex& l, const BSONElementAndIndex& r) const {
            return l.element.woCompare(r.element, false, _collator) < 0;
        }

        bool operator()(const BSONElementAndIndex& l, const mutablebson::Element& r) const {
            return r.compareWithBSONElement(l.element, _collator, false) > 0;
        }

        bool operator()(const mutablebson::Element& l, const BSONElementAndIndex& r) const {
            return l.compareWithBSONElement(r.element, _collator, false) < 0;
        }

        const CollatorInterface* collator() const {
            return _collator;
        }

    private:
        const CollatorInterface* _collator;
    };

    void _deduplicateElements();
    std::vector<BSONElementAndIndex> _getElementsToAdd(mutablebson::Element* element) const;

    // The array of elements to be added.
    std::vector<BSONElement> _elements;
    // The set of elements to be added, pairsed with their index in the original _elements array.
    absl::btree_set<BSONElementAndIndex, BSONElementAndIndexComparator> _elementsSet;

    // Specifies whether the $each operator was used. This is important for correct serialization to
    // representative and debug query shapes.
    bool _useEachElem = false;
};

}  // namespace mongo
