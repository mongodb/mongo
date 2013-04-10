/*    Copyright 2012 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
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

#define ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS)                 \
    (GRAPH).addInitializer(                                             \
            (NAME),                                                     \
            (FN),                                                       \
            MONGO_MAKE_STRING_VECTOR PREREQS,                           \
            MONGO_MAKE_STRING_VECTOR DEPS)

#define ASSERT_ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS)  \
    ASSERT_EQUALS(Status::OK(), ADD_INITIALIZER(GRAPH, NAME, FN, PREREQS, DEPS))


#define CONSTRUCT_DEPENDENCY_GRAPH(GRAPH, FN0, FN1, FN2, FN3, FN4, FN5, FN6, FN7, FN8) \
    do {                                                                \
        InitializerDependencyGraph& _graph_ = (GRAPH);                  \
        ASSERT_ADD_INITIALIZER(_graph_, "n0", FN0, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS); \
        ASSERT_ADD_INITIALIZER(_graph_, "n1", FN1, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS); \
        ASSERT_ADD_INITIALIZER(_graph_, "n2", FN2, ("n0", "n1"), MONGO_NO_DEPENDENTS);   \
        ASSERT_ADD_INITIALIZER(_graph_, "n3", FN3, ("n0", "n2"), MONGO_NO_DEPENDENTS);   \
        ASSERT_ADD_INITIALIZER(_graph_, "n4", FN4, ("n2", "n1"), MONGO_NO_DEPENDENTS);   \
        ASSERT_ADD_INITIALIZER(_graph_, "n5", FN5, ("n3", "n4"), MONGO_NO_DEPENDENTS);   \
        ASSERT_ADD_INITIALIZER(_graph_, "n6", FN6, ("n4"), MONGO_NO_DEPENDENTS);         \
        ASSERT_ADD_INITIALIZER(_graph_, "n7", FN7, ("n3"), MONGO_NO_DEPENDENTS);         \
        ASSERT_ADD_INITIALIZER(_graph_, "n8", FN8, ("n5", "n6", "n7"), MONGO_NO_DEPENDENTS); \
    } while (false)

namespace mongo {
namespace {

    int globalCounts[9];

    Status doNothing(InitializerContext*) { return Status::OK(); }

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
                                   set0, set1, set2, set3, set4, set5, set6, set7, set8);
        clearCounts();
        ASSERT_OK(initializer.execute(InitializerContext::ArgumentVector(),
                                      InitializerContext::EnvironmentMap()));
        for (int i = 0; i < 9; ++i)
            ASSERT_EQUALS(1, globalCounts[i]);
    }

    TEST(InitializerTest, Step5Misimplemented) {
        Initializer initializer;
        CONSTRUCT_DEPENDENCY_GRAPH(initializer.getInitializerDependencyGraph(),
                                   set0, set1, set2, set3, set4, doNothing, set6, set7, set8);
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
