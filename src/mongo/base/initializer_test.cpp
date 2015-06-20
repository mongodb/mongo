/*    Copyright 2012 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * Unit tests of the Initializer type.
 */

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/initializer_dependency_graph.h"
#include "mongo/base/make_string_vector.h"
#include "mongo/unittest/unittest.h"

/*
 * Unless otherwise specified, all tests herein use the following
 * dependency graph.
 *
 * 0 <-  3 <- 7
 *  ^   / ^    ^
 *   \ v   \     \
 *    2     5 <-  8
 *   / ^   /     /
 *  v   \ v    v
 * 1 <-  4 <- 6
 *
 */

#define ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS) \
    (GRAPH).addInitializer(                             \
        (NAME), (FN), MONGO_MAKE_STRING_VECTOR PREREQS, MONGO_MAKE_STRING_VECTOR DEPS)

#define ASSERT_ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS) \
    ASSERT_EQUALS(Status::OK(), ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS))


#define CONSTRUCT_DEPENDENCY_GRAPH(GRAPH, FN0, FN1, FN2, FN3, FN4, FN5, FN6, FN7, FN8)           \
    do {                                                                                         \
        InitializerDependencyGraph& _graph_ = (GRAPH);                                           \
        ASSERT_ADD_INITIALIZER(_graph_, "n0", FN0, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS); \
        ASSERT_ADD_INITIALIZER(_graph_, "n1", FN1, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS); \
        ASSERT_ADD_INITIALIZER(_graph_, "n2", FN2, ("n0", "n1"), MONGO_NO_DEPENDENTS);           \
        ASSERT_ADD_INITIALIZER(_graph_, "n3", FN3, ("n0", "n2"), MONGO_NO_DEPENDENTS);           \
        ASSERT_ADD_INITIALIZER(_graph_, "n4", FN4, ("n2", "n1"), MONGO_NO_DEPENDENTS);           \
        ASSERT_ADD_INITIALIZER(_graph_, "n5", FN5, ("n3", "n4"), MONGO_NO_DEPENDENTS);           \
        ASSERT_ADD_INITIALIZER(_graph_, "n6", FN6, ("n4"), MONGO_NO_DEPENDENTS);                 \
        ASSERT_ADD_INITIALIZER(_graph_, "n7", FN7, ("n3"), MONGO_NO_DEPENDENTS);                 \
        ASSERT_ADD_INITIALIZER(_graph_, "n8", FN8, ("n5", "n6", "n7"), MONGO_NO_DEPENDENTS);     \
    } while (false)

namespace mongo {
namespace {

int globalCounts[9];

Status doNothing(InitializerContext*) {
    return Status::OK();
}

Status set0(InitializerContext*) {
    globalCounts[0] = 1;
    return Status::OK();
}

Status set1(InitializerContext*) {
    globalCounts[1] = 1;
    return Status::OK();
}

Status set2(InitializerContext*) {
    if (!globalCounts[0] || !globalCounts[1])
        return Status(ErrorCodes::UnknownError, "one of 0 or 1 not already set");
    globalCounts[2] = 1;
    return Status::OK();
}

Status set3(InitializerContext*) {
    if (!globalCounts[0] || !globalCounts[2])
        return Status(ErrorCodes::UnknownError, "one of 0 or 2 not already set");
    globalCounts[3] = 1;
    return Status::OK();
}

Status set4(InitializerContext*) {
    if (!globalCounts[1] || !globalCounts[2])
        return Status(ErrorCodes::UnknownError, "one of 1 or 2 not already set");
    globalCounts[4] = 1;
    return Status::OK();
}

Status set5(InitializerContext*) {
    if (!globalCounts[3] || !globalCounts[4])
        return Status(ErrorCodes::UnknownError, "one of 3 or 4 not already set");
    globalCounts[5] = 1;
    return Status::OK();
}

Status set6(InitializerContext*) {
    if (!globalCounts[4])
        return Status(ErrorCodes::UnknownError, "4 not already set");
    globalCounts[6] = 1;
    return Status::OK();
}

Status set7(InitializerContext*) {
    if (!globalCounts[3])
        return Status(ErrorCodes::UnknownError, "3 not already set");
    globalCounts[7] = 1;
    return Status::OK();
}

Status set8(InitializerContext*) {
    if (!globalCounts[5] || !globalCounts[6] || !globalCounts[7])
        return Status(ErrorCodes::UnknownError, "one of 5, 6, 7 not already set");
    globalCounts[8] = 1;
    return Status::OK();
}

void clearCounts() {
    for (size_t i = 0; i < 9; ++i)
        globalCounts[i] = 0;
}

TEST(InitializerTest, SuccessfulInitialization) {
    Initializer initializer;
    CONSTRUCT_DEPENDENCY_GRAPH(initializer.getInitializerDependencyGraph(),
                               set0,
                               set1,
                               set2,
                               set3,
                               set4,
                               set5,
                               set6,
                               set7,
                               set8);
    clearCounts();
    ASSERT_OK(initializer.execute(InitializerContext::ArgumentVector(),
                                  InitializerContext::EnvironmentMap()));
    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(1, globalCounts[i]);
}

TEST(InitializerTest, Step5Misimplemented) {
    Initializer initializer;
    CONSTRUCT_DEPENDENCY_GRAPH(initializer.getInitializerDependencyGraph(),
                               set0,
                               set1,
                               set2,
                               set3,
                               set4,
                               doNothing,
                               set6,
                               set7,
                               set8);
    clearCounts();
    ASSERT_EQUALS(ErrorCodes::UnknownError,
                  initializer.execute(InitializerContext::ArgumentVector(),
                                      InitializerContext::EnvironmentMap()));
    ASSERT_EQUALS(1, globalCounts[0]);
    ASSERT_EQUALS(1, globalCounts[1]);
    ASSERT_EQUALS(1, globalCounts[2]);
    ASSERT_EQUALS(1, globalCounts[3]);
    ASSERT_EQUALS(1, globalCounts[4]);
    ASSERT_EQUALS(0, globalCounts[8]);
}

}  // namespace
}  // namespace mongo
