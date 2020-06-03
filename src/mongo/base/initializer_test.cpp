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

/**
 * Unit tests of the Initializer type.
 */

#include "mongo/base/init.h"
#include "mongo/base/initializer.h"
#include "mongo/base/initializer_dependency_graph.h"
#include "mongo/unittest/death_test.h"
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

#define STRIP_PARENS_(...) __VA_ARGS__

#define ADD_INITIALIZER(GRAPH, NAME, INIT_FN, DEINIT_FN, PREREQS, DEPS)     \
    (GRAPH).addInitializer((NAME),                                          \
                           (INIT_FN),                                       \
                           (DEINIT_FN),                                     \
                           std::vector<std::string>{STRIP_PARENS_ PREREQS}, \
                           std::vector<std::string>{STRIP_PARENS_ DEPS})

#define ASSERT_ADD_INITIALIZER(GRAPH, NAME, INIT_FN, DEINIT_FN, PREREQS, DEPS) \
    ASSERT_EQUALS(Status::OK(), ADD_INITIALIZER(GRAPH, NAME, INIT_FN, DEINIT_FN, PREREQS, DEPS))


#define CONSTRUCT_DEPENDENCY_GRAPH(GRAPH,                                                         \
                                   INIT_FN0,                                                      \
                                   DEINIT_FN0,                                                    \
                                   INIT_FN1,                                                      \
                                   DEINIT_FN1,                                                    \
                                   INIT_FN2,                                                      \
                                   DEINIT_FN2,                                                    \
                                   INIT_FN3,                                                      \
                                   DEINIT_FN3,                                                    \
                                   INIT_FN4,                                                      \
                                   DEINIT_FN4,                                                    \
                                   INIT_FN5,                                                      \
                                   DEINIT_FN5,                                                    \
                                   INIT_FN6,                                                      \
                                   DEINIT_FN6,                                                    \
                                   INIT_FN7,                                                      \
                                   DEINIT_FN7,                                                    \
                                   INIT_FN8,                                                      \
                                   DEINIT_FN8)                                                    \
    do {                                                                                          \
        InitializerDependencyGraph& _graph_ = (GRAPH);                                            \
        ASSERT_ADD_INITIALIZER(                                                                   \
            _graph_, "n0", INIT_FN0, DEINIT_FN0, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);    \
        ASSERT_ADD_INITIALIZER(                                                                   \
            _graph_, "n1", INIT_FN1, DEINIT_FN1, MONGO_NO_PREREQUISITES, MONGO_NO_DEPENDENTS);    \
        ASSERT_ADD_INITIALIZER(                                                                   \
            _graph_, "n2", INIT_FN2, DEINIT_FN2, ("n0", "n1"), MONGO_NO_DEPENDENTS);              \
        ASSERT_ADD_INITIALIZER(                                                                   \
            _graph_, "n3", INIT_FN3, DEINIT_FN3, ("n0", "n2"), MONGO_NO_DEPENDENTS);              \
        ASSERT_ADD_INITIALIZER(                                                                   \
            _graph_, "n4", INIT_FN4, DEINIT_FN4, ("n2", "n1"), MONGO_NO_DEPENDENTS);              \
        ASSERT_ADD_INITIALIZER(                                                                   \
            _graph_, "n5", INIT_FN5, DEINIT_FN5, ("n3", "n4"), MONGO_NO_DEPENDENTS);              \
        ASSERT_ADD_INITIALIZER(_graph_, "n6", INIT_FN6, DEINIT_FN6, ("n4"), MONGO_NO_DEPENDENTS); \
        ASSERT_ADD_INITIALIZER(_graph_, "n7", INIT_FN7, DEINIT_FN7, ("n3"), MONGO_NO_DEPENDENTS); \
        ASSERT_ADD_INITIALIZER(                                                                   \
            _graph_, "n8", INIT_FN8, DEINIT_FN8, ("n5", "n6", "n7"), MONGO_NO_DEPENDENTS);        \
    } while (false)

