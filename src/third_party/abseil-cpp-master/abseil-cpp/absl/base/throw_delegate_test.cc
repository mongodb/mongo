// Copyright 2017 The Abseil Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/base/internal/throw_delegate.h"

#include <functional>
#include <new>
#include <stdexcept>

#include "gtest/gtest.h"

namespace {

using absl::base_internal::ThrowStdLogicError;
using absl::base_internal::ThrowStdInvalidArgument;
using absl::base_internal::ThrowStdDomainError;
using absl::base_internal::ThrowStdLengthError;
using absl::base_internal::ThrowStdOutOfRange;
using absl::base_internal::ThrowStdRuntimeError;
using absl::base_internal::ThrowStdRangeError;
using absl::base_internal::ThrowStdOverflowError;
using absl::base_internal::ThrowStdUnderflowError;
using absl::base_internal::ThrowStdBadFunctionCall;
using absl::base_internal::ThrowStdBadAlloc;

constexpr const char* what_arg = "The quick brown fox jumps over the lazy dog";

template <typename E>
void ExpectThrowChar(void (*f)(const char*)) {
  try {
    f(what_arg);
    FAIL() << "Didn't throw";
  } catch (const E& e) {
    EXPECT_STREQ(e.what(), what_arg);
  }
}

template <typename E>
void ExpectThrowString(void (*f)(const std::string&)) {
  try {
    f(what_arg);
    FAIL() << "Didn't throw";
  } catch (const E& e) {
    EXPECT_STREQ(e.what(), what_arg);
  }
}

template <typename E>
void ExpectThrowNoWhat(void (*f)()) {
  try {
    f();
    FAIL() << "Didn't throw";
  } catch (const E& e) {
  }
}

TEST(ThrowHelper, Test) {
  // Not using EXPECT_THROW because we want to check the .what() message too.
  ExpectThrowChar<std::logic_error>(ThrowStdLogicError);
  ExpectThrowChar<std::invalid_argument>(ThrowStdInvalidArgument);
  ExpectThrowChar<std::domain_error>(ThrowStdDomainError);
  ExpectThrowChar<std::length_error>(ThrowStdLengthError);
  ExpectThrowChar<std::out_of_range>(ThrowStdOutOfRange);
  ExpectThrowChar<std::runtime_error>(ThrowStdRuntimeError);
  ExpectThrowChar<std::range_error>(ThrowStdRangeError);
  ExpectThrowChar<std::overflow_error>(ThrowStdOverflowError);
  ExpectThrowChar<std::underflow_error>(ThrowStdUnderflowError);

  ExpectThrowString<std::logic_error>(ThrowStdLogicError);
  ExpectThrowString<std::invalid_argument>(ThrowStdInvalidArgument);
  ExpectThrowString<std::domain_error>(ThrowStdDomainError);
  ExpectThrowString<std::length_error>(ThrowStdLengthError);
  ExpectThrowString<std::out_of_range>(ThrowStdOutOfRange);
  ExpectThrowString<std::runtime_error>(ThrowStdRuntimeError);
  ExpectThrowString<std::range_error>(ThrowStdRangeError);
  ExpectThrowString<std::overflow_error>(ThrowStdOverflowError);
  ExpectThrowString<std::underflow_error>(ThrowStdUnderflowError);

  ExpectThrowNoWhat<std::bad_function_call>(ThrowStdBadFunctionCall);
  ExpectThrowNoWhat<std::bad_alloc>(ThrowStdBadAlloc);
}

}  // namespace
