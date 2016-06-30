/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/query/collation/collation_index_key.h"

#include <stack>

#include "mongo/base/disallow_copying.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"

namespace mongo {

namespace {

// To implement string translation iteratively, we utilize TranslateContext.  A translate context
// holds necessary information for in-progress translations.  TranslateContexts are held by a
// TranslateStack, which acts like a heap-allocated call stack.
class TranslateContext {
    MONGO_DISALLOW_COPYING(TranslateContext);

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
        case BSONType::String: {
            out->append(fieldName,
                        collator->getComparisonKey(element.valueStringData()).getKeyData());
            return;
        }
        case BSONType::Object: {
            invariant(ctxStack);
            ctxStack->emplace(element.Obj().begin(), &out->subobjStart(fieldName));
            return;
        }
        case BSONType::Array: {
            invariant(ctxStack);
            ctxStack->emplace(element.Obj().begin(), &out->subarrayStart(fieldName));
            return;
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
    ctxStack.emplace(obj.begin(), out);

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
}

// TODO SERVER-24674: We may want to check that objects and arrays actually do contain strings
// before returning true.
bool CollationIndexKey::shouldUseCollationIndexKey(BSONElement elt,
                                                   const CollatorInterface* collator) {
    return collator && isCollatableType(elt.type());
}

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
                  elt.type() == BSONType::Array ? &out->subarrayStart("") : &out->subobjStart(""));
    } else {
        translateElement("", elt, collator, out, nullptr);
    }
}

}  // namespace mongo
