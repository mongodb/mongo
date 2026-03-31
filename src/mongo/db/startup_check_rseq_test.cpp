/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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
