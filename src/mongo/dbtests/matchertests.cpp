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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/client.h"
#include "mongo/db/exec/matcher/matcher.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/matcher/matcher.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/timer.h"

#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace MatcherTests {

class CollectionBase {
public:
    CollectionBase() {}

    virtual ~CollectionBase() {}
};

const NamespaceString kTestNss = NamespaceString::createNamespaceString_forTest("db.dummy");

template <typename M>
class Basic {
public:
    void run() {
        BSONObj query = fromjson("{\"a\":\"b\"}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);
        ASSERT(exec::matcher::matches(&m, fromjson("{\"a\":\"b\"}")));
    }
};

template <typename M>
class DoubleEqual {
public:
    void run() {
        BSONObj query = fromjson("{\"a\":5}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);
        ASSERT(exec::matcher::matches(&m, fromjson("{\"a\":5}")));
    }
};

template <typename M>
class MixedNumericEqual {
public:
    void run() {
        BSONObjBuilder query;
        query.append("a", 5);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query.done(), expCtx);
        ASSERT(exec::matcher::matches(&m, fromjson("{\"a\":5}")));
    }
};

template <typename M>
class MixedNumericGt {
public:
    void run() {
        BSONObj query = fromjson("{\"a\":{\"$gt\":4}}");
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);
        BSONObjBuilder b;
        b.append("a", 5);
        ASSERT(exec::matcher::matches(&m, b.done()));
    }
};

template <typename M>
class MixedNumericIN {
public:
    void run() {
        BSONObj query = fromjson("{ a : { $in : [4,6] } }");
        ASSERT_EQUALS(4, query["a"].embeddedObject()["$in"].embeddedObject()["0"].number());
        ASSERT_EQUALS(BSONType::numberInt,
                      query["a"].embeddedObject()["$in"].embeddedObject()["0"].type());

        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(query, expCtx);

        {
            BSONObjBuilder b;
            b.append("a", 4.0);
            ASSERT(exec::matcher::matches(&m, b.done()));
        }

        {
            BSONObjBuilder b;
            b.append("a", 5);
            ASSERT(!exec::matcher::matches(&m, b.done()));
        }


        {
            BSONObjBuilder b;
            b.append("a", 4);
            ASSERT(exec::matcher::matches(&m, b.done()));
        }
    }
};

template <typename M>
class MixedNumericEmbedded {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(BSON("a" << BSON("x" << 1)), expCtx);
        ASSERT(exec::matcher::matches(&m, BSON("a" << BSON("x" << 1))));
        ASSERT(exec::matcher::matches(&m, BSON("a" << BSON("x" << 1.0))));
    }
};

template <typename M>
class Size {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{a:{$size:4}}"), expCtx);
        ASSERT(exec::matcher::matches(&m, fromjson("{a:[1,2,3,4]}")));
        ASSERT(!exec::matcher::matches(&m, fromjson("{a:[1,2,3]}")));
        ASSERT(!exec::matcher::matches(&m, fromjson("{a:[1,2,3,'a','b']}")));
        ASSERT(!exec::matcher::matches(&m, fromjson("{a:[[1,2,3,4]]}")));
    }
};

template <typename M>
class WithinBox {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{loc:{$within:{$box:[{x: 4, y:4},[6,6]]}}}"), expCtx);
        ASSERT(!exec::matcher::matches(&m, fromjson("{loc: [3,4]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [4,4]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [5,5]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [5,5.1]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: {x: 5, y:5.1}}")));
    }
};

template <typename M>
class WithinPolygon {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{loc:{$within:{$polygon:[{x:0,y:0},[0,5],[5,5],[5,0]]}}}"), expCtx);
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [3,4]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [4,4]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: {x:5,y:5}}")));
        ASSERT(!exec::matcher::matches(&m, fromjson("{loc: [5,5.1]}")));
        ASSERT(!exec::matcher::matches(&m, fromjson("{loc: {}}")));
    }
};

