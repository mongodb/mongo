/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/update/pullall_node.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/query/collation/collator_interface.h"

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
                           [&element, collator{_collator} ](const auto& elementToMatch) {
                               return element.compareWithBSONElement(
                                          elementToMatch, collator, false) == 0;
                           });
    }

    void setCollator(const CollatorInterface* collator) final {
        _collator = collator;
    }

private:
    BSONObj value() const final {
        BSONObjBuilder bob;
        {
            BSONArrayBuilder subarrayBuilder(bob.subarrayStart(""));
            for (const auto element : _elementsToMatch)
                subarrayBuilder << element;
        }
        return bob.obj();
    }

    std::vector<BSONElement> _elementsToMatch;
    const CollatorInterface* _collator;
};

Status PullAllNode::init(BSONElement modExpr,
                         const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    invariant(modExpr.ok());

    if (modExpr.type() != Array) {
        return Status(ErrorCodes::BadValue,
                      str::stream() << "$pullAll requires an array argument but was given a "
                                    << typeName(modExpr.type()));
    }

    _matcher = std::make_unique<SetMatcher>(modExpr.Array(), expCtx->getCollator());

    return Status::OK();
}

}  // namespace mongo
