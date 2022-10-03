#include "ares-test.h"
#include "dns-proto.h"

#include <stdio.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <fcntl.h>

extern "C" {
// Remove command-line defines of package variables for the test project...
#undef PACKAGE_NAME
#undef PACKAGE_BUGREPORT
#undef PACKAGE_STRING
#undef PACKAGE_TARNAME
// ... so we can include the library's config without symbol redefinitions.
#include "ares_setup.h"
#include "ares_nowarn.h"
#include "ares_inet_net_pton.h"
#include "ares_data.h"
#include "ares_private.h"
#include "bitncmp.h"

#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#ifdef HAVE_SYS_UIO_H
#  include <sys/uio.h>
#endif
}

#include <string>
#include <vector>

namespace ares {
namespace test {

#ifndef CARES_SYMBOL_HIDING
void CheckPtoN4(int size, unsigned int value, const char *input) {
  struct in_addr a4;
  a4.s_addr = 0;
  uint32_t expected = htonl(value);
  EXPECT_EQ(size, ares_inet_net_pton(AF_INET, input, &a4, sizeof(a4)))
    << " for input " << input;
  EXPECT_EQ(expected, a4.s_addr) << " for input " << input;
}
#endif

TEST_F(LibraryTest, InetPtoN) {
  struct in_addr a4;
  struct in6_addr a6;

#ifndef CARES_SYMBOL_HIDING
  uint32_t expected;

  CheckPtoN4(4 * 8, 0x01020304, "1.2.3.4");
  CheckPtoN4(4 * 8, 0x81010101, "129.1.1.1");
  CheckPtoN4(4 * 8, 0xC0010101, "192.1.1.1");
  CheckPtoN4(4 * 8, 0xE0010101, "224.1.1.1");
  CheckPtoN4(4 * 8, 0xE1010101, "225.1.1.1");
  CheckPtoN4(4, 0xE0000000, "224");
  CheckPtoN4(4 * 8, 0xFD000000, "253");
  CheckPtoN4(4 * 8, 0xF0010101, "240.1.1.1");
  CheckPtoN4(4 * 8, 0x02030405, "02.3.4.5");
  CheckPtoN4(3 * 8, 0x01020304, "1.2.3.4/24");
  CheckPtoN4(3 * 8, 0x01020300, "1.2.3/24");
  CheckPtoN4(2 * 8, 0xa0000000, "0xa");
  CheckPtoN4(0, 0x02030405, "2.3.4.5/000");
  CheckPtoN4(1 * 8, 0x01020000, "1.2/8");
  CheckPtoN4(2 * 8, 0x01020000, "0x0102/16");
  CheckPtoN4(4 * 8, 0x02030405, "02.3.4.5");

  EXPECT_EQ(16 * 8, ares_inet_net_pton(AF_INET6, "::", &a6, sizeof(a6)));
  EXPECT_EQ(16 * 8, ares_inet_net_pton(AF_INET6, "::1", &a6, sizeof(a6)));
  EXPECT_EQ(16 * 8, ares_inet_net_pton(AF_INET6, "1234:5678::", &a6, sizeof(a6)));
  EXPECT_EQ(16 * 8, ares_inet_net_pton(AF_INET6, "12:34::ff", &a6, sizeof(a6)));
  EXPECT_EQ(16 * 8, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3.4", &a6, sizeof(a6)));
  EXPECT_EQ(23, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3.4/23", &a6, sizeof(a6)));
  EXPECT_EQ(3 * 8, ares_inet_net_pton(AF_INET6, "12:34::ff/24", &a6, sizeof(a6)));
  EXPECT_EQ(0, ares_inet_net_pton(AF_INET6, "12:34::ff/0", &a6, sizeof(a6)));
  EXPECT_EQ(16 * 8, ares_inet_net_pton(AF_INET6, "12:34::ffff:0.2", &a6, sizeof(a6)));
  EXPECT_EQ(16 * 8, ares_inet_net_pton(AF_INET6, "1234:1234:1234:1234:1234:1234:1234:1234", &a6, sizeof(a6)));

  // Various malformed versions
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, " ", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x ", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "x0", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0xXYZZY", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "xyzzy", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET+AF_INET6, "1.2.3.4", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "257.2.3.4", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "002.3.4.x", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "00.3.4.x", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "2.3.4.x", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "2.3.4.5.6", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "2.3.4.5.6/12", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "2.3.4:5", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "2.3.4.5/120", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "2.3.4.5/1x", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "2.3.4.5/x", &a4, sizeof(a4)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ff/240", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ff/02", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ff/2y", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ff/y", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ff/", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, ":x", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, ":", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, ": :1234", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "::12345", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "1234::2345:3456::0011", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "1234:1234:1234:1234:1234:1234:1234:1234:", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "1234:1234:1234:1234:1234:1234:1234:1234::", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "1234:1234:1234:1234:1234:1234:1234:1.2.3.4", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, ":1234:1234:1234:1234:1234:1234:1234:1234", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, ":1234:1234:1234:1234:1234:1234:1234:1234:", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "1234:1234:1234:1234:1234:1234:1234:1234:5678", &a6, sizeof(a6)));
  // TODO(drysdale): check whether the next two tests should give -1.
  EXPECT_EQ(0, ares_inet_net_pton(AF_INET6, "1234:1234:1234:1234:1234:1234:1234:1234:5678:5678", &a6, sizeof(a6)));
  EXPECT_EQ(0, ares_inet_net_pton(AF_INET6, "1234:1234:1234:1234:1234:1234:1234:1234:5678:5678:5678", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:257.2.3.4", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:002.2.3.4", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3.4.5.6", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3.4.5", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3.z", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3001.4", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3..4", &a6, sizeof(a6)));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ffff:1.2.3.", &a6, sizeof(a6)));

  // Hex constants are allowed.
  EXPECT_EQ(4 * 8, ares_inet_net_pton(AF_INET, "0x01020304", &a4, sizeof(a4)));
  expected = htonl(0x01020304);
  EXPECT_EQ(expected, a4.s_addr);
  EXPECT_EQ(4 * 8, ares_inet_net_pton(AF_INET, "0x0a0b0c0d", &a4, sizeof(a4)));
  expected = htonl(0x0a0b0c0d);
  EXPECT_EQ(expected, a4.s_addr);
  EXPECT_EQ(4 * 8, ares_inet_net_pton(AF_INET, "0x0A0B0C0D", &a4, sizeof(a4)));
  expected = htonl(0x0a0b0c0d);
  EXPECT_EQ(expected, a4.s_addr);
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x0xyz", &a4, sizeof(a4)));
  EXPECT_EQ(4 * 8, ares_inet_net_pton(AF_INET, "0x1122334", &a4, sizeof(a4)));
  expected = htonl(0x11223340);
  EXPECT_EQ(expected, a4.s_addr);  // huh?

  // No room, no room.
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "1.2.3.4", &a4, sizeof(a4) - 1));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET6, "12:34::ff", &a6, sizeof(a6) - 1));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x01020304", &a4, 2));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x01020304", &a4, 0));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x0a0b0c0d", &a4, 0));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x0xyz", &a4, 0));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "0x1122334", &a4, sizeof(a4) - 1));
  EXPECT_EQ(-1, ares_inet_net_pton(AF_INET, "253", &a4, sizeof(a4) - 1));
