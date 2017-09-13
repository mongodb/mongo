/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <algorithm>
#include <cctype>

#include "mongo/db/field_ref.h"
#include "mongo/db/field_ref_set.h"
#include "mongo/db/repl/idempotency_document_structure.h"
#include "mongo/db/repl/idempotency_update_sequence.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

std::vector<std::string> eliminatePrefixPaths_forTest(const std::string& path,
                                                      const std::vector<std::string>& paths) {
    return UpdateSequenceGenerator::_eliminatePrefixPaths(path, paths);
}

size_t getPathDepth_forTest(const std::string& path) {
    return UpdateSequenceGenerator::_getPathDepth(path);
}

namespace {

TEST(UpdateGenTest, FindsAllPaths) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 1;
    size_t length = 1;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generator({fields, depth, length}, random, &trivialScalarGenerator);

    ASSERT_EQ(generator.getPaths().size(), 5U);

    std::vector<std::string> expectedPaths{"a", "a.0", "a.b", "b", "b.0"};
    std::vector<std::string> foundPaths(generator.getPaths());
    std::sort(expectedPaths.begin(), expectedPaths.end());
    std::sort(foundPaths.begin(), foundPaths.end());
    if (foundPaths != expectedPaths) {
        StringBuilder sb;
        sb << "Did not find all paths. Instead, we found: [ ";
        bool firstIter = true;
        for (auto path : foundPaths) {
            if (!firstIter) {
                sb << ", ";
            } else {
                firstIter = false;
            }
            sb << path;
        }
        sb << " ]; ";
        FAIL(sb.str());
    }
}

TEST(UpdateGenTest, NoDuplicatePaths) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 2;
    size_t length = 2;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generator({fields, depth, length}, random, &trivialScalarGenerator);

    auto paths = generator.getPaths();
    for (size_t i = 0; i < paths.size(); i++) {
        for (size_t j = i + 1; j < paths.size(); j++) {
            if (paths[i] == paths[j]) {
                StringBuilder sb;
                sb << "Outer path matched with inner path.";
                sb << generator.getPaths()[i] << " was duplicated.";
                FAIL(sb.str());
            }
        }
    }
}

TEST(UpdateGenTest, UpdatesHaveValidPaths) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 1;
    size_t length = 1;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generator({fields, depth, length}, random, &trivialScalarGenerator);
    auto update = generator.generateUpdate();

    BSONObj updateArg;
    if (auto setElem = update["$set"]) {
        updateArg = setElem.Obj();
    } else if (auto unsetElem = update["$unset"]) {
        updateArg = unsetElem.Obj();
    } else {
        StringBuilder sb;
        sb << "The generated update is not a $set or $unset BSONObj: " << update;
        FAIL(sb.str());
    }

    std::set<std::string> argPaths;
    updateArg.getFieldNames(argPaths);
    std::set<std::string> correctPaths{"a", "b", "a.0", "a.b", "b.0"};
    for (auto path : argPaths) {
        FieldRef pathRef(path);
        StringBuilder sb;
        if (path[0] == '0' || path[0] == '1') {
            sb << "Some path (" << path << "), found in the (un)set arguments from the update "
               << update << " contains a leading array position. ";
            FAIL(sb.str());
        }
        if (correctPaths.find(path) == correctPaths.end()) {
            sb << "Some path (" << path << "), found in the (un)set arguments from the update "
               << update << " contains an invalid fieldname(s). ";
            FAIL(sb.str());
        }
    }
}

TEST(UpdateGenTest, UpdatesAreNotAmbiguous) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 1;
    size_t length = 1;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generator({fields, depth, length}, random, &trivialScalarGenerator);
    auto update = generator.generateUpdate();

    BSONObj updateArg;
    if (auto setElem = update["$set"]) {
        updateArg = setElem.Obj();
    } else if (auto unsetElem = update["$unset"]) {
        updateArg = unsetElem.Obj();
    } else {
        StringBuilder sb;
        sb << "The generated update is not a $set or $unset BSONObj: " << update;
        FAIL(sb.str());
    }
    std::set<std::string> argPathsSet;
    updateArg.getFieldNames(argPathsSet);

    std::vector<std::unique_ptr<FieldRef>> argPathsRefVec;
    FieldRefSet pathRefSet;
    for (auto path : argPathsSet) {
        argPathsRefVec.push_back(stdx::make_unique<FieldRef>(path));
        const FieldRef* conflict;
        if (!pathRefSet.insert(argPathsRefVec.back().get(), &conflict)) {
            StringBuilder sb;
            sb << "Some path in the (un)set arguments of " << update
               << " causes ambiguity due to a conflict between "
               << argPathsRefVec.back()->dottedField() << " and " << conflict->dottedField();
            FAIL(sb.str());
        }
    }
}

