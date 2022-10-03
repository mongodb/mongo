#include "ares-test.h"
#include "dns-proto.h"

#include <sstream>
#include <vector>

namespace ares {
namespace test {

TEST_F(LibraryTest, ParseAReplyOK) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 0x01020304, {2,3,4,5}))
    .add_answer(new DNSAaaaRR("example.com", 0x01020304, {0,0,0,0,0,0,0,0,0,0,0,0,2,3,4,5}));
  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // num questions
    0x00, 0x02,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // type A
    0x00, 0x01,  // class IN
    // Answer 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length
    0x02, 0x03, 0x04, 0x05,
    // Answer 2
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x1c,  //  RR type
    0x00, 0x01,  //  class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x10,  // rdata length
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x03, 0x04, 0x05,
  };
  EXPECT_EQ(data, pkt.data());
  struct hostent *host = nullptr;
  struct ares_addrttl info[5];
  int count = 5;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(1, count);
  EXPECT_EQ(0x01020304, info[0].ttl);
  unsigned long expected_addr = htonl(0x02030405);
  EXPECT_EQ(expected_addr, info[0].ipaddr.s_addr);
  EXPECT_EQ("2.3.4.5", AddressToString(&(info[0].ipaddr), 4));
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'example.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  ares_free_hostent(host);

  // Repeat without providing a hostent
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             nullptr, info, &count));
  EXPECT_EQ(1, count);
  EXPECT_EQ(0x01020304, info[0].ttl);
  EXPECT_EQ(expected_addr, info[0].ipaddr.s_addr);
  EXPECT_EQ("2.3.4.5", AddressToString(&(info[0].ipaddr), 4));
}

TEST_F(LibraryTest, ParseMalformedAReply) {
  std::vector<byte> data = {
    0x12, 0x34,  // [0:2) qid
    0x84, // [2] response + query + AA + not-TC + not-RD
    0x00, // [3] not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // [4:6) num questions
    0x00, 0x02,  // [6:8) num answer RRs
    0x00, 0x00,  // [8:10) num authority RRs
    0x00, 0x00,  // [10:12) num additional RRs
    // Question
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', // [12:20)
    0x03, 'c', 'o', 'm', // [20,24)
    0x00, // [24]
    0x00, 0x01,  // [25:26) type A
    0x00, 0x01,  // [27:29) class IN
    // Answer 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', // [29:37)
    0x03, 'c', 'o', 'm', // [37:41)
    0x00, // [41]
    0x00, 0x01,  // [42:44) RR type
    0x00, 0x01,  // [44:46) class IN
    0x01, 0x02, 0x03, 0x04, // [46:50) TTL
    0x00, 0x04,  // [50:52) rdata length
    0x02, 0x03, 0x04, 0x05, // [52,56)
  };
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;

  // Invalid RR-len.
  std::vector<byte> invalid_rrlen(data);
  invalid_rrlen[51] = 180;
  EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(invalid_rrlen.data(), invalid_rrlen.size(),
                                              &host, info, &count));

  // Truncate mid-question.
  EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(data.data(), 26,
                                              &host, info, &count));

  // Truncate mid-answer.
  EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(data.data(), 42,
                                              &host, info, &count));
}