#endif

  EXPECT_EQ(1, ares_inet_pton(AF_INET, "1.2.3.4", &a4));
  EXPECT_EQ(1, ares_inet_pton(AF_INET6, "12:34::ff", &a6));
  EXPECT_EQ(1, ares_inet_pton(AF_INET6, "12:34::ffff:1.2.3.4", &a6));
  EXPECT_EQ(0, ares_inet_pton(AF_INET, "xyzzy", &a4));
  EXPECT_EQ(-1, ares_inet_pton(AF_INET+AF_INET6, "1.2.3.4", &a4));
}

TEST_F(LibraryTest, FreeCorruptData) {
  // ares_free_data(p) expects that there is a type field and a marker
  // field in the memory before p.  Feed it incorrect versions of each.
  struct ares_data *data = (struct ares_data *)malloc(sizeof(struct ares_data));
  void* p = &(data->data);

  // Invalid type
  data->type = (ares_datatype)99;
  data->mark = ARES_DATATYPE_MARK;
  ares_free_data(p);

  // Invalid marker
  data->type = (ares_datatype)ARES_DATATYPE_MX_REPLY;
  data->mark = ARES_DATATYPE_MARK + 1;
  ares_free_data(p);

  // Null pointer
  ares_free_data(nullptr);

  free(data);
}

