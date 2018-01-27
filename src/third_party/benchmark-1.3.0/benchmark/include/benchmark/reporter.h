// Copyright 2015 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef BENCHMARK_REPORTER_H_
#define BENCHMARK_REPORTER_H_

#ifdef __DEPRECATED
# ifndef BENCHMARK_WARNING_MSG
#   warning the reporter.h header has been deprecated and will be removed, please include benchmark.h instead
# else
    BENCHMARK_WARNING_MSG("the reporter.h header has been deprecated and will be removed, please include benchmark.h instead")
# endif
#endif

#include "benchmark.h"  // For forward declaration of BenchmarkReporter

#endif  // BENCHMARK_REPORTER_H_
