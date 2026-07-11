// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/collation/collation_index_key.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <stack>
#include <string_view>
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
void translateElement(std::string_view fieldName,
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
            tassert(11177300,
                    "translateElement() cannot be called with an object if ctxStack is null",
                    ctxStack);
            ctxStack->emplace(BSONObjIterator(element.Obj()), &out->subobjStart(fieldName));
            return;
        }
        case BSONType::array: {
            tassert(11177301,
                    "translateElement() cannot be called with an array if ctxStack is null",
                    ctxStack);
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
    tassert(11177302, "collator cannot be null", collator);

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
    tassert(11177303, "out cannot be null", out);
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