std::size_t getMaxDepth(BSONObj obj) {
    size_t curMaxDepth = 0;
    for (auto elem : obj) {
        if (elem.type() == BSONType::Object || elem.type() == BSONType::Array) {
            curMaxDepth = std::max(curMaxDepth, 1 + getMaxDepth(elem.Obj()));
        }
    }

    return curMaxDepth;
}

TEST(UpdateGenTest, UpdatesPreserveDepthConstraint) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 2;
    size_t length = 1;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generator(
        {fields, depth, length, 0.333, 0.333, 0.334}, random, &trivialScalarGenerator);

    BSONElement setElem;
    BSONObj update;
    // Because our probabilities sum to 1, we are guaranteed to always get a $set.
    update = generator.generateUpdate();
    setElem = update["$set"];
    BSONObj updateArg = setElem.Obj();

    std::set<std::string> argPaths;
    updateArg.getFieldNames(argPaths);
    for (auto path : argPaths) {
        auto pathDepth = getPathDepth_forTest(path);
        auto particularSetArgument = updateArg[path];
        auto embeddedArgDepth = 0;
        if (particularSetArgument.type() == BSONType::Object ||
            particularSetArgument.type() == BSONType::Array) {
            embeddedArgDepth = getMaxDepth(particularSetArgument.Obj()) + 1;
        }

        auto argDepth = pathDepth + embeddedArgDepth;
        if (argDepth > depth) {
            StringBuilder sb;
            sb << "The path " << path << " and its argument " << particularSetArgument
               << " exceeds the maximum depth.";
            FAIL(sb.str());
        }
    }
}

TEST(UpdateGenTest, OnlyGenerateUnset) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 1;
    size_t length = 1;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generatorNoSet(
        {fields, depth, length, 0.0, 0.0, 0.0}, random, &trivialScalarGenerator);

    for (size_t i = 0; i < 100; i++) {
        auto update = generatorNoSet.generateUpdate();
        if (!update["$unset"]) {
            StringBuilder sb;
            sb << "Generator created an update that was not an $unset, even though the probability "
                  "of doing so is zero: "
               << update;
            FAIL(sb.str());
        }
    }
}

TEST(UpdateGenTest, OnlySetUpdatesWithScalarValue) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 1;
    size_t length = 1;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generatorNoUnsetAndOnlyScalar(
        {fields, depth, length, 1.0, 0.0, 0.0}, random, &trivialScalarGenerator);

    for (size_t i = 0; i < 100; i++) {
        auto update = generatorNoUnsetAndOnlyScalar.generateUpdate();
        if (!update["$set"]) {
            StringBuilder sb;
            sb << "Generator created an update that was not an $set, even though the probability "
                  "of doing so is zero: "
               << update;
            FAIL(sb.str());
        } else if (getMaxDepth(update["$set"].Obj()) != 0) {
            StringBuilder sb;
            sb << "Generator created an update that had a nonscalar value, because it's maximum "
                  "depth was nonzero: "
               << update;
            FAIL(sb.str());
        }
    }
}

TEST(UpdateGenTest, OnlySetUpdatesWithScalarsAtMaxDepth) {
    std::set<StringData> fields{"a", "b"};
    size_t depth = 2;
    size_t length = 1;

    PseudoRandom random(SecureRandom::create()->nextInt64());
    TrivialScalarGenerator trivialScalarGenerator;
    UpdateSequenceGenerator generatorNeverScalar(
        {fields, depth, length, 0.0, 0.5, 0.5}, random, &trivialScalarGenerator);

    for (size_t i = 0; i < 100; i++) {
        auto update = generatorNeverScalar.generateUpdate();
        for (auto elem : update["$set"].Obj()) {
            StringData fieldName = elem.fieldNameStringData();
            FieldRef fieldRef(fieldName);
            size_t pathDepth = getPathDepth_forTest(fieldName.toString());
            bool isDocOrArr = elem.type() == BSONType::Object || elem.type() == BSONType::Array;
            if (pathDepth != depth) {
                // If the path is not equal to the max depth we provided above, then there
                // should
                // only be an array or doc at this point.
                if (!isDocOrArr) {
                    StringBuilder sb;
                    sb << "The set argument: " << elem
                       << " is a scalar, but the probability of a scalar occuring for a path that "
                          "does not meet the maximum depth is zero.";
                    FAIL(sb.str());
                }
            } else {
                if (isDocOrArr) {
                    StringBuilder sb;
                    sb << "The set argument: " << elem
                       << " is not scalar, however, this path reaches the maximum depth so a "
                          "scalar should be the only choice.";
                    FAIL(sb.str());
                }
            }
        }
    }
}

}  // namespace
}  // namespace mongo
