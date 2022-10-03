#include "ares-test-ai.h"
#include "dns-proto.h"

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include <sstream>
#include <vector>

using testing::InvokeWithoutArgs;
using testing::DoAll;

namespace ares {
namespace test {

MATCHER_P(IncludesNumAddresses, n, "") {
  if(!arg)
    return false;
  int cnt = 0;
  for (const ares_addrinfo_node* ai = arg->nodes; ai != NULL; ai = ai->ai_next)
    cnt++;
  return n == cnt;
}

MATCHER_P(IncludesV4Address, address, "") {
  if(!arg)
    return false;
  in_addr addressnum = {};
  if (!ares_inet_pton(AF_INET, address, &addressnum))
    return false; // wrong number format?
  for (const ares_addrinfo_node* ai = arg->nodes; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET)
      continue;
    if (reinterpret_cast<sockaddr_in*>(ai->ai_addr)->sin_addr.s_addr ==
        addressnum.s_addr)
      return true; // found
  }
  return false;
}

MATCHER_P(IncludesV6Address, address, "") {
  if(!arg)
    return false;
  in6_addr addressnum = {};
  if (!ares_inet_pton(AF_INET6, address, &addressnum)) {
    return false; // wrong number format?
  }
  for (const ares_addrinfo_node* ai = arg->nodes; ai != NULL; ai = ai->ai_next) {
    if (ai->ai_family != AF_INET6)
      continue;
    if (!memcmp(
        reinterpret_cast<sockaddr_in6*>(ai->ai_addr)->sin6_addr.s6_addr,
        addressnum.s6_addr, sizeof(addressnum.s6_addr)))
      return true; // found
  }
  return false;
}

// UDP only so mock server doesn't get confused by concatenated requests
TEST_P(MockUDPChannelTestAI, GetAddrInfoParallelLookups) {
  DNSPacket rsp1;
  rsp1.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp1));
  DNSPacket rsp2;
  rsp2.set_response().set_aa()
    .add_question(new DNSQuestion("www.example.com", T_A))
    .add_answer(new DNSARR("www.example.com", 100, {1, 2, 3, 4}));
  ON_CALL(server_, OnRequest("www.example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp2));

  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  AddrInfoResult result1;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result1);
  AddrInfoResult result2;
  ares_getaddrinfo(channel_, "www.example.com.", NULL, &hints, AddrInfoCallback, &result2);
  AddrInfoResult result3;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result3);
  Process();

  EXPECT_TRUE(result1.done_);
  EXPECT_EQ(result1.status_, ARES_SUCCESS);
  EXPECT_THAT(result1.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result1.ai_, IncludesV4Address("2.3.4.5"));

  EXPECT_TRUE(result2.done_);
  EXPECT_EQ(result2.status_, ARES_SUCCESS);
  EXPECT_THAT(result2.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result2.ai_, IncludesV4Address("1.2.3.4"));

  EXPECT_TRUE(result3.done_);
  EXPECT_EQ(result3.status_, ARES_SUCCESS);
  EXPECT_THAT(result3.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result3.ai_, IncludesV4Address("2.3.4.5"));
}

// UDP to TCP specific test
TEST_P(MockUDPChannelTestAI, TruncationRetry) {
  DNSPacket rsptruncated;
  rsptruncated.set_response().set_aa().set_tc()
    .add_question(new DNSQuestion("www.google.com", T_A));
  DNSPacket rspok;
  rspok.set_response()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {1, 2, 3, 4}));
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsptruncated))
    .WillOnce(SetReply(&server_, &rspok));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(result.status_, ARES_SUCCESS);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV4Address("1.2.3.4"));
}

// TCP only to prevent retries
TEST_P(MockTCPChannelTestAI, MalformedResponse) {
  std::vector<byte> one = {0x01};
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReplyData(&server_, one));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ETIMEOUT, result.status_);
}

TEST_P(MockTCPChannelTestAI, FormErrResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(FORMERR);
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EFORMERR, result.status_);
}

TEST_P(MockTCPChannelTestAI, ServFailResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(SERVFAIL);
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  // ARES_FLAG_NOCHECKRESP not set, so SERVFAIL consumed
  EXPECT_EQ(ARES_ECONNREFUSED, result.status_);
}