namespace mongo {
namespace {

enum State {
    UNSET = 0,
    INITIALIZED = 1,
    DEINITIALIZED = 2,
};

State globalStates[9];

Status initNoop(InitializerContext*) {
    return Status::OK();
}

Status deinitNoop(DeinitializerContext*) {
    return Status::OK();
}

Status init0(InitializerContext*) {
    globalStates[0] = INITIALIZED;
    return Status::OK();
}

Status init1(InitializerContext*) {
    globalStates[1] = INITIALIZED;
    return Status::OK();
}

Status init2(InitializerContext*) {
    if (globalStates[0] != INITIALIZED || globalStates[1] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(init2) one of 0 or 1 not already initialized");
    globalStates[2] = INITIALIZED;
    return Status::OK();
}

Status init3(InitializerContext*) {
    if (globalStates[0] != INITIALIZED || globalStates[2] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(init3) one of 0 or 2 not already initialized");
    globalStates[3] = INITIALIZED;
    return Status::OK();
}

Status init4(InitializerContext*) {
    if (globalStates[1] != INITIALIZED || globalStates[2] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(init4) one of 1 or 2 not already initialized");
    globalStates[4] = INITIALIZED;
    return Status::OK();
}

Status init5(InitializerContext*) {
    if (globalStates[3] != INITIALIZED || globalStates[4] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(init5) one of 3 or 4 not already initialized");
    globalStates[5] = INITIALIZED;
    return Status::OK();
}

Status init6(InitializerContext*) {
    if (globalStates[4] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(init6) 4 not already initialized");
    globalStates[6] = INITIALIZED;
    return Status::OK();
}

Status init7(InitializerContext*) {
    if (globalStates[3] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(init7) 3 not already initialized");
    globalStates[7] = INITIALIZED;
    return Status::OK();
}

Status init8(InitializerContext*) {
    if (globalStates[5] != INITIALIZED || globalStates[6] != INITIALIZED ||
        globalStates[7] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(init8) one of 5, 6, 7 not already initialized");
    globalStates[8] = INITIALIZED;
    return Status::OK();
}

Status deinit8(DeinitializerContext*) {
    if (globalStates[8] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit8) 8 not initialized");
    globalStates[8] = DEINITIALIZED;
    return Status::OK();
}

Status deinit7(DeinitializerContext*) {
    if (globalStates[7] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit7) 7 not initialized");
    if (globalStates[8] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit7) 8 not already deinitialized");
    globalStates[7] = DEINITIALIZED;
    return Status::OK();
}

Status deinit6(DeinitializerContext*) {
    if (globalStates[6] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit6) 6 not initialized");
    if (globalStates[8] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit6) 8 not already deinitialized");
    globalStates[6] = DEINITIALIZED;
    return Status::OK();
}

Status deinit5(DeinitializerContext*) {
    if (globalStates[5] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit5) 5 not initialized");
    if (globalStates[8] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit5) 8 not already deinitialized");
    globalStates[5] = DEINITIALIZED;
    return Status::OK();
}

Status deinit4(DeinitializerContext*) {
    if (globalStates[4] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit4) 4 not initialized");
    if (globalStates[5] != DEINITIALIZED || globalStates[6] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError,
                      "(deinit4) one of 5 or 6 not already deinitialized");
    globalStates[4] = DEINITIALIZED;
    return Status::OK();
}

Status deinit3(DeinitializerContext*) {
    if (globalStates[3] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit3) 3 not initialized");
    if (globalStates[5] != DEINITIALIZED || globalStates[7] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError,
                      "(deinit3) one of 5 or 7 not already deinitialized");
    globalStates[3] = DEINITIALIZED;
    return Status::OK();
}

Status deinit2(DeinitializerContext*) {
    if (globalStates[2] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit2) 2 not initialized");
    if (globalStates[3] != DEINITIALIZED || globalStates[4] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError,
                      "(deinit2) one of 3 or 4 not already deinitialized");
    globalStates[2] = DEINITIALIZED;
    return Status::OK();
}

Status deinit1(DeinitializerContext*) {
    if (globalStates[1] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit1) 1 not initialized");
    if (globalStates[2] != DEINITIALIZED || globalStates[4] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError,
                      "(deinit1) one of 2 or 4 not already deinitialized");
    globalStates[1] = DEINITIALIZED;
    return Status::OK();
}

Status deinit0(DeinitializerContext*) {
    if (globalStates[0] != INITIALIZED)
        return Status(ErrorCodes::UnknownError, "(deinit0) 0 not initialized");
    if (globalStates[2] != DEINITIALIZED || globalStates[3] != DEINITIALIZED)
        return Status(ErrorCodes::UnknownError,
                      "(deinit0) one of 2 or 3 not already deinitialized");
    globalStates[0] = DEINITIALIZED;
    return Status::OK();
}

void clearCounts() {
    for (size_t i = 0; i < 9; ++i)
        globalStates[i] = UNSET;
}

void constructNormalDependencyGraph(Initializer* initializer) {
    CONSTRUCT_DEPENDENCY_GRAPH(initializer->getInitializerDependencyGraph(),
                               init0,
                               deinit0,
                               init1,
                               deinit1,
                               init2,
                               deinit2,
                               init3,
                               deinit3,
                               init4,
                               deinit4,
                               init5,
                               deinit5,
                               init6,
                               deinit6,
                               init7,
                               deinit7,
                               init8,
                               deinit8);
}

TEST(InitializerTest, SuccessfulInitializationAndDeinitialization) {
    Initializer initializer;
    constructNormalDependencyGraph(&initializer);
    clearCounts();

    ASSERT_OK(initializer.executeInitializers({}));

    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(INITIALIZED, globalStates[i]);

    ASSERT_OK(initializer.executeDeinitializers());

    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(DEINITIALIZED, globalStates[i]);
}

TEST(InitializerTest, Init5Misimplemented) {
    Initializer initializer;
    CONSTRUCT_DEPENDENCY_GRAPH(initializer.getInitializerDependencyGraph(),
                               init0,
                               deinitNoop,
                               init1,
                               deinitNoop,
                               init2,
                               deinitNoop,
                               init3,
                               deinitNoop,
                               init4,
                               deinitNoop,
                               initNoop,
                               deinitNoop,
                               init6,
                               deinitNoop,
                               init7,
                               deinitNoop,
                               init8,
                               deinitNoop);
    clearCounts();

    ASSERT_EQUALS(ErrorCodes::UnknownError, initializer.executeInitializers({}));

    ASSERT_EQUALS(INITIALIZED, globalStates[0]);
    ASSERT_EQUALS(INITIALIZED, globalStates[1]);
    ASSERT_EQUALS(INITIALIZED, globalStates[2]);
    ASSERT_EQUALS(INITIALIZED, globalStates[3]);
    ASSERT_EQUALS(INITIALIZED, globalStates[4]);
    ASSERT_EQUALS(UNSET, globalStates[5]);
    ASSERT_EQUALS(INITIALIZED, globalStates[6]);
    ASSERT_EQUALS(INITIALIZED, globalStates[7]);
    ASSERT_EQUALS(UNSET, globalStates[8]);
}

TEST(InitializerTest, Deinit2Misimplemented) {
    Initializer initializer;
    CONSTRUCT_DEPENDENCY_GRAPH(initializer.getInitializerDependencyGraph(),
                               init0,
                               deinit0,
                               init1,
                               deinit1,
                               init2,
                               deinitNoop,
                               init3,
                               deinit3,
                               init4,
                               deinit4,
                               init5,
                               deinit5,
                               init6,
                               deinit6,
                               init7,
                               deinit7,
                               init8,
                               deinit8);
    clearCounts();

    ASSERT_OK(initializer.executeInitializers({}));

    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(INITIALIZED, globalStates[i]);

    ASSERT_EQUALS(ErrorCodes::UnknownError, initializer.executeDeinitializers());

    ASSERT_EQUALS(DEINITIALIZED, globalStates[8]);
    ASSERT_EQUALS(DEINITIALIZED, globalStates[7]);
    ASSERT_EQUALS(DEINITIALIZED, globalStates[6]);
    ASSERT_EQUALS(DEINITIALIZED, globalStates[5]);
    ASSERT_EQUALS(DEINITIALIZED, globalStates[4]);
    ASSERT_EQUALS(DEINITIALIZED, globalStates[3]);
    ASSERT_EQUALS(INITIALIZED, globalStates[2]);
    ASSERT_EQUALS(INITIALIZED, globalStates[1]);
    ASSERT_EQUALS(INITIALIZED, globalStates[0]);
}

DEATH_TEST(InitializerTest, CannotAddInitializerAfterInitializing, "!frozen()") {
    Initializer initializer;
    constructNormalDependencyGraph(&initializer);
    clearCounts();

    ASSERT_OK(initializer.executeInitializers({}));

    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(INITIALIZED, globalStates[i]);

    ASSERT_ADD_INITIALIZER(initializer.getInitializerDependencyGraph(),
                           "test",
                           initNoop,
                           deinitNoop,
                           MONGO_NO_PREREQUISITES,
                           MONGO_NO_DEPENDENTS);
}

DEATH_TEST(InitializerTest, CannotDoubleInitialize, "invalid initializer state transition") {
    Initializer initializer;
    constructNormalDependencyGraph(&initializer);
    clearCounts();

    ASSERT_OK(initializer.executeInitializers({}));

    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(INITIALIZED, globalStates[i]);

    initializer.executeInitializers({}).ignore();
}

DEATH_TEST(InitializerTest,
           CannotDeinitializeWithoutInitialize,
           "invalid initializer state transition") {
    Initializer initializer;
    constructNormalDependencyGraph(&initializer);
    clearCounts();

    initializer.executeDeinitializers().ignore();
}

DEATH_TEST(InitializerTest, CannotDoubleDeinitialize, "invalid initializer state transition") {
    Initializer initializer;
    constructNormalDependencyGraph(&initializer);
    clearCounts();

    ASSERT_OK(initializer.executeInitializers({}));

    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(INITIALIZED, globalStates[i]);

    ASSERT_OK(initializer.executeDeinitializers());

    for (int i = 0; i < 9; ++i)
        ASSERT_EQUALS(DEINITIALIZED, globalStates[i]);

    initializer.executeDeinitializers().ignore();
}

}  // namespace
}  // namespace mongo
