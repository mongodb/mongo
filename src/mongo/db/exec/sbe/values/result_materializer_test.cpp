// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/exec/sbe/values/result_materializer.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/expressions/sbe_fn_names.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <array>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/optional.hpp>

namespace mongo::sbe {
namespace {

class ResultMaterializerTest : public EExpressionTestFixture {
protected:
    /**
     * The kinds of slot a plan exposes its result base object through.
     */
    enum class BaseKind {
        kOwnedBson,        // An OwnedValueAccessor owning a bsonObject.
        kUnownedBson,      // An OwnedValueAccessor viewing a bsonObject.
        kRecord,           // A BSONObjValueAccessor viewing a record.
        kSbeObject,        // An OwnedValueAccessor owning an Object.
        kMaterializedRow,  // A MaterializedSingleRowAccessor over a row owning a bsonObject.
    };

    /**
     * The kinds of slot an object-valued set or added field is exposed through. Other values are
     * always owned by an OwnedValueAccessor.
     */
    enum class ValueKind { kOwned, kRecord, kSbeObject };

    static constexpr std::array kAllBaseKinds{BaseKind::kOwnedBson,
                                              BaseKind::kUnownedBson,
                                              BaseKind::kRecord,
                                              BaseKind::kSbeObject,
                                              BaseKind::kMaterializedRow};
    static constexpr std::array kAllValueKinds{
        ValueKind::kOwned, ValueKind::kRecord, ValueKind::kSbeObject};

    /**
     * One change to the result base. 'value' is only used for kSet and kAdd; a missing Value is
     * Nothing.
     */
    struct Change {
        std::string name;
        ResultFieldAction action;
        Value value;
    };

    /**
     * A result base (none is Nothing) and change list with the result they must produce.
     */
    struct Case {
        std::string name;
        boost::optional<BSONObj> base;
        bool closed;
        std::vector<Change> changes;
        BSONObj expected;
    };

    static Change keep(std::string name) {
        return {std::move(name), ResultFieldAction::kKeep};
    }
    static Change drop(std::string name) {
        return {std::move(name), ResultFieldAction::kDrop};
    }
    static Change set(std::string name, ImplicitValue value) {
        return {std::move(name), ResultFieldAction::kSet, std::move(value)};
    }
    static Change add(std::string name, ImplicitValue value) {
        return {std::move(name), ResultFieldAction::kAdd, std::move(value)};
    }

    static const char* toString(BaseKind kind) {
        switch (kind) {
            case BaseKind::kOwnedBson:
                return "owned bsonObject";
            case BaseKind::kUnownedBson:
                return "unowned bsonObject";
            case BaseKind::kRecord:
                return "record";
            case BaseKind::kSbeObject:
                return "Object";
            case BaseKind::kMaterializedRow:
                return "materialized row";
        }
        MONGO_UNREACHABLE;
    }

    static const char* toString(ValueKind kind) {
        switch (kind) {
            case ValueKind::kOwned:
                return "owned bsonObject";
            case ValueKind::kRecord:
                return "record";
            case ValueKind::kSbeObject:
                return "Object";
        }
        MONGO_UNREACHABLE;
    }

    value::OwnedValueAccessor* makeAccessor(value::TagValueOwned value) {
        auto& accessor = _ownedAccessors.emplace_back();
        accessor.reset(std::move(value));
        return &accessor;
    }

    value::OwnedValueAccessor* makeAccessor(TypedValue ownedValue) {
        return makeAccessor(value::TagValueOwned::fromRaw(ownedValue));
    }

    /**
     * A view of 'obj', which the fixture keeps alive, or Nothing.
     */
    value::TagValueView viewOf(const boost::optional<BSONObj>& obj) {
        if (!obj) {
            return value::TagValueView::nothing();
        }
        const BSONObj& stored = _bsonObjs.emplace_back(*obj);
        return {value::TypeTags::bsonObject, value::bitcastFrom<const char*>(stored.objdata())};
    }

