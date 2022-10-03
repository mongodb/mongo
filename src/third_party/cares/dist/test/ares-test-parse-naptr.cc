#include "ares-test.h"
#include "dns-proto.h"

#include <sstream>
#include <vector>

namespace ares {
namespace test {

TEST_F(LibraryTest, ParseNaptrReplyOK) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_NAPTR))
    .add_answer(new DNSNaptrRR("example.com", 100,
                               10, 20, "SP", "service", "regexp", "replace"))
    .add_answer(new DNSNaptrRR("example.com", 0x0010,
                               11, 21, "SP", "service2", "regexp2", "replace2"));
  std::vector<byte> data = pkt.data();

  struct ares_naptr_reply* naptr = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
  ASSERT_NE(nullptr, naptr);
  EXPECT_EQ("SP", std::string((char*)naptr->flags));
  EXPECT_EQ("service", std::string((char*)naptr->service));
  EXPECT_EQ("regexp", std::string((char*)naptr->regexp));
  EXPECT_EQ("replace", std::string((char*)naptr->replacement));
  EXPECT_EQ(10, naptr->order);
  EXPECT_EQ(20, naptr->preference);

  struct ares_naptr_reply* naptr2 = naptr->next;
  ASSERT_NE(nullptr, naptr2);
  EXPECT_EQ("SP", std::string((char*)naptr2->flags));
  EXPECT_EQ("service2", std::string((char*)naptr2->service));
  EXPECT_EQ("regexp2", std::string((char*)naptr2->regexp));
  EXPECT_EQ("replace2", std::string((char*)naptr2->replacement));
  EXPECT_EQ(11, naptr2->order);
  EXPECT_EQ(21, naptr2->preference);
  EXPECT_EQ(nullptr, naptr2->next);

  ares_free_data(naptr);
}

TEST_F(LibraryTest, ParseNaptrReplyErrors) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_NAPTR))
    .add_answer(new DNSNaptrRR("example.com", 100,
                               10, 20, "SP", "service", "regexp", "replace"));
  std::vector<byte> data;
  struct ares_naptr_reply* naptr = nullptr;

  // No question.
  pkt.questions_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
  pkt.add_question(new DNSQuestion("example.com", T_NAPTR));

#ifdef DISABLED
  // Question != answer
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("Axample.com", T_NAPTR));
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("example.com", T_NAPTR));
#endif

  // Two questions
  pkt.add_question(new DNSQuestion("example.com", T_NAPTR));
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("example.com", T_NAPTR));

  // Wrong sort of answer.
  pkt.answers_.clear();
  pkt.add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"));
  data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
  EXPECT_EQ(nullptr, naptr);
  pkt.answers_.clear();
  pkt.add_answer(new DNSNaptrRR("example.com", 100,
                               10, 20, "SP", "service", "regexp", "replace"));

  // No answer.
  pkt.answers_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
  pkt.add_answer(new DNSNaptrRR("example.com", 100,
                               10, 20, "SP", "service", "regexp", "replace"));

  // Truncated packets.
  data = pkt.data();
  for (size_t len = 1; len < data.size(); len++) {
    int rc = ares_parse_naptr_reply(data.data(), len, &naptr);
    EXPECT_TRUE(rc == ARES_EBADRESP || rc == ARES_EBADNAME);
  }
}

TEST_F(LibraryTest, ParseNaptrReplyTooShort) {
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
    0x00, 0x23,  // type NAPTR
    0x00, 0x01,  // class IN
    // Answer 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x23,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x01,  // rdata length
    0x00,  // Too short: expect 2 x int16 and 3 x name (min 1 byte each)
  };
  struct ares_naptr_reply* naptr = nullptr;
  EXPECT_EQ(ARES_EBADRESP, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
}

TEST_F(LibraryTest, ParseNaptrReplyAllocFail) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_NAPTR))
    .add_answer(new DNSNaptrRR("example.com", 100,
                               10, 20, "SP", "service", "regexp", "replace"))
    .add_answer(new DNSNaptrRR("example.com", 0x0010,
                               11, 21, "SP", "service2", "regexp2", "replace2"));
  std::vector<byte> data = pkt.data();
  struct ares_naptr_reply* naptr = nullptr;

  for (int ii = 1; ii <= 13; ii++) {
    ClearFails();
    SetAllocFail(ii);
    EXPECT_EQ(ARES_ENOMEM, ares_parse_naptr_reply(data.data(), data.size(), &naptr));
  }
}

}  // namespace test
}  // namespace ares