#ifndef CARES_SYMBOL_HIDING
TEST_F(LibraryTest, FreeLongChain) {
  struct ares_addr_node *data = nullptr;
  for (int ii = 0; ii < 100000; ii++) {
    struct ares_addr_node *prev = (struct ares_addr_node*)ares_malloc_data(ARES_DATATYPE_ADDR_NODE);
    prev->next = data;
    data = prev;
  }

  ares_free_data(data);
}

TEST(LibraryInit, StrdupFailures) {
  EXPECT_EQ(ARES_SUCCESS, ares_library_init(ARES_LIB_INIT_ALL));
  char* copy = ares_strdup("string");
  EXPECT_NE(nullptr, copy);
  ares_free(copy);
  ares_library_cleanup();
}

TEST_F(LibraryTest, StrdupFailures) {
  SetAllocFail(1);
  char* copy = ares_strdup("string");
  EXPECT_EQ(nullptr, copy);
}

TEST_F(LibraryTest, MallocDataFail) {
  EXPECT_EQ(nullptr, ares_malloc_data((ares_datatype)99));
  SetAllocSizeFail(sizeof(struct ares_data));
  EXPECT_EQ(nullptr, ares_malloc_data(ARES_DATATYPE_MX_REPLY));
}

TEST(Misc, Bitncmp) {
  byte a[4] = {0x80, 0x01, 0x02, 0x03};
  byte b[4] = {0x80, 0x01, 0x02, 0x04};
  byte c[4] = {0x01, 0xFF, 0x80, 0x02};
  EXPECT_GT(0, ares__bitncmp(a, b, sizeof(a)*8));
  EXPECT_LT(0, ares__bitncmp(b, a, sizeof(a)*8));
  EXPECT_EQ(0, ares__bitncmp(a, a, sizeof(a)*8));

  for (int ii = 1; ii < (3*8+5); ii++) {
    EXPECT_EQ(0, ares__bitncmp(a, b, ii));
    EXPECT_EQ(0, ares__bitncmp(b, a, ii));
    EXPECT_LT(0, ares__bitncmp(a, c, ii));
    EXPECT_GT(0, ares__bitncmp(c, a, ii));
  }

  // Last byte differs at 5th bit
  EXPECT_EQ(0, ares__bitncmp(a, b, 3*8 + 3));
  EXPECT_EQ(0, ares__bitncmp(a, b, 3*8 + 4));
  EXPECT_EQ(0, ares__bitncmp(a, b, 3*8 + 5));
  EXPECT_GT(0, ares__bitncmp(a, b, 3*8 + 6));
  EXPECT_GT(0, ares__bitncmp(a, b, 3*8 + 7));
}

TEST_F(LibraryTest, Casts) {
  ares_ssize_t ssz = 100;
  unsigned int u = 100;
  int i = 100;
  long l = 100;

  unsigned int ru = aresx_sztoui(ssz);
  EXPECT_EQ(u, ru);
  int ri = aresx_sztosi(ssz);
  EXPECT_EQ(i, ri);

  ri = aresx_sltosi(l);
  EXPECT_EQ(l, (long)ri);
}

