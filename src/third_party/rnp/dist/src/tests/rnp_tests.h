/*
 * Copyright (c) 2017-2019 [Ribose Inc](https://www.ribose.com).
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1.  Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 * 2.  Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef RNP_TESTS_H
#define RNP_TESTS_H

#include <gtest/gtest.h>
#include "support.h"

class rnp_tests : public ::testing::Test {
  public:
    rnp_tests();
    virtual ~rnp_tests();

    const char *original_dir() const;

  protected:
    char *               m_dir;
    rnp::SecurityContext global_ctx;
};

typedef struct {
    char *original_dir;
    char *home;
    char *data_dir;
    int   not_fatal;
} rnp_test_state_t;

#if defined(RNP_TESTS_EXPECT)
#define assert_true(a) EXPECT_TRUE((a))
#define assert_false(a) EXPECT_FALSE((a))
#define assert_string_equal(a, b) EXPECT_STREQ((a), (b))
#define assert_int_equal(a, b) EXPECT_EQ((a), (b))
#define assert_int_not_equal(a, b) EXPECT_NE((a), (b))
#define assert_greater_than(a, b) EXPECT_GT((a), (b))
#define assert_non_null(a) EXPECT_NE((a), nullptr)
#define assert_null(a) EXPECT_EQ((a), nullptr)
#define assert_rnp_success(a) EXPECT_EQ((a), RNP_SUCCESS)
#define assert_rnp_failure(a) EXPECT_NE((a), RNP_SUCCESS)
#define assert_memory_equal(a, b, sz) EXPECT_EQ(0, memcmp((a), (b), (sz)))
#define assert_memory_not_equal(a, b, sz) EXPECT_NE(0, memcmp((a), (b), (sz)))
#define assert_throw(a) EXPECT_ANY_THROW(a)
#else
#define assert_true(a) ASSERT_TRUE((a))
#define assert_false(a) ASSERT_FALSE((a))
#define assert_string_equal(a, b) ASSERT_STREQ((a), (b))
#define assert_int_equal(a, b) ASSERT_EQ((a), (b))
#define assert_int_not_equal(a, b) ASSERT_NE((a), (b))
#define assert_greater_than(a, b) ASSERT_GT((a), (b))
#define assert_non_null(a) ASSERT_NE((a), nullptr)
#define assert_null(a) ASSERT_EQ((a), nullptr)
#define assert_rnp_success(a) ASSERT_EQ((a), RNP_SUCCESS)
#define assert_rnp_failure(a) ASSERT_NE((a), RNP_SUCCESS)
#define assert_memory_equal(a, b, sz) ASSERT_EQ(0, memcmp((a), (b), (sz)))
#define assert_memory_not_equal(a, b, sz) ASSERT_NE(0, memcmp((a), (b), (sz)))
#define assert_throw(a) ASSERT_ANY_THROW(a)
#endif

#endif // RNP_TESTS_H
