#include <signal.h>
#include <stdlib.h>

#include "ares-test.h"

int main(int argc, char* argv[]) {
  std::vector<char*> gtest_argv = {argv[0]};
  for (int ii = 1; ii < argc; ii++) {
    if (strcmp(argv[ii], "-v") == 0) {
      ares::test::verbose = true;
    } else if ((strcmp(argv[ii], "-p") == 0) && (ii + 1 < argc)) {
      ii++;
      ares::test::mock_port = atoi(argv[ii]);
    } else if (strcmp(argv[ii], "-4") == 0) {
      ares::test::families = ares::test::ipv4_family;
      ares::test::families_modes = ares::test::ipv4_family_both_modes;
    } else if (strcmp(argv[ii], "-6") == 0) {
      ares::test::families = ares::test::ipv6_family;
      ares::test::families_modes = ares::test::ipv6_family_both_modes;
    } else {
      gtest_argv.push_back(argv[ii]);
    }
  }
  int gtest_argc = gtest_argv.size();
  gtest_argv.push_back(nullptr);
  ::testing::InitGoogleTest(&gtest_argc, gtest_argv.data());

#ifdef WIN32
  WORD wVersionRequested = MAKEWORD(2, 2);
  WSADATA wsaData;
  WSAStartup(wVersionRequested, &wsaData);
#else
  signal(SIGPIPE, SIG_IGN);
#endif

  int rc = RUN_ALL_TESTS();

#ifdef WIN32
  WSACleanup();
#endif

  return rc;
}