TEST_F(LibraryTest, ParseAReplyNoData) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A));
  std::vector<byte> data = pkt.data();
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  EXPECT_EQ(ARES_ENODATA, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(0, count);
  EXPECT_EQ(nullptr, host);

  // Again but with a CNAME.
  pkt.add_answer(new DNSCnameRR("example.com", 200, "c.example.com"));
  data = pkt.data();
  // Expect success as per https://github.com/c-ares/c-ares/commit/2c63440127feed70ccefb148b8f938a2df6c15f8
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(0, count);
  EXPECT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'c.example.com' aliases=[example.com] addrs=[]}", ss.str());
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParseAReplyVariantA) {
  DNSPacket pkt;
  pkt.set_qid(6366).set_rd().set_ra()
    .add_question(new DNSQuestion("mit.edu", T_A))
    .add_answer(new DNSARR("mit.edu", 52, {18,7,22,69}))
    .add_auth(new DNSNsRR("mit.edu", 292, "W20NS.mit.edu"))
    .add_auth(new DNSNsRR("mit.edu", 292, "BITSY.mit.edu"))
    .add_auth(new DNSNsRR("mit.edu", 292, "STRAWB.mit.edu"))
    .add_additional(new DNSARR("STRAWB.mit.edu", 292, {18,71,0,151}));
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  std::vector<byte> data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(1, count);
  EXPECT_EQ("18.7.22.69", AddressToString(&(info[0].ipaddr), 4));
  EXPECT_EQ(52, info[0].ttl);
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParseAReplyJustCname) {
  DNSPacket pkt;
  pkt.set_qid(6366).set_rd().set_ra()
    .add_question(new DNSQuestion("mit.edu", T_A))
    .add_answer(new DNSCnameRR("mit.edu", 52, "other.mit.edu"));
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  std::vector<byte> data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(0, count);
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'other.mit.edu' aliases=[mit.edu] addrs=[]}", ss.str());
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParseAReplyVariantCname) {
  DNSPacket pkt;
  pkt.set_qid(6366).set_rd().set_ra()
    .add_question(new DNSQuestion("query.example.com", T_A))
    .add_answer(new DNSCnameRR("query.example.com", 200, "redirect.query.example.com"))
    .add_answer(new DNSARR("redirect.query.example.com", 300, {129,97,123,22}))
    .add_auth(new DNSNsRR("example.com", 218, "aa.ns1.example.com"))
    .add_auth(new DNSNsRR("example.com", 218, "ns2.example.com"))
    .add_auth(new DNSNsRR("example.com", 218, "ns3.example.com"))
    .add_auth(new DNSNsRR("example.com", 218, "ns4.example.com"))
    .add_additional(new DNSARR("aa.ns1.example.com", 218, {129,97,1,1}))
    .add_additional(new DNSARR("ns2.example.com", 218, {129,97,1,2}))
    .add_additional(new DNSARR("ns3.example.com", 218, {129,97,1,3}))
    .add_additional(new DNSARR("ns4.example.com", 218, {129,97,1,4}));
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  std::vector<byte> data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(1, count);
  EXPECT_EQ("129.97.123.22", AddressToString(&(info[0].ipaddr), 4));
  // TTL is reduced to match CNAME's.
  EXPECT_EQ(200, info[0].ttl);
  ares_free_hostent(host);

  // Repeat parsing without places to put the results.
  count = 0;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             nullptr, info, &count));
}

TEST_F(LibraryTest, ParseAReplyVariantCnameChain) {
  DNSPacket pkt;
  pkt.set_qid(6366).set_rd().set_ra()
    .add_question(new DNSQuestion("c1.localhost", T_A))
    .add_answer(new DNSCnameRR("c1.localhost", 604800, "c2.localhost"))
    .add_answer(new DNSCnameRR("c2.localhost", 604800, "c3.localhost"))
    .add_answer(new DNSCnameRR("c3.localhost", 604800, "c4.localhost"))
    .add_answer(new DNSARR("c4.localhost", 604800, {8,8,8,8}))
    .add_auth(new DNSNsRR("localhost", 604800, "localhost"))
    .add_additional(new DNSARR("localhost", 604800, {127,0,0,1}))
    .add_additional(new DNSAaaaRR("localhost", 604800,
                              {0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                               0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01}));
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  std::vector<byte> data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(1, count);
  EXPECT_EQ("8.8.8.8", AddressToString(&(info[0].ipaddr), 4));
  EXPECT_EQ(604800, info[0].ttl);
  ares_free_hostent(host);
}