TEST_P(MockTCPChannelTestAI, NotImplResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(NOTIMP);
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  // ARES_FLAG_NOCHECKRESP not set, so NOTIMP consumed
  EXPECT_EQ(ARES_ECONNREFUSED, result.status_);
}

TEST_P(MockTCPChannelTestAI, RefusedResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(REFUSED);
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  // ARES_FLAG_NOCHECKRESP not set, so REFUSED consumed
  EXPECT_EQ(ARES_ECONNREFUSED, result.status_);
}

TEST_P(MockTCPChannelTestAI, YXDomainResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(YXDOMAIN);
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENODATA, result.status_);
}

class MockExtraOptsTestAI
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface< std::pair<int, bool> > {
 public:
  MockExtraOptsTestAI()
    : MockChannelOptsTest(1, GetParam().first, GetParam().second,
                          FillOptions(&opts_),
                          ARES_OPT_SOCK_SNDBUF|ARES_OPT_SOCK_RCVBUF) {}
  static struct ares_options* FillOptions(struct ares_options * opts) {
    memset(opts, 0, sizeof(struct ares_options));
    // Set a few options that affect socket communications
    opts->socket_send_buffer_size = 514;
    opts->socket_receive_buffer_size = 514;
    return opts;
  }
 private:
  struct ares_options opts_;
};

TEST_P(MockExtraOptsTestAI, SimpleQuery) {
  ares_set_local_ip4(channel_, 0x7F000001);
  byte addr6[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  ares_set_local_ip6(channel_, addr6);
  ares_set_local_dev(channel_, "dummy");

  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_SUCCESS, result.status_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV4Address("2.3.4.5"));
}

class MockExtraOptsNDotsTestAI
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface< std::pair<int, bool> > {
 public:
  MockExtraOptsNDotsTestAI(int ndots)
    : MockChannelOptsTest(1, GetParam().first, GetParam().second,
                          FillOptions(&opts_, ndots),
                          ARES_OPT_SOCK_SNDBUF|ARES_OPT_SOCK_RCVBUF|ARES_OPT_NDOTS) {}
  static struct ares_options* FillOptions(struct ares_options * opts, int ndots) {
    memset(opts, 0, sizeof(struct ares_options));
    // Set a few options that affect socket communications
    opts->socket_send_buffer_size = 514;
    opts->socket_receive_buffer_size = 514;
    opts->ndots = ndots;
    return opts;
  }
 private:
  struct ares_options opts_;
};

class MockExtraOptsNDots5TestAI : public MockExtraOptsNDotsTestAI {
 public:
  MockExtraOptsNDots5TestAI() : MockExtraOptsNDotsTestAI(5) {}
};

TEST_P(MockExtraOptsNDots5TestAI, SimpleQuery) {
  ares_set_local_ip4(channel_, 0x7F000001);
  byte addr6[16] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
  ares_set_local_ip6(channel_, addr6);
  ares_set_local_dev(channel_, "dummy");

  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("dynamodb.us-east-1.amazonaws.com", T_A))
    .add_answer(new DNSARR("dynamodb.us-east-1.amazonaws.com", 100, {123, 45, 67, 8}));
  ON_CALL(server_, OnRequest("dynamodb.us-east-1.amazonaws.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "dynamodb.us-east-1.amazonaws.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_SUCCESS, result.status_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV4Address("123.45.67.8"));
}

class MockFlagsChannelOptsTestAI
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface< std::pair<int, bool> > {
 public:
  MockFlagsChannelOptsTestAI(int flags)
    : MockChannelOptsTest(1, GetParam().first, GetParam().second,
                          FillOptions(&opts_, flags), ARES_OPT_FLAGS) {}
  static struct ares_options* FillOptions(struct ares_options * opts, int flags) {
    memset(opts, 0, sizeof(struct ares_options));
    opts->flags = flags;
    return opts;
  }
 private:
  struct ares_options opts_;
};

class MockNoCheckRespChannelTestAI : public MockFlagsChannelOptsTestAI {
 public:
  MockNoCheckRespChannelTestAI() : MockFlagsChannelOptsTestAI(ARES_FLAG_NOCHECKRESP) {}
};

