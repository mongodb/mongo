// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/startup_check_rseq.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(StartupCheckRseq, SafeKernelsOld) {
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("5.15.0-generic"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.18.99"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.0.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("4.19.0-aws"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("5.4.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.18.0"));
}

TEST(StartupCheckRseq, SafeKernelsUpdated) {
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.99"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.14"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.15"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.15-aws"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.1.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.2.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.9.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.99.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.99.9"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("7.99.9-generic"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("8.0.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("8.0.9"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("8.1.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("8.2.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("8.9.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("8.10.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("9.0.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("9.0.9"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("9.1.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("9.2.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("9.9.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("9.10.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("10.0.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("10.9.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("10.0.9"));
}

TEST(StartupCheckRseq, UnsafeKernels) {
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.19.0"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.19.5-aws"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.19.3-generic"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.20.0"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.0"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.9"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.10"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.11"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.11-aws"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.13"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.13-generic"));
}

TEST(StartupCheckRseq, UnparseableReturnsTrue) {
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache(""));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("invalid"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("abc.def"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6."));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache(".19"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache(".6.19.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.18"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.19"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.19."));
}

}  // namespace
}  // namespace mongo
