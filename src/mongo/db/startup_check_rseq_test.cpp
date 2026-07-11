// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/startup_check_rseq.h"

#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(StartupCheckRseq, SafeKernels) {
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("5.15.0-generic"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.18.99"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.0.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("4.19.0-aws"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("5.4.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.18.0"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6.18"));
}

TEST(StartupCheckRseq, UnsafeKernels) {
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.19"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.19.0"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.19.3-generic"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("6.20.0"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("7.0.0"));
    ASSERT_FALSE(isKernelVersionSafeForTCMallocPerCPUCache("10.0.0"));
}

TEST(StartupCheckRseq, UnparseableReturnsTrue) {
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache(""));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("invalid"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("abc.def"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache("6."));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache(".19"));
    ASSERT_TRUE(isKernelVersionSafeForTCMallocPerCPUCache(".6.19.0"));
}

}  // namespace
}  // namespace mongo
