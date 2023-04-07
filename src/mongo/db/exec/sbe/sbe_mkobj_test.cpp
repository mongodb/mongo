/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/sbe_plan_stage_test.h"
#include "mongo/db/exec/sbe/stages/makeobj.h"

namespace mongo::sbe {
class MkObjStageTest : public PlanStageTestFixture {
public:
    void addBsonObjToArray(value::Array* arr, const BSONObj& obj) {
        auto [tag, val] = value::copyValue(value::TypeTags::bsonObject,
                                           value::bitcastFrom<const char*>(obj.objdata()));

        arr->push_back(tag, val);
    }

    void addObjectToArray(value::Array* arr, const BSONObj& obj) {
        auto [objTag, objVal] = value::makeNewObject();
        auto objView = value::getObjectView(objVal);

        const char* be = obj.objdata();
        be += 4;
        const char* end = be + obj.objsize();
        while (*be != 0) {
            auto sv = bson::fieldNameAndLength(be);
            auto [tag, val] = bson::convertFrom<false>(be, end, sv.size());
            be = bson::advance(be, sv.size());

            objView->push_back(sv, tag, val);
        }
        arr->push_back(objTag, objVal);
    }

    template <class MkObjStageType>
    void testKeep() {
        auto objOutSlotId = generateSlotId();
        auto makeStageFn = [objOutSlotId](value::SlotId scanSlot,
                                          std::unique_ptr<PlanStage> scanStage) {
            auto mkobj = makeS<MkObjStageType>(std::move(scanStage),
                                               objOutSlotId,
                                               scanSlot,
                                               MkObjStageType::FieldBehavior::keep,
                                               std::vector<std::string>{"a", "b"},
                                               std::vector<std::string>{},
                                               value::SlotVector{},
                                               false,  // force new
                                               false,  // return old
                                               kEmptyPlanNodeId);

            return std::make_pair(objOutSlotId, std::move(mkobj));
        };

        auto [inputTag, inputVal] = value::makeNewArray();
        value::ValueGuard inputGuard{inputTag, inputVal};
        {
            auto inputView = value::getArrayView(inputVal);
            addBsonObjToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
            addBsonObjToArray(inputView, BSON("b" << 1 << "c" << 2));
            addBsonObjToArray(inputView, BSON("b" << 1 << "a" << 2));

            addObjectToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
            addObjectToArray(inputView, BSON("b" << 1 << "c" << 2));
            addObjectToArray(inputView, BSON("b" << 1 << "a" << 2));
        }

        auto [expectedTag, expectedVal] = stage_builder::makeValue(
            BSON_ARRAY(BSON("a" << 1 << "b" << 2) << BSON("b" << 1) << BSON("b" << 1 << "a" << 2) <<

                       BSON("a" << 1 << "b" << 2) << BSON("b" << 1) << BSON("b" << 1 << "a" << 2)));
        value::ValueGuard expectedGuard{expectedTag, expectedVal};

        inputGuard.reset();
        expectedGuard.reset();
        runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
    }

    template <class MkObjStageType>
    void testDrop() {
        auto objOutSlotId = generateSlotId();
        auto makeStageFn = [objOutSlotId](value::SlotId scanSlot,
                                          std::unique_ptr<PlanStage> scanStage) {
            auto mkobj = makeS<MkObjStageType>(std::move(scanStage),
                                               objOutSlotId,
                                               scanSlot,
                                               MkObjStageType::FieldBehavior::drop,
                                               std::vector<std::string>{"a", "b"},
                                               std::vector<std::string>{},
                                               value::SlotVector{},
                                               false,  // force new
                                               false,  // return old
                                               kEmptyPlanNodeId);

            return std::make_pair(objOutSlotId, std::move(mkobj));
        };

        auto [inputTag, inputVal] = value::makeNewArray();
        value::ValueGuard inputGuard{inputTag, inputVal};
        {
            auto inputView = value::getArrayView(inputVal);
            // Add BSON to the input.
            addBsonObjToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
            addBsonObjToArray(inputView, BSON("c" << 1));
            addBsonObjToArray(inputView, BSON("a" << 1));

            // Add some SBE objects to the input.
            addObjectToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
            addObjectToArray(inputView, BSON("c" << 1));
            addObjectToArray(inputView, BSON("a" << 1));
        }

        auto [expectedTag, expectedVal] = stage_builder::makeValue(
            BSON_ARRAY(BSON("c" << 3) << BSON("c" << 1) << BSONObj() << BSON("c" << 3)
                                      << BSON("c" << 1) << BSONObj()));
        value::ValueGuard expectedGuard{expectedTag, expectedVal};

        inputGuard.reset();
        expectedGuard.reset();
        runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
    }