template <typename M>
class WithinCenter {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(fromjson("{loc:{$within:{$center:[{x:30,y:30},10]}}}"), expCtx);
        ASSERT(!exec::matcher::matches(&m, fromjson("{loc: [3,4]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: {x:30,y:30}}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [20,30]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [30,20]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [40,30]}")));
        ASSERT(exec::matcher::matches(&m, fromjson("{loc: [30,40]}")));
        ASSERT(!exec::matcher::matches(&m, fromjson("{loc: [31,40]}")));
    }
};

/** Test that MatchDetails::elemMatchKey() is set correctly after a match. */
template <typename M>
class ElemMatchKey {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M matcher(BSON("a.b" << 1), expCtx);
        MatchDetails details;
        details.requestElemMatchKey();
        ASSERT(!details.hasElemMatchKey());
        ASSERT(exec::matcher::matches(&matcher, fromjson("{ a:[ { b:1 } ] }"), &details));
        // The '0' entry of the 'a' array is matched.
        ASSERT(details.hasElemMatchKey());
        ASSERT_EQUALS(std::string("0"), details.elemMatchKey());
    }
};

template <typename M>
class WhereSimple1 {
public:
    void run() {
        const ServiceContext::UniqueOperationContext opCtxPtr = cc().makeOperationContext();
        OperationContext& opCtx = *opCtxPtr;
        const NamespaceString nss =
            NamespaceString::createNamespaceString_forTest("unittests.matchertests");
        const auto expCtx = ExpressionContextBuilder{}.opCtx(opCtxPtr.get()).ns(kTestNss).build();
        M m(BSON("$where" << "function(){ return this.a == 1; }"),
            expCtx,
            ExtensionsCallbackReal(&opCtx, &nss),
            MatchExpressionParser::AllowedFeatures::kJavascript);
        ASSERT(exec::matcher::matches(&m, BSON("a" << 1)));
        ASSERT(!exec::matcher::matches(&m, BSON("a" << 2)));
    }
};

template <typename M>
class TimingBase {
public:
    long dotime(const BSONObj& patt, const BSONObj& obj) {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M m(patt, expCtx);
        Timer t;
        for (int i = 0; i < 900000; i++) {
            if (!exec::matcher::matches(&m, obj)) {
                ASSERT(0);
            }
        }
        return t.millis();
    }
};

template <typename M>
class AllTiming : public TimingBase<M> {
public:
    void run() {
        long normal = TimingBase<M>::dotime(BSON("x" << 5), BSON("x" << 5));

        long all =
            TimingBase<M>::dotime(BSON("x" << BSON("$all" << BSON_ARRAY(5))), BSON("x" << 5));

        std::cout << "AllTiming " << demangleName(typeid(M)) << " normal: " << normal
                  << " all: " << all << std::endl;
    }
};

/** Test that 'collator' is passed to MatchExpressionParser::parse(). */
template <typename M>
class NullCollator {
public:
    void run() {
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        M matcher(BSON("a" << "string"), expCtx);
        ASSERT(!exec::matcher::matches(&matcher, BSON("a" << "string2")));
    }
};

/** Test that 'collator' is passed to MatchExpressionParser::parse(). */
template <typename M>
class Collator {
public:
    void run() {
        auto collator =
            std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        expCtx->setCollator(std::move(collator));
        M matcher(BSON("a" << "string"), expCtx);
        ASSERT(exec::matcher::matches(&matcher, BSON("a" << "string2")));
    }
};

class All : public unittest::OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("matcher") {}

#define ADD_BOTH(TEST) add<TEST<Matcher>>();

    void setupTests() override {
        ADD_BOTH(Basic);
        ADD_BOTH(DoubleEqual);
        ADD_BOTH(MixedNumericEqual);
        ADD_BOTH(MixedNumericGt);
        ADD_BOTH(MixedNumericIN);
        ADD_BOTH(Size);
        ADD_BOTH(MixedNumericEmbedded);
        ADD_BOTH(ElemMatchKey);
        ADD_BOTH(WhereSimple1);
        ADD_BOTH(AllTiming);
        ADD_BOTH(WithinBox);
        ADD_BOTH(WithinCenter);
        ADD_BOTH(WithinPolygon);
        ADD_BOTH(NullCollator);
        ADD_BOTH(Collator);
    }
};

unittest::OldStyleSuiteInitializer<All> dball;

}  // namespace MatcherTests
}  // namespace mongo