TEST_P(MockNoCheckRespChannelTestAI, ServFailResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(SERVFAIL);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ESERVFAIL, result.status_);
}

TEST_P(MockNoCheckRespChannelTestAI, NotImplResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(NOTIMP);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENOTIMP, result.status_);
}

TEST_P(MockNoCheckRespChannelTestAI, RefusedResponse) {
  DNSPacket rsp;
  rsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.google.com", T_A));
  rsp.set_rcode(REFUSED);
  ON_CALL(server_, OnRequest("www.google.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EREFUSED, result.status_);
}

TEST_P(MockChannelTestAI, FamilyV6) {
  DNSPacket rsp6;
  rsp6.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_AAAA))
    .add_answer(new DNSAaaaRR("example.com", 100,
                              {0x21, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03}));
  ON_CALL(server_, OnRequest("example.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &rsp6));
  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET6;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "example.com.", NULL, &hints,
                   AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV6Address("2121:0000:0000:0000:0000:0000:0000:0303"));
}

TEST_P(MockChannelTestAI, FamilyV4) {
  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));
  AddrInfoResult result = {};
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "example.com.", NULL, &hints,
                   AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV4Address("2.3.4.5"));
}

TEST_P(MockChannelTestAI, FamilyV4_MultipleAddresses) {
  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {2, 3, 4, 5}))
    .add_answer(new DNSARR("example.com", 100, {7, 8, 9, 0}));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));
  AddrInfoResult result = {};
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "example.com.", NULL, &hints,
                   AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.ai_;
  EXPECT_EQ("{addr=[2.3.4.5], addr=[7.8.9.0]}", ss.str());
}

TEST_P(MockChannelTestAI, FamilyUnspecified) {
  DNSPacket rsp6;
  rsp6.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_AAAA))
    .add_answer(new DNSAaaaRR("example.com", 100,
                              {0x21, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03}));
  ON_CALL(server_, OnRequest("example.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &rsp6));
  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));
  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "example.com.", NULL, &hints,
                   AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(2));
  EXPECT_THAT(result.ai_, IncludesV4Address("2.3.4.5"));
  EXPECT_THAT(result.ai_, IncludesV6Address("2121:0000:0000:0000:0000:0000:0000:0303"));
}

class MockEDNSChannelTestAI : public MockFlagsChannelOptsTestAI {
 public:
  MockEDNSChannelTestAI() : MockFlagsChannelOptsTestAI(ARES_FLAG_EDNS) {}
};

TEST_P(MockEDNSChannelTestAI, RetryWithoutEDNS) {
  DNSPacket rspfail;
  rspfail.set_response().set_aa().set_rcode(FORMERR)
    .add_question(new DNSQuestion("www.google.com", T_A));
  DNSPacket rspok;
  rspok.set_response()
    .add_question(new DNSQuestion("www.google.com", T_A))
    .add_answer(new DNSARR("www.google.com", 100, {1, 2, 3, 4}));
  EXPECT_CALL(server_, OnRequest("www.google.com", T_A))
    .WillOnce(SetReply(&server_, &rspfail))
    .WillOnce(SetReply(&server_, &rspok));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www.google.com.", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV4Address("1.2.3.4"));
}

