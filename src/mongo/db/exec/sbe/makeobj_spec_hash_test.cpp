/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/expression_test_base.h"
#include "mongo/db/exec/sbe/makeobj_spec.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo::sbe::value {
namespace {

using MakeObjSpecHash = EExpressionTestFixture;
using FieldAction = MakeObjSpec::FieldAction;
using Keep = MakeObjSpec::Keep;
using Drop = MakeObjSpec::Drop;
using SetArg = MakeObjSpec::SetArg;
using LambdaArg = MakeObjSpec::LambdaArg;
using MakeObj = MakeObjSpec::MakeObj;
using NonObjInputBehavior = MakeObjSpec::NonObjInputBehavior;

std::vector<FieldAction> makeFieldActionVector(int depth = 2) {
    std::vector<FieldAction> fi;
    fi.emplace_back(Keep{});
    fi.emplace_back(Drop{});
    fi.emplace_back(SetArg{0});
    fi.emplace_back(SetArg{1});
    fi.emplace_back(LambdaArg{0, false});
    fi.emplace_back(LambdaArg{0, true});
    fi.emplace_back(
        MakeObj{std::make_unique<MakeObjSpec>(FieldListScope::kOpen, std::vector<std::string>{})});
    fi.emplace_back(MakeObj{
        std::make_unique<MakeObjSpec>(FieldListScope::kClosed, std::vector<std::string>{})});
    fi.emplace_back(MakeObj{std::make_unique<MakeObjSpec>(FieldListScope::kOpen,
                                                          std::vector<std::string>{},
                                                          std::vector<FieldAction>{},
                                                          NonObjInputBehavior::kReturnInput)});
    fi.emplace_back(MakeObj{std::make_unique<MakeObjSpec>(FieldListScope::kOpen,
                                                          std::vector<std::string>{},
                                                          std::vector<FieldAction>{},
                                                          NonObjInputBehavior::kReturnInput,
                                                          3)});
    // Tests recursive case!
    if (depth > 0) {
        auto fiv = makeFieldActionVector(depth - 1);
        std::vector<std::string> fieldNames;
        fieldNames.reserve(fiv.size());
        for (size_t i = 0; i < fiv.size(); i++) {
            fieldNames.push_back((std::stringstream() << "a" << i).str());
        }
        fi.emplace_back(MakeObj{std::make_unique<MakeObjSpec>(
            FieldListScope::kOpen, std::move(fieldNames), std::move(fiv))});
    }
    return fi;
}

std::vector<MakeObjSpec> makeMakeObjSpecVector() {
    std::vector<MakeObjSpec> mos;
    mos.emplace_back(FieldListScope::kOpen, std::vector<std::string>{});
    mos.emplace_back(FieldListScope::kClosed, std::vector<std::string>{});
    mos.emplace_back(FieldListScope::kClosed,
                     std::vector<std::string>{},
                     std::vector<FieldAction>{},
                     NonObjInputBehavior::kReturnNothing);
    mos.emplace_back(FieldListScope::kClosed,
                     std::vector<std::string>{},
                     std::vector<FieldAction>{},
                     NonObjInputBehavior::kReturnNothing,
                     3);

    for (size_t depth = 0; depth < 3; depth++) {
        auto fiv = makeFieldActionVector(depth);
        std::vector<std::string> fieldNames;
        fieldNames.reserve(fiv.size());
        for (size_t i = 0; i < fiv.size(); i++) {
            fieldNames.push_back((std::stringstream() << "a" << i).str());
        }
        mos.emplace_back(FieldListScope::kClosed, std::move(fieldNames), std::move(fiv));
    }

    return mos;
}

TEST_F(MakeObjSpecHash, TestFieldActionHash) {
    auto left = makeFieldActionVector();
    auto right = makeFieldActionVector();
    for (size_t i = 0; i < left.size(); i++) {
        ASSERT_EQ(left[i], right[i]);
        auto leftHash = absl::Hash<FieldAction>{}(left[i]);
        auto rightHash = absl::Hash<FieldAction>{}(right[i]);
        ASSERT_EQ(leftHash, rightHash);
        for (size_t j = i + 1; j < right.size(); j++) {
            ASSERT_NE(leftHash, absl::Hash<FieldAction>{}(right[j]));
        }
    }
}

TEST_F(MakeObjSpecHash, TestMakeObjSpecHash) {
    auto left = makeMakeObjSpecVector();
    auto right = makeMakeObjSpecVector();
    for (size_t i = 0; i < left.size(); i++) {
        ASSERT_EQ(left[i], right[i]);
        auto leftHash = absl::Hash<MakeObjSpec>{}(left[i]);
        auto rightHash = absl::Hash<MakeObjSpec>{}(right[i]);
        ASSERT_EQ(leftHash, rightHash);
        for (size_t j = i + 1; j < right.size(); j++) {
            ASSERT_NE(leftHash, absl::Hash<MakeObjSpec>{}(right[j]));
        }
    }
}

}  // namespace
}  // namespace mongo::sbe::value
