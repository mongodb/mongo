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

#include "MongoAssertCheck.h"
#include "MongoCctypeCheck.h"
#include "MongoCollectionShardingRuntimeCheck.h"
#include "MongoConfigHeaderCheck.h"
#include "MongoCxx20BannedIncludesCheck.h"
#include "MongoCxx20StdChronoCheck.h"
#include "MongoFCVConstantCheck.h"
#include "MongoHeaderBracketCheck.h"
#include "MongoInvariantStatusIsOKCheck.h"
#include "MongoMacroDefinitionLeaksCheck.h"
#include "MongoNoUniqueAddressCheck.h"
#include "MongoPolyFillCheck.h"
#include "MongoRWMutexCheck.h"
#include "MongoRandCheck.h"
#include "MongoStdAtomicCheck.h"
#include "MongoStdOptionalCheck.h"
#include "MongoStringDataConstRefCheck.h"
#include "MongoTraceCheck.h"
#include "MongoUninterruptibleLockGuardCheck.h"
#include "MongoUnstructuredLogCheck.h"
#include "MongoVolatileCheck.h"

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>
#include <clang-tidy/ClangTidyModule.h>
#include <clang-tidy/ClangTidyModuleRegistry.h>

namespace mongo {
namespace tidy {

class MongoTidyModule : public clang::tidy::ClangTidyModule {
public:
    void addCheckFactories(clang::tidy::ClangTidyCheckFactories& CheckFactories) override {
        CheckFactories.registerCheck<MongoUninterruptibleLockGuardCheck>(
            "mongo-uninterruptible-lock-guard-check");
        CheckFactories.registerCheck<MongoHeaderBracketCheck>("mongo-header-bracket-check");
        CheckFactories.registerCheck<MongoCctypeCheck>("mongo-cctype-check");
        CheckFactories.registerCheck<MongoConfigHeaderCheck>("mongo-config-header-check");
        CheckFactories.registerCheck<MongoCxx20BannedIncludesCheck>(
            "mongo-cxx20-banned-includes-check");
        CheckFactories.registerCheck<MongoCxx20StdChronoCheck>("mongo-cxx20-std-chrono-check");
        CheckFactories.registerCheck<MongoStdOptionalCheck>("mongo-std-optional-check");
        CheckFactories.registerCheck<MongoVolatileCheck>("mongo-volatile-check");
        CheckFactories.registerCheck<MongoTraceCheck>("mongo-trace-check");
        CheckFactories.registerCheck<MongoStdAtomicCheck>("mongo-std-atomic-check");
        CheckFactories.registerCheck<MongoAssertCheck>("mongo-assert-check");
        CheckFactories.registerCheck<MongoFCVConstantCheck>("mongo-fcv-constant-check");
        CheckFactories.registerCheck<MongoUnstructuredLogCheck>("mongo-unstructured-log-check");
        CheckFactories.registerCheck<MongoCollectionShardingRuntimeCheck>(
            "mongo-collection-sharding-runtime-check");
        CheckFactories.registerCheck<MongoMacroDefinitionLeaksCheck>(
            "mongo-macro-definition-leaks-check");
        CheckFactories.registerCheck<MongoRandCheck>("mongo-rand-check");
        CheckFactories.registerCheck<MongoPolyFillCheck>("mongo-polyfill-check");
        CheckFactories.registerCheck<MongoNoUniqueAddressCheck>("mongo-no-unique-address-check");
        CheckFactories.registerCheck<MongoStringDataConstRefCheck>(
            "mongo-stringdata-const-ref-check");
        CheckFactories.registerCheck<MongoRWMutexCheck>("mongo-rwmutex-check");
        CheckFactories.registerCheck<MongoInvariantStatusIsOKCheck>(
            "mongo-invariant-status-is-ok-check");
    }
};

// Register the MongoTidyModule using this statically initialized variable.
static clang::tidy::ClangTidyModuleRegistry::Add<MongoTidyModule> X("mongo-tidy-module",
                                                                    "MongoDB custom checks.");

}  // namespace tidy

// This anchor is used to force the linker to link in the generated object file
// and thus register the MongoTidyModule.
volatile int MongoTidyModuleAnchorSource = 0;  // NOLINT

}  // namespace mongo
