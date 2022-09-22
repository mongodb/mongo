#include "ares-test.h"
#include "dns-proto.h"

#include <vector>

namespace ares {
namespace test {

TEST(DNSProto, EncodeQuestions) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com.", T_A))
    .add_question(new DNSQuestion("www.example.com", T_AAAA, C_CHAOS));

  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x02,  // num questions
    0x00, 0x00,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Question 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // type A
    0x00, 0x01,  // class IN
    // Question 2
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x1C,  // type AAAA = 28
    0x00, 0x03,  // class CHAOS = 3
  };
  EXPECT_EQ(data, pkt.data());
}

TEST(DNSProto, EncodeSingleNameAnswers) {
  DNSPacket pkt;
  pkt.qid_ = 0x1234;
  pkt.response_ = true;
  pkt.aa_ = true;
  pkt.opcode_ = O_QUERY;
  pkt.add_answer(new DNSCnameRR("example.com", 0x01020304, "other.com."));
  pkt.add_auth(new DNSPtrRR("www.example.com", 0x01020304, "www.other.com"));

  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x00,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x01,  // num authority RRs
    0x00, 0x00,  // num additional RRs
    // Answer 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x05,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x0B,  // rdata length
    0x05, 'o', 't', 'h', 'e', 'r',
    0x03, 'c', 'o', 'm',
    0x00,
    // Authority 1
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x0c,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x0F,  // rdata length
    0x03, 'w', 'w', 'w',
    0x05, 'o', 't', 'h', 'e', 'r',
    0x03, 'c', 'o', 'm',
    0x00,
  };
  EXPECT_EQ(data, pkt.data());
}

TEST(DNSProto, EncodeAddressAnswers) {
  DNSPacket pkt;
  pkt.qid_ = 0x1234;
  pkt.response_ = true;
  pkt.aa_ = true;
  pkt.opcode_ = O_QUERY;
  std::vector<byte> addrv4 = {0x02, 0x03, 0x04, 0x05};
  pkt.add_answer(new DNSARR("example.com", 0x01020304, addrv4));
  byte addrv6[16] = {0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
                     0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04};
  pkt.add_additional(new DNSAaaaRR("www.example.com", 0x01020304, addrv6, 16));

  std::vector<byte> data = {
    0x12, 0x34,  // qid
    0x84, // response + query + AA + not-TC + not-RD
    0x00, // not-RA + not-Z + not-AD + not-CD + rc=NoError
    0x00, 0x00,  // num questions
    0x00, 0x01,  // num answer RRs
    0x00, 0x00,  // num authority RRs
    0x00, 0x01,  // num additional RRs
    // Answer 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x01,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length
    0x02, 0x03, 0x04, 0x05,
    // Additional 1
    0x03, 'w', 'w', 'w',
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x1c,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x10,  // rdata length
    0x01, 0x01, 0x01, 0x01, 0x02, 0x02, 0x02, 0x02,
    0x03, 0x03, 0x03, 0x03, 0x04, 0x04, 0x04, 0x04
  };
  EXPECT_EQ(data, pkt.data());
}


}  // namespace test
}  // namespace ares