TEST_P(MockChannelTestAI, SearchDomains) {
  DNSPacket nofirst;
  nofirst.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.first.com", T_A));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &nofirst));
  DNSPacket nosecond;
  nosecond.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.second.org", T_A));
  ON_CALL(server_, OnRequest("www.second.org", T_A))
    .WillByDefault(SetReply(&server_, &nosecond));
  DNSPacket yesthird;
  yesthird.set_response().set_aa()
    .add_question(new DNSQuestion("www.third.gov", T_A))
    .add_answer(new DNSARR("www.third.gov", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillByDefault(SetReply(&server_, &yesthird));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV4Address("2.3.4.5"));
}

TEST_P(MockChannelTestAI, SearchDomainsServFailOnAAAA) {
  DNSPacket nofirst;
  nofirst.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.first.com", T_AAAA));
  ON_CALL(server_, OnRequest("www.first.com", T_AAAA))
    .WillByDefault(SetReply(&server_, &nofirst));
  DNSPacket nofirst4;
  nofirst4.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.first.com", T_A));
  ON_CALL(server_, OnRequest("www.first.com", T_A))
    .WillByDefault(SetReply(&server_, &nofirst4));

  DNSPacket nosecond;
  nosecond.set_response().set_aa().set_rcode(NXDOMAIN)
    .add_question(new DNSQuestion("www.second.org", T_AAAA));
  ON_CALL(server_, OnRequest("www.second.org", T_AAAA))
    .WillByDefault(SetReply(&server_, &nosecond));
  DNSPacket yessecond4;
  yessecond4.set_response().set_aa()
    .add_question(new DNSQuestion("www.second.org", T_A))
    .add_answer(new DNSARR("www.second.org", 0x0200, {2, 3, 4, 5}));
  ON_CALL(server_, OnRequest("www.second.org", T_A))
    .WillByDefault(SetReply(&server_, &yessecond4));

  DNSPacket failthird;
  failthird.set_response().set_aa().set_rcode(SERVFAIL)
    .add_question(new DNSQuestion("www.third.gov", T_AAAA));
  ON_CALL(server_, OnRequest("www.third.gov", T_AAAA))
    .WillByDefault(SetReply(&server_, &failthird));
  DNSPacket failthird4;
  failthird4.set_response().set_aa().set_rcode(SERVFAIL)
    .add_question(new DNSQuestion("www.third.gov", T_A));
  ON_CALL(server_, OnRequest("www.third.gov", T_A))
    .WillByDefault(SetReply(&server_, &failthird4));

  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "www", NULL, &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
  EXPECT_THAT(result.ai_, IncludesV4Address("2.3.4.5"));
}

class MockMultiServerChannelTestAI
  : public MockChannelOptsTest,
    public ::testing::WithParamInterface< std::pair<int, bool> > {
 public:
  MockMultiServerChannelTestAI(bool rotate)
    : MockChannelOptsTest(3, GetParam().first, GetParam().second, nullptr, rotate ? ARES_OPT_ROTATE : ARES_OPT_NOROTATE) {}
  void CheckExample() {
    AddrInfoResult result;
    struct ares_addrinfo_hints hints = {};
    hints.ai_family = AF_INET;
    hints.ai_flags = ARES_AI_NOSORT;
    ares_getaddrinfo(channel_, "www.example.com.", NULL, &hints, AddrInfoCallback, &result);
    Process();
    EXPECT_TRUE(result.done_);
    EXPECT_EQ(result.status_, ARES_SUCCESS);
    EXPECT_THAT(result.ai_, IncludesNumAddresses(1));
    EXPECT_THAT(result.ai_, IncludesV4Address("2.3.4.5"));
  }
};

class RotateMultiMockTestAI : public MockMultiServerChannelTestAI {
 public:
  RotateMultiMockTestAI() : MockMultiServerChannelTestAI(true) {}
};

class NoRotateMultiMockTestAI : public MockMultiServerChannelTestAI {
 public:
  NoRotateMultiMockTestAI() : MockMultiServerChannelTestAI(false) {}
};


TEST_P(RotateMultiMockTestAI, ThirdServer) {
  struct ares_options opts = {0};
  int optmask = 0;
  EXPECT_EQ(ARES_SUCCESS, ares_save_options(channel_, &opts, &optmask));
  EXPECT_EQ(0, (optmask & ARES_OPT_NOROTATE));
  ares_destroy_options(&opts);

  DNSPacket servfailrsp;
  servfailrsp.set_response().set_aa().set_rcode(SERVFAIL)
    .add_question(new DNSQuestion("www.example.com", T_A));
  DNSPacket notimplrsp;
  notimplrsp.set_response().set_aa().set_rcode(NOTIMP)
    .add_question(new DNSQuestion("www.example.com", T_A));
  DNSPacket okrsp;
  okrsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.example.com", T_A))
    .add_answer(new DNSARR("www.example.com", 100, {2,3,4,5}));

  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &notimplrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  CheckExample();

  // Second time around, starts from server [1].
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &servfailrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &notimplrsp));
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &okrsp));
  CheckExample();

  // Third time around, starts from server [2].
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &servfailrsp));
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &notimplrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &okrsp));
  CheckExample();
}