    value::SlotAccessor* makeBase(const boost::optional<BSONObj>& base, BaseKind kind) {
        switch (kind) {
            case BaseKind::kOwnedBson:
                return makeAccessor(base ? makeBsonObject(*base) : makeNothing());
            case BaseKind::kUnownedBson: {
                auto& accessor = _ownedAccessors.emplace_back();
                accessor.reset(viewOf(base));
                return &accessor;
            }
            case BaseKind::kRecord: {
                auto& accessor = _recordAccessors.emplace_back();
                accessor.reset(viewOf(base));
                return &accessor;
            }
            case BaseKind::kSbeObject:
                return makeAccessor(base ? makeObject(*base) : makeNothing());
            case BaseKind::kMaterializedRow: {
                auto& row = _rows.emplace_back(1);
                row.reset(
                    0, value::TagValueOwned::fromRaw(base ? makeBsonObject(*base) : makeNothing()));
                return &_rowAccessors.emplace_back(row, 0);
            }
        }
        MONGO_UNREACHABLE;
    }

    value::SlotAccessor* makeFieldAccessor(const Value& value, ValueKind kind) {
        if (value.missing()) {
            return makeAccessor(makeNothing());
        }
        if (value.getType() != BSONType::object) {
            BSONObjBuilder bob;
            value.addToBsonObj(&bob, "");
            BSONObj wrapper = bob.obj();
            return makeAccessor(bson::convertToOwned(wrapper.firstElement()));
        }

        BSONObj obj = value.getDocument().toBson();
        switch (kind) {
            case ValueKind::kOwned:
                return makeAccessor(makeBsonObject(obj));
            case ValueKind::kRecord: {
                auto& accessor = _recordAccessors.emplace_back();
                accessor.reset(viewOf(obj));
                return &accessor;
            }
            case ValueKind::kSbeObject:
                return makeAccessor(makeObject(obj));
        }
        MONGO_UNREACHABLE;
    }

    /**
     * Evaluates makeBsonObj() with the MakeObjSpec equivalent to 'closed' and 'changes' over the
     * value of 'base', reading the changed values from 'fieldAccessors'.
     */
    BSONObj runMakeBsonObj(value::SlotAccessor* base,
                           bool closed,
                           const std::vector<Change>& changes,
                           const std::vector<value::SlotAccessor*>& fieldAccessors) {
        std::vector<std::string> names;
        std::vector<MakeObjSpec::FieldAction> actions;
        EExpression::Vector argExprs;
        for (size_t i = 0; i < changes.size(); ++i) {
            names.push_back(changes[i].name);
            switch (changes[i].action) {
                case ResultFieldAction::kKeep:
                    actions.emplace_back(MakeObjSpec::Keep{});
                    break;
                case ResultFieldAction::kDrop:
                    actions.emplace_back(MakeObjSpec::Drop{});
                    break;
                case ResultFieldAction::kSet:
                    actions.emplace_back(MakeObjSpec::SetArg{argExprs.size()});
                    argExprs.push_back(makeE<EVariable>(bindAccessor(fieldAccessors[i])));
                    break;
                case ResultFieldAction::kAdd:
                    actions.emplace_back(MakeObjSpec::AddArg{argExprs.size()});
                    argExprs.push_back(makeE<EVariable>(bindAccessor(fieldAccessors[i])));
                    break;
            }
        }

        auto spec = std::make_unique<MakeObjSpec>(closed ? MakeObjSpec::FieldListScope::kClosed
                                                         : MakeObjSpec::FieldListScope::kOpen,
                                                  std::move(names),
                                                  std::move(actions),
                                                  MakeObjSpec::NonObjInputBehavior::kNewObj,
                                                  0 /* traversalDepth */);

        EExpression::Vector fnArgs;
        fnArgs.push_back(
            makeC(value::TypeTags::makeObjSpec, value::bitcastFrom<MakeObjSpec*>(spec.release())));
        fnArgs.push_back(makeE<EVariable>(bindAccessor(base)));
        std::move(argExprs.begin(), argExprs.end(), std::back_inserter(fnArgs));

        auto expr = makeE<EFunction>(EFn::kMakeBsonObj, std::move(fnArgs));
        auto code = compileExpression(*expr);
        auto result = value::TagValueOwned::fromRaw(runCompiledExpression(code.get()));
        ASSERT_EQ(result.tag(), value::TypeTags::bsonObject);
        return BSONObj(value::bitcastTo<const char*>(result.value())).getOwned();
    }

