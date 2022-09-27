#include "ares-test.h"
#include "dns-proto.h"

#include <sstream>
#include <vector>

namespace ares {
namespace test {

TEST_F(LibraryTest, ParseMxReplyOK) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_MX))
    .add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"))
    .add_answer(new DNSMxRR("example.com", 100, 200, "mx2.example.com"));
  std::vector<byte> data = pkt.data();

  struct ares_mx_reply* mx = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_mx_reply(data.data(), data.size(), &mx));
  ASSERT_NE(nullptr, mx);
  EXPECT_EQ("mx1.example.com", std::string(mx->host));
  EXPECT_EQ(100, mx->priority);

  struct ares_mx_reply* mx2 = mx->next;
  ASSERT_NE(nullptr, mx2);
  EXPECT_EQ("mx2.example.com", std::string(mx2->host));
  EXPECT_EQ(200, mx2->priority);
  EXPECT_EQ(nullptr, mx2->next);

  ares_free_data(mx);
}

TEST_F(LibraryTest, ParseMxReplyMalformed) {
  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x01,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x0F,  // type MX
    0x00, 0x01,  // class IN
    // Answer 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x0F,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x01,  // rdata length -- too short
    0x02,
  };

  struct ares_mx_reply* mx = nullptr;
  EXPECT_EQ(ARES_EBADRESP, ares_parse_mx_reply(data.data(), data.size(), &mx));
  ASSERT_EQ(nullptr, mx);
}


TEST_F(LibraryTest, ParseMxReplyErrors) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_MX))
    .add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"));
  std::vector<byte> data;
  struct ares_mx_reply* mx = nullptr;

  // No question.
  pkt.questions_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_mx_reply(data.data(), data.size(), &mx));
  EXPECT_EQ(nullptr, mx);
  pkt.add_question(new DNSQuestion("example.com", T_MX));

#ifdef DISABLED
  // Question != answer
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("Axample.com", T_MX));
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_mx_reply(data.data(), data.size(), &mx));
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("example.com", T_MX));
#endif

  // Two questions.
  pkt.add_question(new DNSQuestion("example.com", T_MX));
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_mx_reply(data.data(), data.size(), &mx));
  EXPECT_EQ(nullptr, mx);
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("example.com", T_MX));

  // Wrong sort of answer.
  // TODO(drysdale): check if this should be ARES_ENODATA?
  pkt.answers_.clear();
  pkt.add_answer(new DNSSrvRR("example.abc.def.com", 180, 0, 10, 8160, "example.abc.def.com"));
  data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_mx_reply(data.data(), data.size(), &mx));
  EXPECT_EQ(nullptr, mx);
  pkt.answers_.clear();
  pkt.add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"));

  // No answer.
  pkt.answers_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_mx_reply(data.data(), data.size(), &mx));
  EXPECT_EQ(nullptr, mx);
  pkt.add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"));

  // Truncated packets.
  data = pkt.data();
  for (size_t len = 1; len < data.size(); len++) {
    int rc = ares_parse_mx_reply(data.data(), len, &mx);
    EXPECT_EQ(nullptr, mx);
    EXPECT_TRUE(rc == ARES_EBADRESP || rc == ARES_EBADNAME);
  }
}

TEST_F(LibraryTest, ParseMxReplyAllocFail) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_MX))
    .add_answer(new DNSCnameRR("example.com", 300, "c.example.com"))
    .add_answer(new DNSMxRR("c.example.com", 100, 100, "mx1.example.com"));
  std::vector<byte> data = pkt.data();
  struct ares_mx_reply* mx = nullptr;

  for (int ii = 1; ii <= 5; ii++) {
    ClearFails();
    SetAllocFail(ii);
    EXPECT_EQ(ARES_ENOMEM, ares_parse_mx_reply(data.data(), data.size(), &mx)) << ii;
  }
}

}  // namespace test
}  // namespace ares
