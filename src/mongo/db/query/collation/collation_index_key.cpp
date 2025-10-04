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

#include "mongo/db/query/collation/collation_index_key.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <stack>
#include <utility>

namespace mongo {

namespace {

// To implement string translation iteratively, we utilize TranslateContext.  A translate context
// holds necessary information for in-progress translations.  TranslateContexts are held by a
// TranslateStack, which acts like a heap-allocated call stack.
class TranslateContext {
    TranslateContext(const TranslateContext&) = delete;
    TranslateContext& operator=(const TranslateContext&) = delete;

public:
    TranslateContext(BSONObjIterator&& iter, BufBuilder* buf)
        : bob(*buf), _objIterator(std::move(iter)) {}

    /**
     * Returns true if the underlying iterator has additional elements.
     */
    bool more() {
        return _objIterator.more();
    }

    /**
     * Access the next element in the underlying iterator, and advance it.
     *
     * *Precondition*: A call to next() is only valid if a prior call to more() returned true.
     */
    BSONElement next() {
        return _objIterator.next();
    }

    BSONObjBuilder& getBuilder() {
        return bob;
    }

private:
    BSONObjBuilder bob;
    BSONObjIterator _objIterator;
};

using TranslateStack = std::stack<TranslateContext>;

// Translate a single element, using the provided collator, appending the result to 'out'.
//
// If the element is an object or array, a new context is added to 'ctxStack', and the function
// returns without modifying 'out'.
//
// If ctxStack is null, _translate must *not* be called with an object or array.  Additionally,
// an empty string will be used for the field name when appending 'element' to 'out'.
void translateElement(StringData fieldName,
                      const BSONElement& element,
                      const CollatorInterface* collator,
                      BSONObjBuilder* out,
                      TranslateStack* ctxStack) {
    switch (element.type()) {
        case BSONType::string: {
            out->append(fieldName,
                        collator->getComparisonKey(element.valueStringData()).getKeyData());
            return;
        }
        case BSONType::object: {
            invariant(ctxStack);
            ctxStack->emplace(BSONObjIterator(element.Obj()), &out->subobjStart(fieldName));
            return;
        }
        case BSONType::array: {
            invariant(ctxStack);
            ctxStack->emplace(BSONObjIterator(element.Obj()), &out->subarrayStart(fieldName));
            return;
        }
        case BSONType::symbol: {
            uasserted(ErrorCodes::CannotBuildIndexKeys,
                      str::stream()
                          << "Cannot index type Symbol with a collation. Failed to index element: "
                          << element << ". Index collation: " << collator->getSpec().toBSON());
        }
        default:
            out->appendAs(element, fieldName);
    }
}

// Translate all strings in 'obj' into comparison keys using 'collator'. The result is
// appended to 'out'.
void translate(BSONObj obj, const CollatorInterface* collator, BufBuilder* out) {
    invariant(collator);

    TranslateStack ctxStack;
    ctxStack.emplace(BSONObjIterator(obj), out);

    while (!ctxStack.empty()) {
        TranslateContext& ctx = ctxStack.top();

        if (!ctx.more()) {
            ctxStack.pop();
            continue;
        }

        BSONElement element = ctx.next();
        translateElement(
            element.fieldNameStringData(), element, collator, &ctx.getBuilder(), &ctxStack);
    }
}
}  // namespace

void CollationIndexKey::collationAwareIndexKeyAppend(BSONElement elt,
                                                     const CollatorInterface* collator,
                                                     BSONObjBuilder* out) {
    invariant(out);
    if (!collator) {
        out->appendAs(elt, "");
        return;
    }

    if (elt.isABSONObj()) {
        translate(elt.Obj(),
                  collator,
                  elt.type() == BSONType::array ? &out->subarrayStart("") : &out->subobjStart(""));
    } else {
        translateElement("", elt, collator, out, nullptr);
    }
}

}  // namespace mongo