TEST_P(NoRotateMultiMockTestAI, ThirdServer) {
  struct ares_options opts = {0};
  int optmask = 0;
  EXPECT_EQ(ARES_SUCCESS, ares_save_options(channel_, &opts, &optmask));
  EXPECT_EQ(ARES_OPT_NOROTATE, (optmask & ARES_OPT_NOROTATE));
  ares_destroy_options(&opts);

  DNSPacket servfailrsp;
  servfailrsp.set_response().set_aa().set_rcode(SERVFAIL)
    .add_question(new DNSQuestion("www.example.com", T_A));
  DNSPacket notimplrsp;
  notimplrsp.set_response().set_aa().set_rcode(NOTIMP)
    .add_question(new DNSQuestion("www.example.com", T_A));
  DNSPacket okrsp;
  okrsp.set_response().set_aa()
    .add_question(new DNSQuestion("www.example.com", T_A))
    .add_answer(new DNSARR("www.example.com", 100, {2,3,4,5}));

  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &notimplrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  CheckExample();

  // Second time around, still starts from server [0].
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &notimplrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  CheckExample();

  // Third time around, still starts from server [0].
  EXPECT_CALL(*servers_[0], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[0].get(), &servfailrsp));
  EXPECT_CALL(*servers_[1], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[1].get(), &notimplrsp));
  EXPECT_CALL(*servers_[2], OnRequest("www.example.com", T_A))
    .WillOnce(SetReply(servers_[2].get(), &okrsp));
  CheckExample();
}

TEST_P(MockChannelTestAI, FamilyV4ServiceName) {
  DNSPacket rsp4;
  rsp4.set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {1, 1, 1, 1}))
    .add_answer(new DNSARR("example.com", 100, {2, 2, 2, 2}));
  ON_CALL(server_, OnRequest("example.com", T_A))
    .WillByDefault(SetReply(&server_, &rsp4));
  AddrInfoResult result = {};
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_INET;
  hints.ai_flags = ARES_AI_NOSORT;
  ares_getaddrinfo(channel_, "example.com", "http", &hints, AddrInfoCallback, &result);
  Process();
  EXPECT_TRUE(result.done_);
  std::stringstream ss;
  ss << result.ai_;
  EXPECT_EQ("{addr=[1.1.1.1:80], addr=[2.2.2.2:80]}", ss.str());
}

// force-tcp does currently not work, possibly test DNS server swallows
// bytes from second query
//INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockChannelTestAI,
//                       ::testing::ValuesIn(ares::test::families_modes));
//const std::vector<std::pair<int, bool>> both_families_udponly = {
//  std::make_pair<int, bool>(AF_INET, false),
//  std::make_pair<int, bool>(AF_INET6, false)
//};
INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockChannelTestAI,
			::testing::Values(std::make_pair<int, bool>(AF_INET, false)));

INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockUDPChannelTestAI,
                        ::testing::ValuesIn(ares::test::families));

INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockTCPChannelTestAI,
                        ::testing::ValuesIn(ares::test::families));

INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockExtraOptsTestAI,
			::testing::ValuesIn(ares::test::families_modes));

INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockExtraOptsNDots5TestAI,
      ::testing::ValuesIn(ares::test::families_modes));

INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockNoCheckRespChannelTestAI,
			::testing::ValuesIn(ares::test::families_modes));

INSTANTIATE_TEST_CASE_P(AddressFamiliesAI, MockEDNSChannelTestAI,
			::testing::ValuesIn(ares::test::families_modes));

INSTANTIATE_TEST_CASE_P(TransportModesAI, RotateMultiMockTestAI,
			::testing::ValuesIn(ares::test::families_modes));

INSTANTIATE_TEST_CASE_P(TransportModesAI, NoRotateMultiMockTestAI,
			::testing::ValuesIn(ares::test::families_modes));


}  // namespace test
}  // namespace ares