    /**
     * Checks the results of a ResultMaterializer for a Case, with the base exposed through a
     * 'baseKind' slot and the object-valued fields through 'valueKind' slots: the BSON results are
     * byte-identical to 'c.expected' and to makeBsonObj() with the equivalent MakeObjSpec, the
     * Document result has the same content, and every field named in the base or the change list
     * reads from it correctly.
     */
    void check(const Case& c, BaseKind baseKind, ValueKind valueKind) {
        const std::string context = str::stream()
            << "case '" << c.name << "', base " << toString(baseKind) << ", object values "
            << toString(valueKind);

        auto* base = makeBase(c.base, baseKind);
        std::vector<value::SlotAccessor*> fieldAccessors;
        std::vector<ResultField> fields;
        for (const auto& change : c.changes) {
            value::SlotAccessor* accessor = nullptr;
            if (change.action == ResultFieldAction::kSet ||
                change.action == ResultFieldAction::kAdd) {
                accessor = makeFieldAccessor(change.value, valueKind);
            }
            fieldAccessors.push_back(accessor);
            fields.push_back({change.name, change.action, accessor});
        }

        BSONObj fromMakeBsonObj = runMakeBsonObj(base, c.closed, c.changes, fieldAccessors);
        ASSERT_TRUE(fromMakeBsonObj.binaryEqual(c.expected))
            << context << ": makeBsonObj() returned " << fromMakeBsonObj;

        ResultMaterializer materializer(base, c.closed, std::move(fields));
        ASSERT_EQ(materializer.hasChanges(), c.closed || !c.changes.empty()) << context;

        BSONObj owned = materializer.toBson(true /* mustOwn */);
        ASSERT_TRUE(owned.isOwned()) << context;
        ASSERT_TRUE(owned.binaryEqual(c.expected))
            << context << ": toBson(true) returned " << owned;

        BSONObj unowned = materializer.toBson(false /* mustOwn */);
        ASSERT_TRUE(unowned.binaryEqual(c.expected))
            << context << ": toBson(false) returned " << unowned;
        if (!materializer.hasChanges() && c.base && baseKind != BaseKind::kSbeObject) {
            // Without changes the result is the base object itself, not a copy of it.
            ASSERT_TRUE(unowned.objdata() ==
                        value::bitcastTo<const char*>(base->getViewOfValue().value))
                << context;
        }

        Document doc = materializer.toDocument();
        ASSERT_TRUE(doc.isOwned()) << context;
        ASSERT_TRUE(doc.toBson().binaryEqual(c.expected))
            << context << ": toDocument() returned " << doc.toBson();

        // Every field named in the base or the change list reads from the Document as it does from
        // the expected BSON: its first occurrence, or missing.
        auto checkField = [&](std::string_view name) {
            Value actual = doc.getField(name);
            Value expected{c.expected[name]};
            ASSERT_TRUE(ValueComparator().evaluate(actual == expected))
                << context << ": getField(" << name << ") returned " << actual << ", expected "
                << expected;
        };
        if (c.base) {
            for (auto&& elem : *c.base) {
                checkField(elem.fieldNameStringData());
            }
        }
        for (const auto& change : c.changes) {
            checkField(change.name);
        }

        // Building the Document result leaves the BSON result unchanged.
        ASSERT_TRUE(materializer.toBson(true).binaryEqual(c.expected)) << context;
    }

