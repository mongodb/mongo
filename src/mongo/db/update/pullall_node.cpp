// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/pullall_node.h"

#include "mongo/base/clonable_ptr.h"
#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/db/exec/mutable_bson/const_element.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

class PullAllNode::SetMatcher final : public ArrayCullingNode::ElementMatcher {
public:
    SetMatcher(std::vector<BSONElement> elementsToMatch, const CollatorInterface* collator)
        : _elementsToMatch(std::move(elementsToMatch)), _collator(collator) {}

    std::unique_ptr<ElementMatcher> clone() const final {
        return std::make_unique<SetMatcher>(*this);
    }

    bool match(const mutablebson::ConstElement& element) final {
        return std::any_of(_elementsToMatch.begin(),
                           _elementsToMatch.end(),
                           [&element, collator{_collator}](const auto& elementToMatch) {
                               return element.compareWithBSONElement(
                                          elementToMatch, collator, false) == 0;
                           });
    }

    void setCollator(const CollatorInterface* collator) final {
        _collator = collator;
    }

private:
    BSONObj value(const query_shape::SerializationOptions& opts) const final {
        BSONArrayBuilder subarrayBuilder;
        for (const auto& element : _elementsToMatch)
            subarrayBuilder << element;
        return BSON("" << opts.serializeLiteral(subarrayBuilder.arr()));
    }

    std::vector<BSONElement> _elementsToMatch;
    const CollatorInterface* _collator;
};

Status PullAllNode::init(BSONElement modExpr,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    if (modExpr.type() != BSONType::array) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "$pullAll requires an array argument but was given a "
                                    << typeName(modExpr.type()));
    }

    _matcher = std::make_unique<SetMatcher>(modExpr.Array(), expCtx->getCollator());

    return Status::OK();
}

}  // namespace mongo
