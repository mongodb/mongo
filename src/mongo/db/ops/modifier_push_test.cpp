/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/db/ops/modifier_push.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/document.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/ops/log_builder.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

namespace {

using mongo::BSONObj;
using mongo::BSONObjBuilder;
using mongo::BSONArrayBuilder;
using mongo::CollatorInterfaceMock;
using mongo::fromjson;
using mongo::LogBuilder;
using mongo::ModifierInterface;
using mongo::ModifierPush;
using mongo::NumberInt;
using mongo::Ordering;
using mongo::Status;
using mongo::StringData;
using mongo::mutablebson::ConstElement;
using mongo::mutablebson::countChildren;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;
using std::sort;
using std::vector;

namespace dps = ::mongo::dotted_path_support;

void combineVec(const vector<int>& origVec,
                const vector<int>& modVec,
                int32_t slice,
                vector<int>* combined) {
    using namespace std;
    combined->clear();

    // Slice 0 means the result is empty
    if (slice == 0)
        return;

    // Combine both vectors
    *combined = origVec;
    combined->insert(combined->end(), modVec.begin(), modVec.end());

    // Remove sliced items
    bool removeFromFront = (slice < 0);

    // if abs(slice) is larger than the size, nothing to do.
    if (abs(slice) >= int32_t(combined->size()))
        return;

    if (removeFromFront) {
        // Slice is negative.
        int32_t removeCount = combined->size() + slice;
        combined->erase(combined->begin(), combined->begin() + removeCount);
    } else {
        combined->resize(std::min(combined->size(), size_t(slice)));
    }
}

/**
 * Comparator between two BSONObjects that takes in consideration only the keys and
 * direction described in the sort pattern.
 */
struct ProjectKeyCmp {
    BSONObj sortPattern;
    bool useWholeValue;

    ProjectKeyCmp(BSONObj pattern) : sortPattern(pattern) {
        useWholeValue = pattern.hasField("");
    }

    int operator()(const BSONObj& left, const BSONObj& right) const {
        int ret = 0;
        if (useWholeValue) {
            ret = left.woCompare(right, Ordering::make(sortPattern), false);
        } else {
            BSONObj lhsKey = dps::extractElementsBasedOnTemplate(left, sortPattern, true);
            BSONObj rhsKey = dps::extractElementsBasedOnTemplate(right, sortPattern, true);
            ret = lhsKey.woCompare(rhsKey, sortPattern);
        }
        return ret < 0;
    }
};

void combineAndSortVec(const vector<BSONObj>& origVec,
                       const vector<BSONObj>& modVec,
                       int32_t slice,
                       BSONObj sortOrder,
                       vector<BSONObj>* combined) {
    combined->clear();

    // Slice 0 means the result is empty
    if (slice == 0)
        return;

    *combined = origVec;
    combined->insert(combined->end(), modVec.begin(), modVec.end());

    sort(combined->begin(), combined->end(), ProjectKeyCmp(sortOrder));

    // Remove sliced items
    bool removeFromFront = (slice < 0);

    // if abs(slice) is larger than the size, nothing to do.
    if (abs(slice) >= int32_t(combined->size()))
        return;

    if (removeFromFront) {
        // Slice is negative.
        int32_t removeCount = combined->size() + slice;
        combined->erase(combined->begin(), combined->begin() + removeCount);
    } else {
        combined->resize(std::min(combined->size(), size_t(slice)));
    }
}

//
// Init testing (module field checking, which is done in 'fieldchecker'
//

TEST(Init, SimplePush) {
    BSONObj modObj = fromjson("{$push: {x: 0}}");
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

//
// If present, is the $each clause valid?
//

TEST(Init, PushEachNormal) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2]}}}");
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachMixed) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, {a: 2}]}}}");
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachObject) {
    // $each must be an array
    BSONObj modObj = fromjson("{$push: {x: {$each: {'0': 1}}}}");
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachSimpleType) {
    // $each must be an array.
    BSONObj modObj = fromjson("{$push: {x: {$each: 1}}}");
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachEmpty) {
    BSONObj modObj = fromjson("{$push: {x: {$each: []}}}");
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachInvalidType) {
    // $each must be an array.
    BSONObj modObj = fromjson("{$push: {x: {$each: {b: 1}}}}");
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

//
// If present, is the $slice clause valid?
//

TEST(Init, PushEachWithSliceBottom) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2], $slice: -3}}}");
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithSliceTop) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2], $slice: 3}}}");
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithInvalidSliceObject) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2], $slice: {a: 1}}}}");
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithInvalidSliceDouble) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2], $slice: -2.1}}}");
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithValidSliceDouble) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2], $slice: -2.0}}}");
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithUnsupportedFullSlice) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2], $slice: [1,2]}}}");
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithWrongTypeSlice) {
    BSONObj modObj = fromjson("{$push: {x: {$each: [1, 2], $slice: '-1'}}}");
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

//
// If present, is the sort $sort clause valid?
//

TEST(Init, PushEachWithObjectSort) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {a:1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithNumbericSort) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort:1 }}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithInvalidSortType) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: [{a:1}]}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachDuplicateSortPattern) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: [{a:1,a:1}]}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithInvalidSortValue) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {a:100}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithEmptySortField) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {'':1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithEmptyDottedSortField) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {'.':1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithMissingSortFieldSuffix) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {'a.':1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithMissingSortFieldPreffix) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {'.b':1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithMissingSortFieldMiddle) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {'a..b':1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithEmptySort) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort:{} }}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