    // Accessors, rows, and viewed objects live in deques so that their addresses stay stable.
    std::deque<value::OwnedValueAccessor> _ownedAccessors;
    std::deque<value::BSONObjValueAccessor> _recordAccessors;
    std::deque<value::MaterializedRow> _rows;
    std::deque<value::MaterializedSingleRowAccessor> _rowAccessors;
    std::deque<BSONObj> _bsonObjs;
};

TEST_F(ResultMaterializerTest, Table) {
    const BSONObj base = BSON("_id" << 1 << "a" << 2 << "b" << BSON("c" << 3) << "d" << 4);
    const BSONObj duplicates = BSON("a" << 1 << "a" << 2 << "b" << 3);
    const BSONObj empty;

    const std::vector<Case> kCases = {
        // Open results keep the base fields that are not listed.
        {"open no changes", base, false, {}, base},
        {"open keep",
         base,
         false,
         {keep("a"), set("x", 1)},
         BSON("_id" << 1 << "a" << 2 << "b" << BSON("c" << 3) << "d" << 4 << "x" << 1)},
        {"open drop",
         base,
         false,
         {drop("a"), drop("d"), drop("missing")},
         BSON("_id" << 1 << "b" << BSON("c" << 3))},
        {"open set existing",
         base,
         false,
         {set("a", 10), set("b", BSON("e" << 5))},
         BSON("_id" << 1 << "a" << 10 << "b" << BSON("e" << 5) << "d" << 4)},
        {"open set new",
         base,
         false,
         {set("x", 10), set("y", std::string("a long string"))},
         BSON("_id" << 1 << "a" << 2 << "b" << BSON("c" << 3) << "d" << 4 << "x" << 10 << "y"
                    << "a long string")},
        {"open set nothing",
         base,
         false,
         {set("a", Value()), set("x", Value())},
         BSON("_id" << 1 << "b" << BSON("c" << 3) << "d" << 4)},
        {"open add existing",
         base,
         false,
         {add("a", 10), set("x", 11)},
         BSON("_id" << 1 << "b" << BSON("c" << 3) << "d" << 4 << "a" << 10 << "x" << 11)},
        {"open add new",
         base,
         false,
         {add("x", 10), add("y", 11)},
         BSON("_id" << 1 << "a" << 2 << "b" << BSON("c" << 3) << "d" << 4 << "x" << 10 << "y"
                    << 11)},
        {"open add nothing",
         base,
         false,
         {add("a", Value()), add("x", Value())},
         BSON("_id" << 1 << "b" << BSON("c" << 3) << "d" << 4)},
        {"open mixed",
         base,
         false,
         {drop("_id"), set("x", 1), set("b", 2), add("y", 3), drop("d")},
         BSON("a" << 2 << "b" << 2 << "x" << 1 << "y" << 3)},
        {"open set objects and array",
         base,
         false,
         {set("b", BSON("p" << 1 << "q" << BSON_ARRAY(1 << 2))),
          add("o", BSONObj()),
          set("x", BSON_ARRAY(1 << "two" << BSON("t" << 3)))},
         BSON("_id" << 1 << "a" << 2 << "b" << BSON("p" << 1 << "q" << BSON_ARRAY(1 << 2)) << "d"
                    << 4 << "o" << BSONObj() << "x" << BSON_ARRAY(1 << "two" << BSON("t" << 3)))},
        // Like makeBsonObj(), only the first occurrence of a duplicate field is replaced.
        {"open duplicate base field names",
         duplicates,
         false,
         {set("a", 10)},
         BSON("a" << 10 << "a" << 2 << "b" << 3)},
        {"open empty base", empty, false, {drop("a"), set("x", 1)}, BSON("x" << 1)},
        {"open nothing base", boost::none, false, {drop("a"), set("x", 1)}, BSON("x" << 1)},

        // Closed results drop the base fields that are not listed.
        {"closed no fields", base, true, {}, empty},
        {"closed keep",
         base,
         true,
         {keep("_id"), keep("b"), keep("missing")},
         BSON("_id" << 1 << "b" << BSON("c" << 3))},
        {"closed drop", base, true, {keep("a"), drop("b")}, BSON("a" << 2)},
        {"closed set existing",
         base,
         true,
         {keep("_id"), set("b", 10)},
         BSON("_id" << 1 << "b" << 10)},
        {"closed set new", base, true, {keep("a"), set("x", 10)}, BSON("a" << 2 << "x" << 10)},
        {"closed set nothing", base, true, {keep("a"), set("b", Value())}, BSON("a" << 2)},
        {"closed add",
         base,
         true,
         {add("a", 10), keep("d"), add("x", 1)},
         BSON("d" << 4 << "a" << 10 << "x" << 1)},
        {"closed add nothing", base, true, {keep("a"), add("x", Value())}, BSON("a" << 2)},
        {"closed set objects",
         base,
         true,
         {keep("_id"), set("b", BSON("e" << 5)), add("o", BSON("f" << 6))},
         BSON("_id" << 1 << "b" << BSON("e" << 5) << "o" << BSON("f" << 6))},
        // Both occurrences of the kept field count towards the fields searched for, so the set
        // field is appended without searching the rest of the base.
        {"closed duplicate base field names",
         duplicates,
         true,
         {keep("a"), set("b", 10)},
         BSON("a" << 1 << "a" << 2 << "b" << 10)},
        // The shape of a $group result.
        {"closed empty base",
         empty,
         true,
         {add("_id", 1), add("count", 2), add("none", Value())},
         BSON("_id" << 1 << "count" << 2)},
        {"closed nothing base",
         boost::none,
         true,
         {set("a", 1), add("b", 2)},
         BSON("a" << 1 << "b" << 2)},
    };

    for (const auto& c : kCases) {
        for (auto baseKind : kAllBaseKinds) {
            // A record slot always holds a bsonObject.
            if (!c.base && baseKind == BaseKind::kRecord) {
                continue;
            }
            for (auto valueKind : kAllValueKinds) {
                check(c, baseKind, valueKind);
            }
        }
    }
}

TEST_F(ResultMaterializerTest, SharedRecordBaseIsCopiedOnce) {
    BSONObj record = BSON("_id" << 1 << "blob" << std::string(1000, 'x'));
    value::BSONObjValueAccessor base;
    base.reset(value::TagValueView{value::TypeTags::bsonObject,
                                   value::bitcastFrom<const char*>(record.objdata())});
    ResultMaterializer materializer(&base, false, {});

    // Without changes every result wraps the one owned copy of the record.
    Document first = materializer.toDocument();
    ASSERT_TRUE(first.isOwned());
    const char* shared = first.toBsonIfTriviallyConvertible()->objdata();
    ASSERT_NE(shared, record.objdata());
    ASSERT_EQ(base.getOwnedBSONObj().objdata(), shared);
    ASSERT_EQ(materializer.toDocument().toBsonIfTriviallyConvertible()->objdata(), shared);
    ASSERT_EQ(materializer.toBson(true).objdata(), shared);

    // With changes the results are built field by field from the record.
    // TODO SERVER-138113 improve result materializer efficiency by detecting cases when
    // we can reuse the bson as the base of the document and build fields in the field cache.
    auto* value = makeAccessor(makeInt32(10));
    ResultMaterializer withChanges(&base, false, {{"joined", ResultFieldAction::kSet, value}});
    Document joined = withChanges.toDocument();
    ASSERT_TRUE(joined.isOwned());
    auto expected = BSON("_id" << 1 << "blob" << std::string(1000, 'x') << "joined" << 10);
    ASSERT_BSONOBJ_EQ(joined.toBson(), expected);

    // A new record drops the stage's reference but the results keep theirs.
    BSONObj next = BSON("_id" << 2);
    base.reset(value::TagValueView{value::TypeTags::bsonObject,
                                   value::bitcastFrom<const char*>(next.objdata())});
    ASSERT_BSONOBJ_EQ(first.toBson(), record);
    ASSERT_BSONOBJ_EQ(joined.toBson(), expected);
    ASSERT_BSONOBJ_EQ(materializer.toDocument().toBson(), next);
    ASSERT_BSONOBJ_EQ(withChanges.toDocument().toBson(), BSON("_id" << 2 << "joined" << 10));
}

TEST_F(ResultMaterializerTest, SharedRecordFieldIsCopiedOnce) {
    BSONObj foreign = BSON("_id" << 7 << "b" << 1);
    value::BSONObjValueAccessor embedded;
    embedded.reset(value::TagValueView{value::TypeTags::bsonObject,
                                       value::bitcastFrom<const char*>(foreign.objdata())});
    auto* base = makeAccessor(makeBsonObject(BSON("_id" << 1)));
    ResultMaterializer materializer(base, false, {{"f", ResultFieldAction::kSet, &embedded}});

    Document first = materializer.toDocument();
    Document second = materializer.toDocument();
    const char* shared = embedded.getOwnedBSONObj().objdata();
    ASSERT_EQ(first["f"].getDocument().toBsonIfTriviallyConvertible()->objdata(), shared);
    ASSERT_EQ(second["f"].getDocument().toBsonIfTriviallyConvertible()->objdata(), shared);
    ASSERT_BSONOBJ_EQ(first["f"].getDocument().toBson(), foreign);
}

TEST_F(ResultMaterializerTest, OwnedBaseIsCopied) {
    BSONObj obj = BSON("_id" << 1 << "a" << 2);
    auto* base = makeAccessor(makeBsonObject(obj));
    const char* data = value::bitcastTo<const char*>(base->getViewOfValue().value);
    ResultMaterializer materializer(base, false, {});

    // The stage keeps its value: each result is a copy of it.
    Document doc = materializer.toDocument();
    ASSERT_TRUE(doc.isOwned());
    ASSERT_NE(doc.toBsonIfTriviallyConvertible()->objdata(), data);
    ASSERT_NE(materializer.toBson(true).objdata(), data);

    // The results outlive the stage's value.
    base->reset(value::TagValueOwned::fromRaw(makeBsonObject(BSON("_id" << 2))));
    ASSERT_BSONOBJ_EQ(doc.toBson(), obj);
    ASSERT_BSONOBJ_EQ(materializer.toDocument().toBson(), BSON("_id" << 2));
}

TEST_F(ResultMaterializerTest, MergeJoinBufferedRowsCanBeRevisited) {
    // A merge join exposes its buffered outer rows through one accessor and revisits each of them
    // for every matching inner row.
    std::vector<BSONObj> objs{BSON("_id" << 1), BSON("_id" << 2)};
    std::vector<value::MaterializedRow> buffer;
    for (const auto& obj : objs) {
        buffer.emplace_back(1).reset(0, value::TagValueOwned::fromRaw(makeBsonObject(obj)));
    }
    size_t bufferIt = 0;
    value::MaterializedRowAccessor<std::vector<value::MaterializedRow>> base(buffer, bufferIt, 0);
    ResultMaterializer materializer(&base, false, {});

    // Rows (A, I1), (B, I1), (A, I2), each released before the next one is built.
    for (size_t row : {0, 1, 0}) {
        bufferIt = row;
        ASSERT_BSONOBJ_EQ(materializer.toDocument().toBson(), objs[row]);
    }
}

TEST_F(ResultMaterializerTest, SortRowOutlivesTheMaterializer) {
    // A sort under a loop join keeps exposing the same output row for every inner row, including
    // after the plan saves its state, which the sort ignores. So the row must not depend on the
    // materializer or its results to keep its value.
    BSONObj obj = BSON("_id" << 1 << "a" << 2);
    std::pair<value::MaterializedRow, value::MaterializedRow> outputData;
    outputData.second.resize(1);
    outputData.second.reset(0, value::TagValueOwned::fromRaw(makeBsonObject(obj)));
    auto* outputDataIt = &outputData;
    value::MaterializedRowValueAccessor<decltype(outputDataIt)> base(outputDataIt, 0);

    {
        ResultMaterializer materializer(&base, false, {});
        ASSERT_BSONOBJ_EQ(materializer.toDocument().toBson(), obj);
        ASSERT_BSONOBJ_EQ(materializer.toDocument().toBson(), obj);
    }

    ResultMaterializer materializer(&base, false, {});
    ASSERT_BSONOBJ_EQ(materializer.toDocument().toBson(), obj);
}

TEST_F(ResultMaterializerTest, UnownedBaseIsCopied) {
    BSONObj obj = BSON("_id" << 1);
    value::OwnedValueAccessor base;
    base.reset(value::TagValueView{value::TypeTags::bsonObject,
                                   value::bitcastFrom<const char*>(obj.objdata())});
    ResultMaterializer materializer(&base, false, {});

    Document doc = materializer.toDocument();
    ASSERT_TRUE(doc.isOwned());
    ASSERT_NE(doc.toBsonIfTriviallyConvertible()->objdata(), obj.objdata());
    ASSERT_BSONOBJ_EQ(doc.toBson(), obj);

    // Without 'mustOwn' the BSON result is a view of the base.
    ASSERT_EQ(materializer.toBson(false).objdata(), obj.objdata());
}

}  // namespace
}  // namespace mongo::sbe