TEST_F(LibraryTest, DISABLED_ParseAReplyVariantCnameLast) {
  DNSPacket pkt;
  pkt.set_qid(6366).set_rd().set_ra()
    .add_question(new DNSQuestion("query.example.com", T_A))
    .add_answer(new DNSARR("redirect.query.example.com", 300, {129,97,123,221}))
    .add_answer(new DNSARR("redirect.query.example.com", 300, {129,97,123,222}))
    .add_answer(new DNSARR("redirect.query.example.com", 300, {129,97,123,223}))
    .add_answer(new DNSARR("redirect.query.example.com", 300, {129,97,123,224}))
    .add_answer(new DNSCnameRR("query.example.com", 60, "redirect.query.example.com"))
    .add_additional(new DNSTxtRR("query.example.com", 60, {"text record"}));
  struct hostent *host = nullptr;
  struct ares_addrttl info[8];
  int count = 8;
  std::vector<byte> data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(4, count);
  EXPECT_EQ("129.97.123.221", AddressToString(&(info[0].ipaddr), 4));
  EXPECT_EQ("129.97.123.222", AddressToString(&(info[1].ipaddr), 4));
  EXPECT_EQ("129.97.123.223", AddressToString(&(info[2].ipaddr), 4));
  EXPECT_EQ("129.97.123.224", AddressToString(&(info[3].ipaddr), 4));
  EXPECT_EQ(300, info[0].ttl);
  EXPECT_EQ(300, info[1].ttl);
  EXPECT_EQ(300, info[2].ttl);
  EXPECT_EQ(300, info[3].ttl);
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParseAReplyErrors) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSARR("example.com", 100, {0x02, 0x03, 0x04, 0x05}));
  std::vector<byte> data;

  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;

  // No question.
  pkt.questions_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(data.data(), data.size(),
                                              &host, info, &count));
  EXPECT_EQ(nullptr, host);
  pkt.add_question(new DNSQuestion("example.com", T_A));

  // Question != answer
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("Axample.com", T_A));
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_a_reply(data.data(), data.size(),
                                              &host, info, &count));
  EXPECT_EQ(nullptr, host);
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("example.com", T_A));

#ifdef DISABLED
  // Not a response.
  pkt.set_response(false);
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(data.data(), data.size(),
                                              &host, info, &count));
  EXPECT_EQ(nullptr, host);
  pkt.set_response(true);

  // Bad return code.
  pkt.set_rcode(FORMERR);
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_a_reply(data.data(), data.size(),
                                              &host, info, &count));
  EXPECT_EQ(nullptr, host);
  pkt.set_rcode(NOERROR);
#endif

  // Two questions
  pkt.add_question(new DNSQuestion("example.com", T_A));
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(data.data(), data.size(),
                                              &host, info, &count));
  EXPECT_EQ(nullptr, host);
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("example.com", T_A));

  // Wrong sort of answer.
  pkt.answers_.clear();
  pkt.add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"));
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(nullptr, host);
  pkt.answers_.clear();
  pkt.add_answer(new DNSARR("example.com", 100, {0x02, 0x03, 0x04, 0x05}));

  // No answer.
  pkt.answers_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(nullptr, host);
  pkt.add_answer(new DNSARR("example.com", 100, {0x02, 0x03, 0x04, 0x05}));

  // Truncated packets.
  data = pkt.data();
  for (size_t len = 1; len < data.size(); len++) {
    EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(data.data(), len,
                                                &host, info, &count));
    EXPECT_EQ(nullptr, host);
    EXPECT_EQ(ARES_EBADRESP, ares_parse_a_reply(data.data(), len,
                                                nullptr, info, &count));
  }
}

TEST_F(LibraryTest, ParseAReplyAllocFail) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_A))
    .add_answer(new DNSCnameRR("example.com", 300, "c.example.com"))
    .add_answer(new DNSARR("c.example.com", 500, {0x02, 0x03, 0x04, 0x05}));
  std::vector<byte> data = pkt.data();

  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;

  for (int ii = 1; ii <= 8; ii++) {
    ClearFails();
    SetAllocFail(ii);
    EXPECT_EQ(ARES_ENOMEM, ares_parse_a_reply(data.data(), data.size(),
                                              &host, info, &count)) << ii;
    EXPECT_EQ(nullptr, host);
  }
}

}  // namespace test
}  // namespace ares