//
// If in $pushAll semantics, do we check the array and that nothing else is there?
//

TEST(Init, PushAllSimple) {
    BSONObj modObj = fromjson("{$pushAll: {x: [0]}}");
    ModifierPush mod(ModifierPush::PUSH_ALL);
    ASSERT_OK(mod.init(modObj["$pushAll"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushAllMultiple) {
    BSONObj modObj = fromjson("{$pushAll: {x: [1,2,3]}}");
    ModifierPush mod(ModifierPush::PUSH_ALL);
    ASSERT_OK(mod.init(modObj["$pushAll"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushAllObject) {
    BSONObj modObj = fromjson("{$pushAll: {x: [{a:1},{a:2}]}}");
    ModifierPush mod(ModifierPush::PUSH_ALL);
    ASSERT_OK(mod.init(modObj["$pushAll"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushAllMixed) {
    BSONObj modObj = fromjson("{$pushAll: {x: [1,{a:2}]}}");
    ModifierPush mod(ModifierPush::PUSH_ALL);
    ASSERT_OK(mod.init(modObj["$pushAll"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushAllWrongType) {
    BSONObj modObj = fromjson("{$pushAll: {x: 1}}");
    ModifierPush mod(ModifierPush::PUSH_ALL);
    ASSERT_NOT_OK(mod.init(modObj["$pushAll"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushAllNotArray) {
    BSONObj modObj = fromjson("{$pushAll: {x: {a:1}}}");
    ModifierPush mod(ModifierPush::PUSH_ALL);
    ASSERT_NOT_OK(mod.init(modObj["$pushAll"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

//
// Are all clauses present? Is anything extroneous? Is anything duplicated?
//

TEST(Init, PushEachWithSortMissingSlice) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $sort:{a:1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachInvalidClause) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $xxx: -1, $sort:{a:1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachExtraField) {
    const char* c = "{$push: {x: {$each: [{a:1},{a:2}], $slice: -2.0, $sort: {a:1}, b: 1}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachDuplicateSortClause) {
    const char* c = "{$push: {x:{$each:[{a:1},{a:2}], $slice:-2.0, $sort:{a:1}, $sort:{a:1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachDuplicateSliceClause) {
    const char* c = "{$push: {x: {$each:[{a:1},{a:2}], $slice:-2.0, $slice:-2, $sort:{a:1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachDuplicateEachClause) {
    const char* c = "{$push: {x: {$each:[{a:1}], $each:[{a:2}], $slice:-3, $sort:{a:1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_NOT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                           ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithSliceFirst) {
    const char* c = "{$push: {x: {$slice: -2.0, $each: [{a:1},{a:2}], $sort: {a:1}}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

TEST(Init, PushEachWithSortFirst) {
    const char* c = "{$push: {x: {$sort: {a:1}, $slice: -2.0, $each: [{a:1},{a:2}]}}}";
    BSONObj modObj = fromjson(c);
    ModifierPush mod;
    ASSERT_OK(mod.init(modObj["$push"].embeddedObject().firstElement(),
                       ModifierInterface::Options::normal()));
}

//
// Simple mod
//

/** Helper to build and manipulate a $push or a $pushAll mod. */
class Mod {
public:
    Mod() : _mod() {}

    explicit Mod(BSONObj modObj,
                 ModifierInterface::Options options = ModifierInterface::Options::normal())
        : _mod(mongoutils::str::equals(modObj.firstElement().fieldName(), "$pushAll")
                   ? ModifierPush::PUSH_ALL
                   : ModifierPush::PUSH_NORMAL) {
        _modObj = modObj;
        StringData modName = modObj.firstElement().fieldName();
        ASSERT_OK(_mod.init(_modObj[modName].embeddedObject().firstElement(), options));
    }

    Status prepare(Element root, StringData matchedField, ModifierInterface::ExecInfo* execInfo) {
        return _mod.prepare(root, matchedField, execInfo);
    }

    Status apply() const {
        return _mod.apply();
    }

    Status log(LogBuilder* logBuilder) const {
        return _mod.log(logBuilder);
    }

    ModifierPush& mod() {
        return _mod;
    }

private:
    ModifierPush _mod;
    BSONObj _modObj;
};

TEST(SimpleMod, PrepareNonArray) {
    Document doc(fromjson("{a: 1}"));
    Mod pushMod(fromjson("{$push: {a: 1}}"));

    ModifierInterface::ExecInfo dummy;
    ASSERT_NOT_OK(pushMod.prepare(doc.root(), "", &dummy));
}

TEST(SimpleMod, PrepareApplyEmpty) {
    Document doc(fromjson("{a: []}"));
    Mod pushMod(fromjson("{$push: {a: 1}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [1]}}"), logDoc);
}

TEST(SimpleMod, PrepareApplyInexistent) {
    Document doc(fromjson("{}"));
    Mod pushMod(fromjson("{$push: {a: 1}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [1]}}"), logDoc);
}

TEST(SimpleMod, PrepareApplyNormal) {
    Document doc(fromjson("{a: [0]}"));
    Mod pushMod(fromjson("{$push: {a: 1}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [0,1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.1':1}}"), logDoc);
}

//
// Simple object mod
//

TEST(SimpleObjMod, PrepareNonArray) {
    Document doc(fromjson("{a: 1}"));
    Mod pushMod(fromjson("{$push: {a: {b: 1}}}"));

    ModifierInterface::ExecInfo dummy;
    ASSERT_NOT_OK(pushMod.prepare(doc.root(), "", &dummy));
}

TEST(SimpleObjMod, PrepareApplyEmpty) {
    Document doc(fromjson("{a: []}"));
    Mod pushMod(fromjson("{$push: {a: {b: 1}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [{b:1}]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [{b:1}]}}"), logDoc);
}

TEST(SimpleObjMod, PrepareApplyInexistent) {
    Document doc(fromjson("{}"));
    Mod pushMod(fromjson("{$push: {a: {b: 1}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [{b:1}]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [{b:1}]}}"), logDoc);
}

TEST(SimpleObjMod, PrepareApplyNormal) {
    Document doc(fromjson("{a: [{b:0}]}"));
    Mod pushMod(fromjson("{$push: {a: {b: 1}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [{b:0},{b:1}]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.1':{b:1}}}"), logDoc);
}

TEST(SimpleObjMod, PrepareApplyDotted) {
    Document doc(
        fromjson("{ _id : 1 , "
                 "  question : 'a', "
                 "  choices : { "
                 "            first : { choice : 'b' }, "
                 "            second : { choice : 'c' } }"
                 "}"));
    Mod pushMod(fromjson("{$push: {'choices.first.votes': 1}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "choices.first.votes");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ _id : 1 , "
                           "  question : 'a', "
                           "  choices : { "
                           "            first : { choice : 'b', votes: [1]}, "
                           "            second : { choice : 'c' } }"
                           "}"),
                  doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'choices.first.votes':[1]}}"), logDoc);
}


//
// $pushAll Variation
//

TEST(PushAll, PrepareApplyEmpty) {
    Document doc(fromjson("{a: []}"));
    Mod pushMod(fromjson("{$pushAll: {a: [1]}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{a: [1]}"));

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(logDoc, fromjson("{$set: {a: [1]}}"));
}

TEST(PushAll, PrepareApplyInexistent) {
    Document doc(fromjson("{}"));
    Mod pushMod(fromjson("{$pushAll: {a: [1]}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{a: [1]}"));

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(logDoc, fromjson("{$set: {a: [1]}}"));
}

TEST(PushAll, PrepareApplyNormal) {
    Document doc(fromjson("{a: [0]}"));
    Mod pushMod(fromjson("{$pushAll: {a: [1,2]}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, fromjson("{a: [0,1,2]}"));

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(logDoc, fromjson("{$set: {'a.1': 1, 'a.2':2}}"));
}

//
// Simple $each mod
//

TEST(SimpleEachMod, PrepareNonArray) {
    Document doc(fromjson("{a: 1}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [1]}}}"));

    ModifierInterface::ExecInfo dummy;
    ASSERT_NOT_OK(pushMod.prepare(doc.root(), "", &dummy));
}

TEST(SimpleEachMod, PrepareApplyEmpty) {
    Document doc(fromjson("{a: []}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [1]}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [1]}}"), logDoc);
}

TEST(SimpleEachMod, PrepareApplyInexistent) {
    Document doc(fromjson("{}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [1]}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [1]}}"), logDoc);
}

TEST(SimpleEachMod, PrepareApplyInexistentMultiple) {
    Document doc(fromjson("{}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [1, 2]}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1, 2]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [1, 2]}}"), logDoc);
}

TEST(SimpleEachMod, PrepareApplyNormal) {
    Document doc(fromjson("{a: [0]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [1]}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [0,1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.1': 1}}"), logDoc);
}

TEST(SimpleEachMod, PrepareApplyNormalMultiple) {
    Document doc(fromjson("{a: [0]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [1,2]}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [0,1,2]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.1': 1, 'a.2':2}}"), logDoc);
}

/**
 * Slice variants
 */
TEST(SlicePushEach, TopOne) {
    Document doc(fromjson("{a: [3]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [2, -1], $slice:1}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [3]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [3]}}"), logDoc);
}

/**
 * Sort for scalar (whole) array elements
 */
TEST(SortPushEach, NumberSort) {
    Document doc(fromjson("{a: [3]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [2, -1], $sort:1}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [-1,2,3]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [-1, 2, 3]}}"), logDoc);
}

TEST(SortPushEach, NumberSortReverse) {
    Document doc(fromjson("{a: [3]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [4, -1], $sort:-1}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [4,3,-1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: [4,3,-1]}}"), logDoc);
}

TEST(SortPushEach, MixedSortWhole) {
    Document doc(fromjson("{a: [3, 't', {b:1}, {a:1}]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [4, -1], $sort:1}}}"));
    const BSONObj expectedObj = fromjson("{a: [-1,3,4,'t', {a:1}, {b:1}]}");

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(expectedObj, doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(BSON("$set" << expectedObj), logDoc);
}

TEST(SortPushEach, MixedSortWholeReverse) {
    Document doc(fromjson("{a: [3, 't', {b:1}, {a:1}]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [4, -1], $sort:-1}}}"));
    const BSONObj expectedObj = fromjson("{a: [{b:1}, {a:1}, 't', 4, 3, -1]}");

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(expectedObj, doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(BSON("$set" << expectedObj), logDoc);
}

TEST(SortPushEach, MixedSortEmbeddedField) {
    Document doc(fromjson("{a: [3, 't', {b:1}, {a:1}]}"));
    Mod pushMod(fromjson("{$push: {a: {$each: [4, -1], $sort:{a:1}}}}"));
    const BSONObj expectedObj = fromjson("{a: [3, 't', {b: 1}, 4, -1, {a: 1}]}");

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(expectedObj, doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(BSON("$set" << expectedObj), logDoc);
}

TEST(SortPushEach, SortRespectsCollationFromOptions) {
    Document doc(fromjson("{a: ['dd', 'fc', 'gb'] }"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Mod pushMod(fromjson("{$push: {a: {$each: ['ha'], $sort: 1}}}"),
                ModifierInterface::Options::normal(&collator));
    const BSONObj expectedObj = fromjson("{a: ['ha', 'gb', 'fc', 'dd']}");

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_EQUALS(expectedObj, doc);
}

TEST(SortPushEach, SortRespectsCollationFromSetCollator) {
    Document doc(fromjson("{a: ['dd', 'fc', 'gb'] }"));
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    Mod pushMod(fromjson("{$push: {a: {$each: ['ha'], $sort: 1}}}"));
    pushMod.mod().setCollator(&collator);
    const BSONObj expectedObj = fromjson("{a: ['ha', 'gb', 'fc', 'dd']}");

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_EQUALS(expectedObj, doc);
}

/**
 * This fixture supports building $push mods with parameterized $each arrays and $slices.
 * It always assume that the array being operated on is called 'a'. To build a mod, one
 * issues a set*Mod() call.
 *
 * The setSimpleMod() call will build a $each array of numbers. The setObjectMod() call
 * will build a $each array with object. Both these calls take the slice as a parameter as
 * well.
 *
 * Here's a typical test case flow:
 *  + Determine what the original document's 'a' array would contain
 *  + Ditto for the $push's $each arrray
 *  + Loop over slice value
 *    + Apply the $push with current slice value to the doc
 *    + Use the fixture/helpers to combine and slice the mod's and original's 'a'
 *      array
 *    + Build a document with the above and check against the one generated by the mod apply
 */
class SlicedMod : public mongo::unittest::Test {
public:
    SlicedMod() : _mod() {}

    virtual void setUp() {
        // no op; set all state using the setMod() call
    }

    /** Sets up the mod to be {$push: {a: {$each: [<eachArray>], $slice: <slice>}}} */
    void setSimpleMod(int32_t slice, const vector<int>& eachArray) {
        BSONArrayBuilder arrBuilder;
        for (vector<int>::const_iterator it = eachArray.begin(); it != eachArray.end(); ++it) {
            arrBuilder.append(*it);
        }

        _modObj =
            BSON("$push" << BSON("a" << BSON("$each" << arrBuilder.arr() << "$slice" << slice)));

        ASSERT_OK(_mod.init(_modObj["$push"].embeddedObject().firstElement(),
                            ModifierInterface::Options::normal()));
    }

    /** Sets up the mod to be {$push: {a: {$each:[<Obj>,...], $slice:<slice>, $sort:<Obj>}}} */
    void setSortMod(int32_t slice, const vector<BSONObj>& eachArray, BSONObj sort) {
        BSONArrayBuilder arrBuilder;
        for (vector<BSONObj>::const_iterator it = eachArray.begin(); it != eachArray.end(); ++it) {
            arrBuilder.append(*it);
        }

        _modObj = BSON(
            "$push" << BSON(
                "a" << BSON("$each" << arrBuilder.arr() << "$slice" << slice << "$sort" << sort)));

        ASSERT_OK(_mod.init(_modObj["$push"].embeddedObject().firstElement(),
                            ModifierInterface::Options::normal()));
    }

    /** Returns an object {a: [<'vec's content>]} */
    BSONObj getObjectUsing(const vector<int>& vec) {
        BSONArrayBuilder arrBuilder;
        for (vector<int>::const_iterator it = vec.begin(); it != vec.end(); ++it) {
            arrBuilder.append(*it);
        }

        BSONObjBuilder builder;
        builder.appendArray("a", arrBuilder.obj());

        return builder.obj();
    }

    /** Returns an object {a: [<'vec's content>]} */
    BSONObj getObjectUsing(const vector<BSONObj>& vec) {
        BSONArrayBuilder arrBuilder;
        for (vector<BSONObj>::const_iterator it = vec.begin(); it != vec.end(); ++it) {
            arrBuilder.append(*it);
        }

        BSONObjBuilder builder;
        builder.appendArray("a", arrBuilder.obj());

        return builder.obj();
    }

    ModifierPush& mod() {
        return _mod;
    }

    BSONObj modObj() {
        return _modObj;
    }

private:
    ModifierPush _mod;
    BSONObj _modObj;
    vector<int> _eachArray;
};

TEST_F(SlicedMod, SimpleArrayFromEmpty) {
    // We'll simulate the original document having {a: []} and the mod being
    // {$push: {a: {$each: [1], $slice: <-2..0>}}}
    vector<int> docArray;
    vector<int> eachArray;
    eachArray.push_back(1);

    for (int32_t slice = -2; slice <= 0; slice++) {
        setSimpleMod(slice, eachArray);
        Document doc(getObjectUsing(docArray /* {a: []} */));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod().prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod().apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());

        vector<int> combinedVec;
        combineVec(docArray,  /* a: []  */
                   eachArray, /* a: [1] */
                   slice,
                   &combinedVec);
        ASSERT_EQUALS(getObjectUsing(combinedVec), doc);
    }
}

TEST_F(SlicedMod, SimpleArrayFromExisting) {
    // We'll simulate the original document having {a: [2,3]} and the mod being
    // {$push: {a: {$each: [1], $slice: <-4..0>}}}
    vector<int> docArray;
    docArray.push_back(2);
    docArray.push_back(3);
    vector<int> eachArray;
    eachArray.push_back(1);

    for (int32_t slice = -4; slice <= 0; slice++) {
        setSimpleMod(slice, eachArray);
        Document doc(getObjectUsing(docArray /* {a: [2, 3]} */));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(mod().prepare(doc.root(), "", &execInfo));

        ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
        ASSERT_FALSE(execInfo.noOp);

        ASSERT_OK(mod().apply());
        ASSERT_FALSE(doc.isInPlaceModeEnabled());

        vector<int> combinedVec;
        combineVec(docArray,  /* a: [2, 3] */
                   eachArray, /* a: [1] */
                   slice,
                   &combinedVec);
        ASSERT_EQUALS(getObjectUsing(combinedVec), doc);
    }
}

TEST_F(SlicedMod, ObjectArrayFromEmpty) {
    // We'll simulate the original document having {a: []} and the mod being
    // {$push: {a: {$each: [{a:2,b:1}], $slice: <-4..0>}, $sort: {a:-1/1,b:-1/1}}
    vector<BSONObj> docArray;
    vector<BSONObj> eachArray;
    eachArray.push_back(fromjson("{a:2,b:1}"));
    eachArray.push_back(fromjson("{a:1,b:2}"));

    for (int32_t aOrB = 0; aOrB < 2; aOrB++) {
        for (int32_t sortA = 0; sortA < 2; sortA++) {
            for (int32_t sortB = 0; sortB < 2; sortB++) {
                for (int32_t slice = -3; slice <= 3; slice++) {
                    BSONObj sortOrder;
                    if (aOrB == 0) {
                        sortOrder = BSON("a" << (sortA ? 1 : -1) << "b" << (sortB ? 1 : -1));
                    } else {
                        sortOrder = BSON("b" << (sortB ? 1 : -1) << "a" << (sortA ? 1 : -1));
                    }

                    setSortMod(slice, eachArray, sortOrder);
                    Document doc(getObjectUsing(docArray /* {a: []} */));

                    ModifierInterface::ExecInfo execInfo;
                    ASSERT_OK(mod().prepare(doc.root(), "", &execInfo));

                    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
                    ASSERT_FALSE(execInfo.noOp);

                    ASSERT_OK(mod().apply());
                    ASSERT_FALSE(doc.isInPlaceModeEnabled());

                    vector<BSONObj> combinedVec;
                    combineAndSortVec(docArray,  /* a: [] */
                                      eachArray, /* a: [{a:2,b:1},{a:1,b:2}] */
                                      slice,
                                      sortOrder,
                                      &combinedVec);
                    ASSERT_EQUALS(getObjectUsing(combinedVec), doc);

                    Document logDoc;
                    LogBuilder logBuilder(logDoc.root());
                    ASSERT_OK(mod().log(&logBuilder));
                    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
                    ASSERT_EQUALS(BSON("$set" << getObjectUsing(combinedVec)), logDoc);
                }
            }
        }
    }
}

TEST_F(SlicedMod, ObjectArrayFromExisting) {
    // We'll simulate the original document having {a: [{a:2,b:3},{a:3,:b1}]} and the mod being
    // {$push: {a: {$each: [{a:2,b:1}], $slice: <-4..0>}, $sort: {a:-1/1,b:-1/1}}
    vector<BSONObj> docArray;
    docArray.push_back(fromjson("{a:2,b:3}"));
    docArray.push_back(fromjson("{a:3,b:1}"));
    vector<BSONObj> eachArray;
    eachArray.push_back(fromjson("{a:2,b:1}"));

    for (int32_t aOrB = 0; aOrB < 2; aOrB++) {
        for (int32_t sortA = 0; sortA < 2; sortA++) {
            for (int32_t sortB = 0; sortB < 2; sortB++) {
                for (int32_t slice = -4; slice <= 4; slice++) {
                    BSONObj sortOrder;
                    if (aOrB == 0) {
                        sortOrder = BSON("a" << (sortA ? 1 : -1) << "b" << (sortB ? 1 : -1));
                    } else {
                        sortOrder = BSON("b" << (sortB ? 1 : -1) << "a" << (sortA ? 1 : -1));
                    }

                    setSortMod(slice, eachArray, sortOrder);
                    Document doc(getObjectUsing(docArray /* {a: [{a:2,b:b},{a:3,:b1}]} */));

                    ModifierInterface::ExecInfo execInfo;
                    ASSERT_OK(mod().prepare(doc.root(), "", &execInfo));

                    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
                    ASSERT_FALSE(execInfo.noOp);

                    ASSERT_OK(mod().apply());
                    ASSERT_FALSE(doc.isInPlaceModeEnabled());

                    vector<BSONObj> combinedVec;
                    combineAndSortVec(docArray,  /* a: [{a:2,b:3},{a:3,:b1}] */
                                      eachArray, /* a: [{a:2,b:1}] */
                                      slice,
                                      sortOrder,
                                      &combinedVec);
                    ASSERT_EQUALS(getObjectUsing(combinedVec), doc);

                    Document logDoc;
                    LogBuilder logBuilder(logDoc.root());
                    ASSERT_OK(mod().log(&logBuilder));
                    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
                    ASSERT_EQUALS(BSON("$set" << getObjectUsing(combinedVec)), logDoc);
                }
            }
        }
    }
}

// Push to position tests

TEST(ToPosition, BadInputs) {
    const char* const bad[] = {
        "{$push: {a: { $each: [1], $position:-1}}}",
        "{$push: {a: { $each: [1], $position:'s'}}}",
        "{$push: {a: { $each: [1], $position:{}}}}",
        "{$push: {a: { $each: [1], $position:[0]}}}",
        "{$push: {a: { $each: [1], $position:1.1211212}}}",
        "{$push: {a: { $each: [1], $position:3.000000000001}}}",
        "{$push: {a: { $each: [1], $position:1.2}}}",
        "{$push: {a: { $each: [1], $position:-1.2}}}",
        "{$push: {a: { $each: [1], $position:2147483648}}}",
        "{$push: {a: { $each: [1], $position:-2147483649}}}",
        "{$push: {a: { $each: [1], $position:NaN}}}",
        NULL,
    };

    int i = 0;
    while (bad[i] != NULL) {
        ModifierPush pushMod(ModifierPush::PUSH_NORMAL);
        BSONObj modObj = fromjson(bad[i]);
        ASSERT_NOT_OK(pushMod.init(modObj.firstElement().embeddedObject().firstElement(),
                                   ModifierInterface::Options::normal()));
        i++;
    }
}

TEST(ToPosition, GoodInputs) {
    {
        Document doc(fromjson("{a: []}"));
        Mod pushMod(fromjson("{$push: {a: { $each: [1], $position: NumberLong(1)}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));
    }
    {
        Document doc(fromjson("{a: []}"));
        Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:NumberInt(100)}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));
    }
    {
        Document doc(fromjson("{a: []}"));
        Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:1.0}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));
    }
    {
        Document doc(fromjson("{a: []}"));
        Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:1000000}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));
    }
    {
        Document doc(fromjson("{a: []}"));
        Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:0}}}"));

        ModifierInterface::ExecInfo execInfo;
        ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));
    }
}

TEST(ToPosition, EmptyArrayFront) {
    Document doc(fromjson("{a: []}"));
    Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:0}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a':[1]}}"), logDoc);
}

TEST(ToPosition, EmptyArrayBackBigPosition) {
    Document doc(fromjson("{a: []}"));
    Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:1000}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a':[1]}}"), logDoc);
}

TEST(ToPosition, EmptyArrayBack) {
    Document doc(fromjson("{a: []}"));
    Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:1}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a':[1]}}"), logDoc);
}

TEST(ToPosition, Front) {
    Document doc(fromjson("{a: [0]}"));
    Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:0}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [1, 0]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a':[1, 0]}}"), logDoc);
}

TEST(ToPosition, Back) {
    Document doc(fromjson("{a: [0]}"));
    Mod pushMod(fromjson("{$push: {a: { $each: [1], $position:100}}}"));

    ModifierInterface::ExecInfo execInfo;
    ASSERT_OK(pushMod.prepare(doc.root(), "", &execInfo));

    ASSERT_EQUALS(execInfo.fieldRef[0]->dottedField(), "a");
    ASSERT_FALSE(execInfo.noOp);

    ASSERT_OK(pushMod.apply());
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: [0,1]}"), doc);

    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    ASSERT_OK(pushMod.log(&logBuilder));
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.1':1}}"), logDoc);
}

}  // unnamed namespace
