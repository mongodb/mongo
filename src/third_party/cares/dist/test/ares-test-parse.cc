#include "ares-test.h"
#include "dns-proto.h"

#include <sstream>
#include <vector>

namespace ares {
namespace test {

TEST_F(LibraryTest, ParseRootName) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion(".", T_A))
    .add_answer(new DNSARR(".", 100, {0x02, 0x03, 0x04, 0x05}));
  std::vector<byte> data = pkt.data();

  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(1, count);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'' aliases=[] addrs=[2.3.4.5]}", ss.str());
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParseIndirectRootName) {
  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question
    0xC0, 0x04,  // weird: pointer to a random zero earlier in the message
    0x00, 0x01,  // type A
    0x00, 0x01,  // class IN
    // Answer 1
    0xC0, 0x04,
    0x00, 0x01,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length
    0x02, 0x03, 0x04, 0x05,
  };

  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(1, count);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'' aliases=[] addrs=[2.3.4.5]}", ss.str());
  ares_free_hostent(host);
}


#if 0 /* We are validating hostnames now, its not clear how this would ever be valid */
TEST_F(LibraryTest, ParseEscapedName) {
  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question
    0x05, 'a', '\\', 'b', '.', 'c',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // type A
    0x00, 0x01,  // class IN
    // Answer 1
    0x05, 'a', '\\', 'b', '.', 'c',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length
    0x02, 0x03, 0x04, 0x05,
  };
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  EXPECT_EQ(1, count);
  HostEnt hent(host);
  std::stringstream ss;
  ss << hent;
  // The printable name is expanded with escapes.
  EXPECT_EQ(11, hent.name_.size());
  EXPECT_EQ('a', hent.name_[0]);
  EXPECT_EQ('\\', hent.name_[1]);
  EXPECT_EQ('\\', hent.name_[2]);
  EXPECT_EQ('b', hent.name_[3]);
  EXPECT_EQ('\\', hent.name_[4]);
  EXPECT_EQ('.', hent.name_[5]);
  EXPECT_EQ('c', hent.name_[6]);
  ares_free_hostent(host);
}
#endif

TEST_F(LibraryTest, ParsePartialCompressedName) {
  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // type A
    0x00, 0x01,  // class IN
    // Answer 1
    0x03, 'w', 'w', 'w',
    0xc0, 0x10,  // offset 16
    0x00, 0x01,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length
    0x02, 0x03, 0x04, 0x05,
  };
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParseFullyCompressedName) {
  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // type A
    0x00, 0x01,  // class IN
    // Answer 1
    0xc0, 0x0c,  // offset 12
    0x00, 0x01,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length
    0x02, 0x03, 0x04, 0x05,
  };
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParseFullyCompressedName2) {
  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question
    0xC0, 0x12,  // pointer to later in message
    0x00, 0x01,  // type A
    0x00, 0x01,  // class IN
    // Answer 1
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length
    0x02, 0x03, 0x04, 0x05,
  };
  struct hostent *host = nullptr;
  struct ares_addrttl info[2];
  int count = 2;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_a_reply(data.data(), data.size(),
                                             &host, info, &count));
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'www.example.com' aliases=[] addrs=[2.3.4.5]}", ss.str());
  ares_free_hostent(host);
}

}  // namespace test
}  // namespace ares
