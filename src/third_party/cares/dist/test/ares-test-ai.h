#ifndef ARES_TEST_AI_H
#define ARES_TEST_AI_H

#include <utility>
#include "gtest/gtest.h"
#include "gmock/gmock.h"
#include "ares-test.h"

namespace ares {
namespace test {

class MockChannelTestAI
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface< std::pair<int, bool> > {
 public:
  MockChannelTestAI() : MockChannelOptsTest(1, GetParam().first, GetParam().second, nullptr, 0) {}
};

class MockUDPChannelTestAI
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface<int> {
 public:
  MockUDPChannelTestAI() : MockChannelOptsTest(1, GetParam(), false, nullptr, 0) {}
};

class MockTCPChannelTestAI
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface<int> {
 public:
  MockTCPChannelTestAI() : MockChannelOptsTest(1, GetParam(), true, nullptr, 0) {}
};


// Test fixture that uses a default channel.
class DefaultChannelTestAI : public LibraryTest {
 public:
  DefaultChannelTestAI() : channel_(nullptr) {
    EXPECT_EQ(ARES_SUCCESS, ares_init(&channel_));
    EXPECT_NE(nullptr, channel_);
  }

  ~DefaultChannelTestAI() {
    ares_destroy(channel_);
    channel_ = nullptr;
  }

  // Process all pending work on ares-owned file descriptors.
  void Process();

 protected:
  ares_channel channel_;
};

}
}

#endif
