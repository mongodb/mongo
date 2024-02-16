// Copyright 2020 The TCMalloc Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef TCMALLOC_TESTING_THREAD_MANAGER_H_
#define TCMALLOC_TESTING_THREAD_MANAGER_H_

#include <atomic>
#include <thread>
#include <vector>

#include "gtest/gtest.h"
#include "absl/synchronization/blocking_counter.h"

namespace tcmalloc {

class ThreadManager {
 public:
  ThreadManager() : shutdown_(false) {}
  ~ThreadManager() {
    EXPECT_TRUE(shutdown_.load()) << "ThreadManager not stopped";
  }

  void Start(int n, const std::function<void(int)>& func) {
    absl::BlockingCounter started(n);
    for (int i = 0; i < n; ++i) {
      threads_.emplace_back(
          [this, func, &started](int thread_id) {
            started.DecrementCount();
            while (!shutdown_.load()) {
              func(thread_id);
            }
          },
          i);
    }
    started.Wait();
  }

  void Stop() {
    shutdown_.store(true);
    for (auto& t : threads_) t.join();
  }

 private:
  std::atomic<bool> shutdown_;
  std::vector<std::thread> threads_;
};

}  // namespace tcmalloc

#endif  // TCMALLOC_TESTING_THREAD_MANAGER_H_