    template <class MkObjStageType>
    void testProjectWithoutRoot() {
        auto objOutSlotId = generateSlotId();
        auto slotVec = makeSV(generateSlotId(), generateSlotId());

        value::SlotMap<std::unique_ptr<EExpression>> slotMap;
        slotMap[slotVec[0]] = makeE<EConstant>("one");
        slotMap[slotVec[1]] = makeE<EConstant>("two");

        auto makeStageFn = [objOutSlotId, &slotVec, &slotMap](
                               value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
            auto project =
                makeS<ProjectStage>(std::move(scanStage), std::move(slotMap), kEmptyPlanNodeId);

            auto mkobj = makeS<MkObjStageType>(std::move(project),
                                               objOutSlotId,
                                               boost::none,
                                               boost::none,
                                               std::vector<std::string>{},
                                               std::vector<std::string>{"a", "b"},
                                               slotVec,
                                               false,  // force new
                                               false,  // return old
                                               kEmptyPlanNodeId);

            return std::make_pair(objOutSlotId, std::move(mkobj));
        };

        auto [inputTag, inputVal] = value::makeNewArray();
        value::ValueGuard inputGuard{inputTag, inputVal};
        {
            auto inputView = value::getArrayView(inputVal);
            addBsonObjToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
            addObjectToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
        }

        auto [expectedTag, expectedVal] = stage_builder::makeValue(BSON_ARRAY(BSON("a"
                                                                                   << "one"
                                                                                   << "b"
                                                                                   << "two")
                                                                              << BSON("a"
                                                                                      << "one"
                                                                                      << "b"
                                                                                      << "two")));
        value::ValueGuard expectedGuard{expectedTag, expectedVal};

        inputGuard.reset();
        expectedGuard.reset();
        runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
    }

    template <class MkObjStageType>
    void testProjectWithRoot() {
        auto objOutSlotId = generateSlotId();
        auto slotVec = makeSV(generateSlotId(), generateSlotId());

        value::SlotMap<std::unique_ptr<EExpression>> slotMap;
        slotMap[slotVec[0]] = makeE<EConstant>("one");
        slotMap[slotVec[1]] = makeE<EConstant>("two");

        auto makeStageFn = [objOutSlotId, &slotVec, &slotMap](
                               value::SlotId scanSlot, std::unique_ptr<PlanStage> scanStage) {
            auto project =
                makeS<ProjectStage>(std::move(scanStage), std::move(slotMap), kEmptyPlanNodeId);

            auto mkobj = makeS<MkObjStageType>(std::move(project),
                                               objOutSlotId,
                                               scanSlot,
                                               MkObjStageType::FieldBehavior::drop,
                                               std::vector<std::string>{},
                                               std::vector<std::string>{"a", "b"},
                                               slotVec,
                                               false,  // force new
                                               false,  // return old
                                               kEmptyPlanNodeId);

            return std::make_pair(objOutSlotId, std::move(mkobj));
        };

        auto [inputTag, inputVal] = value::makeNewArray();
        value::ValueGuard inputGuard{inputTag, inputVal};
        {
            auto inputView = value::getArrayView(inputVal);
            // Add BSON to the input.
            addBsonObjToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
            addBsonObjToArray(inputView, BSON("a" << 1 << "c" << 2));

            // Add some SBE objects to the input.
            addObjectToArray(inputView, BSON("a" << 1 << "b" << 2 << "c" << 3));
            addObjectToArray(inputView, BSON("a" << 1 << "c" << 2));
        }

        auto [expectedTag, expectedVal] =
            stage_builder::makeValue(BSON_ARRAY(BSON("a"
                                                     << "one"
                                                     << "b"
                                                     << "two"
                                                     << "c" << 3)
                                                << BSON("a"
                                                        << "one"
                                                        << "c" << 2 << "b"
                                                        << "two")
                                                << BSON("a"
                                                        << "one"
                                                        << "b"
                                                        << "two"
                                                        << "c" << 3)
                                                << BSON("a"
                                                        << "one"
                                                        << "c" << 2 << "b"
                                                        << "two")));
        value::ValueGuard expectedGuard{expectedTag, expectedVal};

        inputGuard.reset();
        expectedGuard.reset();
        runTest(inputTag, inputVal, expectedTag, expectedVal, makeStageFn);
    }
};

TEST_F(MkObjStageTest, MakeObjKeep) {
    testKeep<MakeObjStage>();
}

TEST_F(MkObjStageTest, MakeBsonObjKeep) {
    testKeep<MakeBsonObjStage>();
}

TEST_F(MkObjStageTest, MakeObjDrop) {
    testDrop<MakeObjStage>();
}

TEST_F(MkObjStageTest, MakeBsonObjDrop) {
    testDrop<MakeBsonObjStage>();
}

TEST_F(MkObjStageTest, MakeObjProject) {
    testProjectWithoutRoot<MakeObjStage>();
}

TEST_F(MkObjStageTest, MakeBsonObjProject) {
    testProjectWithoutRoot<MakeBsonObjStage>();
}

TEST_F(MkObjStageTest, MakeObjProjectWithRoot) {
    testProjectWithRoot<MakeObjStage>();
}

TEST_F(MkObjStageTest, MakeBsonObjProjectWithRoot) {
    testProjectWithRoot<MakeBsonObjStage>();
}
}  // namespace mongo::sbe
