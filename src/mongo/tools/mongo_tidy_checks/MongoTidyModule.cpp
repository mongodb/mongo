// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "MongoAssertCheck.h"
#include "MongoBannedCatalogAccessFromQueryCodeCheck.h"
#include "MongoBannedNamesCheck.h"
#include "MongoBypassDatabaseMetadataAccessCheck.h"
#include "MongoCctypeCheck.h"
#include "MongoConfigHeaderCheck.h"
#include "MongoCxx20BannedIncludesCheck.h"
#include "MongoCxx20StdChronoCheck.h"
#include "MongoFCVConstantCheck.h"
#include "MongoHeaderBracketCheck.h"
#include "MongoHeaderIncludePathCheck.h"
#include "MongoInvariantShardingCoordinatorCheck.h"
#include "MongoInvariantStatusIsOKCheck.h"
#include "MongoMacroDefinitionLeaksCheck.h"
#include "MongoNoUniqueAddressCheck.h"
#include "MongoRWMutexCheck.h"
#include "MongoRandCheck.h"
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
        CheckFactories.registerCheck<MongoHeaderIncludePathCheck>(
            "mongo-header-include-path-check");
        CheckFactories.registerCheck<MongoCctypeCheck>("mongo-cctype-check");
        CheckFactories.registerCheck<MongoConfigHeaderCheck>("mongo-config-header-check");
        CheckFactories.registerCheck<MongoCxx20BannedIncludesCheck>(
            "mongo-cxx20-banned-includes-check");
        CheckFactories.registerCheck<MongoCxx20StdChronoCheck>("mongo-cxx20-std-chrono-check");
        CheckFactories.registerCheck<MongoVolatileCheck>("mongo-volatile-check");
        CheckFactories.registerCheck<MongoTraceCheck>("mongo-trace-check");
        CheckFactories.registerCheck<MongoAssertCheck>("mongo-assert-check");
        CheckFactories.registerCheck<MongoFCVConstantCheck>("mongo-fcv-constant-check");
        CheckFactories.registerCheck<MongoUnstructuredLogCheck>("mongo-unstructured-log-check");
        CheckFactories.registerCheck<MongoMacroDefinitionLeaksCheck>(
            "mongo-macro-definition-leaks-check");
        CheckFactories.registerCheck<MongoRandCheck>("mongo-rand-check");
        CheckFactories.registerCheck<MongoBannedNamesCheck>("mongo-banned-names-check");
        CheckFactories.registerCheck<MongoNoUniqueAddressCheck>("mongo-no-unique-address-check");
        CheckFactories.registerCheck<MongoRWMutexCheck>("mongo-rwmutex-check");
        CheckFactories.registerCheck<MongoInvariantStatusIsOKCheck>(
            "mongo-invariant-status-is-ok-check");
        CheckFactories.registerCheck<InvariantShardingCoordinatorCheck>(
            "mongo-invariant-sharding-coordinator-check");
        CheckFactories.registerCheck<MongoBypassDatabaseMetadataAccessCheck>(
            "mongo-bypass-database-metadata-access-check");
        CheckFactories.registerCheck<MongoBannedCatalogAccessFromQueryCodeCheck>(
            "mongo-banned-catalog-access-from-query-code-check");
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