TEST_F(LibraryTest, ReadLine) {
  TempFile temp("abcde\n0123456789\nXYZ\n012345678901234567890\n\n");
  FILE *fp = fopen(temp.filename(), "r");
  size_t bufsize = 4;
  char *buf = (char *)ares_malloc(bufsize);

  EXPECT_EQ(ARES_SUCCESS, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ("abcde", std::string(buf));
  EXPECT_EQ(ARES_SUCCESS, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ("0123456789", std::string(buf));
  EXPECT_EQ(ARES_SUCCESS, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ("XYZ", std::string(buf));
  SetAllocFail(1);
  EXPECT_EQ(ARES_ENOMEM, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ(nullptr, buf);

  fclose(fp);
  ares_free(buf);
}

TEST_F(LibraryTest, ReadLineNoBuf) {
  TempFile temp("abcde\n0123456789\nXYZ\n012345678901234567890");
  FILE *fp = fopen(temp.filename(), "r");
  size_t bufsize = 0;
  char *buf = nullptr;

  SetAllocFail(1);
  EXPECT_EQ(ARES_ENOMEM, ares__read_line(fp, &buf, &bufsize));

  EXPECT_EQ(ARES_SUCCESS, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ("abcde", std::string(buf));
  EXPECT_EQ(ARES_SUCCESS, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ("0123456789", std::string(buf));
  EXPECT_EQ(ARES_SUCCESS, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ("XYZ", std::string(buf));
  EXPECT_EQ(ARES_SUCCESS, ares__read_line(fp, &buf, &bufsize));
  EXPECT_EQ("012345678901234567890", std::string(buf));

  fclose(fp);
  ares_free(buf);
}

TEST(Misc, GetHostent) {
  TempFile hostsfile("1.2.3.4 example.com  \n"
                     "  2.3.4.5\tgoogle.com   www.google.com\twww2.google.com\n"
                     "#comment\n"
                     "4.5.6.7\n"
                     "1.3.5.7  \n"
                     "::1    ipv6.com");
  struct hostent *host = nullptr;
  FILE *fp = fopen(hostsfile.filename(), "r");
  ASSERT_NE(nullptr, fp);
  EXPECT_EQ(ARES_EBADFAMILY, ares__get_hostent(fp, AF_INET+AF_INET6, &host));
  rewind(fp);

  EXPECT_EQ(ARES_SUCCESS, ares__get_hostent(fp, AF_INET, &host));
  ASSERT_NE(nullptr, host);
  std::stringstream ss1;
  ss1 << HostEnt(host);
  EXPECT_EQ("{'example.com' aliases=[] addrs=[1.2.3.4]}", ss1.str());
  ares_free_hostent(host);
  host = nullptr;

  EXPECT_EQ(ARES_SUCCESS, ares__get_hostent(fp, AF_INET, &host));
  ASSERT_NE(nullptr, host);
  std::stringstream ss2;
  ss2 << HostEnt(host);
  EXPECT_EQ("{'google.com' aliases=[www.google.com, www2.google.com] addrs=[2.3.4.5]}", ss2.str());
  ares_free_hostent(host);
  host = nullptr;

  EXPECT_EQ(ARES_EOF, ares__get_hostent(fp, AF_INET, &host));

  rewind(fp);
  EXPECT_EQ(ARES_SUCCESS, ares__get_hostent(fp, AF_INET6, &host));
  ASSERT_NE(nullptr, host);
  std::stringstream ss3;
  ss3 << HostEnt(host);
  EXPECT_EQ("{'ipv6.com' aliases=[] addrs=[0000:0000:0000:0000:0000:0000:0000:0001]}", ss3.str());
  ares_free_hostent(host);
  host = nullptr;
  EXPECT_EQ(ARES_EOF, ares__get_hostent(fp, AF_INET6, &host));
  fclose(fp);
}

TEST_F(LibraryTest, GetHostentAllocFail) {
  TempFile hostsfile("1.2.3.4 example.com alias1 alias2\n");
  struct hostent *host = nullptr;
  FILE *fp = fopen(hostsfile.filename(), "r");
  ASSERT_NE(nullptr, fp);

  for (int ii = 1; ii <= 8; ii++) {
    rewind(fp);
    ClearFails();
    SetAllocFail(ii);
    host = nullptr;
    EXPECT_EQ(ARES_ENOMEM, ares__get_hostent(fp, AF_INET, &host)) << ii;
  }
  fclose(fp);
}

TEST_F(DefaultChannelTest, GetAddrInfoHostsPositive) {
  TempFile hostsfile("1.2.3.4 example.com  \n"
                     "  2.3.4.5\tgoogle.com   www.google.com\twww2.google.com\n"
                     "#comment\n"
                     "4.5.6.7\n"
                     "1.3.5.7  \n"
                     "::1    ipv6.com");
  EnvValue with_env("CARES_HOSTS", hostsfile.filename());
  struct ares_addrinfo_hints hints = {};
  AddrInfoResult result = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_CANONNAME | ARES_AI_ENVHOSTS | ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "example.com", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.ai_;
  EXPECT_EQ("{example.com addr=[1.2.3.4]}", ss.str());
}

TEST_F(DefaultChannelTest, GetAddrInfoHostsSpaces) {
  TempFile hostsfile("1.2.3.4 example.com  \n"
                     "  2.3.4.5\tgoogle.com   www.google.com\twww2.google.com\n"
                     "#comment\n"
                     "4.5.6.7\n"
                     "1.3.5.7  \n"
                     "::1    ipv6.com");
  EnvValue with_env("CARES_HOSTS", hostsfile.filename());
  struct ares_addrinfo_hints hints = {};
  AddrInfoResult result = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_CANONNAME | ARES_AI_ENVHOSTS | ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "google.com", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.ai_;
  EXPECT_EQ("{www.google.com->google.com, www2.google.com->google.com addr=[2.3.4.5]}", ss.str());
}

TEST_F(DefaultChannelTest, GetAddrInfoHostsByALias) {
  TempFile hostsfile("1.2.3.4 example.com  \n"
                     "  2.3.4.5\tgoogle.com   www.google.com\twww2.google.com\n"
                     "#comment\n"
                     "4.5.6.7\n"
                     "1.3.5.7  \n"
                     "::1    ipv6.com");
  EnvValue with_env("CARES_HOSTS", hostsfile.filename());
  struct ares_addrinfo_hints hints = {};
  AddrInfoResult result = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_CANONNAME | ARES_AI_ENVHOSTS | ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www2.google.com", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.ai_;
  EXPECT_EQ("{www.google.com->google.com, www2.google.com->google.com addr=[2.3.4.5]}", ss.str());
}

TEST_F(DefaultChannelTest, GetAddrInfoHostsIPV6) {
  TempFile hostsfile("1.2.3.4 example.com  \n"
                     "  2.3.4.5\tgoogle.com   www.google.com\twww2.google.com\n"
                     "#comment\n"
                     "4.5.6.7\n"
                     "1.3.5.7  \n"
                     "::1    ipv6.com");
  EnvValue with_env("CARES_HOSTS", hostsfile.filename());
  struct ares_addrinfo_hints hints = {};
  AddrInfoResult result = {};
  hints.ai_family = AF_INET6;
  hints.ai_flags = ARES_AI_CANONNAME | ARES_AI_ENVHOSTS | ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "ipv6.com", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.ai_;
  EXPECT_EQ("{ipv6.com addr=[[0000:0000:0000:0000:0000:0000:0000:0001]]}", ss.str());
}

TEST_F(LibraryTest, GetAddrInfoAllocFail) {
  TempFile hostsfile("1.2.3.4 example.com alias1 alias2\n");
  struct ares_addrinfo_hints hints;
  unsigned short port = 80;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;

  FILE *fp = fopen(hostsfile.filename(), "r");
  ASSERT_NE(nullptr, fp);

  for (int ii = 1; ii <= 3; ii++) {
    rewind(fp);
    ClearFails();
    SetAllocFail(ii);
    struct ares_addrinfo ai;
    EXPECT_EQ(ARES_ENOMEM, ares__readaddrinfo(fp, "example.com", port, &hints, &ai)) << ii;
  }
  fclose(fp);
}

TEST(Misc, OnionDomain) {
  EXPECT_EQ(0, ares__is_onion_domain("onion.no"));
  EXPECT_EQ(0, ares__is_onion_domain(".onion.no"));
  EXPECT_EQ(1, ares__is_onion_domain(".onion"));
  EXPECT_EQ(1, ares__is_onion_domain(".onion."));
  EXPECT_EQ(1, ares__is_onion_domain("yes.onion"));
  EXPECT_EQ(1, ares__is_onion_domain("yes.onion."));
  EXPECT_EQ(1, ares__is_onion_domain("YES.ONION"));
  EXPECT_EQ(1, ares__is_onion_domain("YES.ONION."));
}
#endif

#ifdef CARES_EXPOSE_STATICS
// These tests access internal static functions from the library, which
// are only exposed when CARES_EXPOSE_STATICS has been configured. As such
// they are tightly couple to the internal library implementation details.
extern "C" char *ares_striendstr(const char*, const char*);
TEST_F(LibraryTest, Striendstr) {
  EXPECT_EQ(nullptr, ares_striendstr("abc", "12345"));
  EXPECT_NE(nullptr, ares_striendstr("abc12345", "12345"));
  EXPECT_NE(nullptr, ares_striendstr("abcxyzzy", "XYZZY"));
  EXPECT_NE(nullptr, ares_striendstr("xyzzy", "XYZZY"));
  EXPECT_EQ(nullptr, ares_striendstr("xyxzy", "XYZZY"));
  EXPECT_NE(nullptr, ares_striendstr("", ""));
  const char *str = "plugh";
  EXPECT_NE(nullptr, ares_striendstr(str, str));
}
extern "C" int ares__single_domain(ares_channel, const char*, char**);
TEST_F(DefaultChannelTest, SingleDomain) {
  TempFile aliases("www www.google.com\n");
  EnvValue with_env("HOSTALIASES", aliases.filename());

  SetAllocSizeFail(128);
  char *ptr = nullptr;
  EXPECT_EQ(ARES_ENOMEM, ares__single_domain(channel_, "www", &ptr));

  channel_->flags |= ARES_FLAG_NOSEARCH|ARES_FLAG_NOALIASES;
  EXPECT_EQ(ARES_SUCCESS, ares__single_domain(channel_, "www", &ptr));
  EXPECT_EQ("www", std::string(ptr));
  ares_free(ptr);
  ptr = nullptr;

  SetAllocFail(1);
  EXPECT_EQ(ARES_ENOMEM, ares__single_domain(channel_, "www", &ptr));
  EXPECT_EQ(nullptr, ptr);
}
#endif

TEST_F(DefaultChannelTest, SaveInvalidChannel) {
  int saved = channel_->nservers;
  channel_->nservers = -1;
  struct ares_options opts;
  int optmask = 0;
  EXPECT_EQ(ARES_ENODATA, ares_save_options(channel_, &opts, &optmask));
  channel_->nservers = saved;
}

// Need to put this in own function due to nested lambda bug
// in VS2013. (C2888)
static int configure_socket(ares_socket_t s) {
  // transposed from ares-process, simplified non-block setter.
#if defined(USE_BLOCKING_SOCKETS)
  return 0; /* returns success */
#elif defined(HAVE_FCNTL_O_NONBLOCK)
  /* most recent unix versions */
  int flags;
  flags = fcntl(s, F_GETFL, 0);
  return fcntl(s, F_SETFL, flags | O_NONBLOCK);
#elif defined(HAVE_IOCTL_FIONBIO)
  /* older unix versions */
  int flags = 1;
  return ioctl(s, FIONBIO, &flags);
#elif defined(HAVE_IOCTLSOCKET_FIONBIO)
#ifdef WATT32
  char flags = 1;
#else
  /* Windows */
  unsigned long flags = 1UL;
#endif
  return ioctlsocket(s, FIONBIO, &flags);
#elif defined(HAVE_IOCTLSOCKET_CAMEL_FIONBIO)
  /* Amiga */
  long flags = 1L;
  return IoctlSocket(s, FIONBIO, flags);
#elif defined(HAVE_SETSOCKOPT_SO_NONBLOCK)
  /* BeOS */
  long b = 1L;
  return setsockopt(s, SOL_SOCKET, SO_NONBLOCK, &b, sizeof(b));
#else
#  error "no non-blocking method was found/used/set"
#endif
}

// TODO: This should not really be in this file, but we need ares config
// flags, and here they are available.
const struct ares_socket_functions VirtualizeIO::default_functions = {
  [](int af, int type, int protocol, void *) -> ares_socket_t {
    auto s = ::socket(af, type, protocol);
    if (s == ARES_SOCKET_BAD) {
      return s;
    }
    if (configure_socket(s) != 0) {
      sclose(s);
      return ares_socket_t(-1);
    }
    return s;
  },
  [](ares_socket_t s, void * p) {
    return :: sclose(s);
  },
  [](ares_socket_t s, const struct sockaddr * addr, socklen_t len, void *) {
    return ::connect(s, addr, len);
  },
  [](ares_socket_t s, void * dst, size_t len, int flags, struct sockaddr * addr, socklen_t * alen, void *) -> ares_ssize_t {
#ifdef HAVE_RECVFROM
    return ::recvfrom(s, reinterpret_cast<RECV_TYPE_ARG2>(dst), len, flags, addr, alen);
#else
    return sread(s, dst, len);
#endif
  },
  [](ares_socket_t s, const struct iovec * vec, int len, void *) {
#ifndef HAVE_WRITEV
    return ares_writev(s, vec, len);
#else
    return :: writev(s, vec, len);
#endif
  }
};


}  // namespace test
}  // namespace ares
