#include "ares-test.h"
#include "dns-proto.h"

#include <sstream>
#include <vector>

namespace ares {
namespace test {

TEST_F(LibraryTest, ParseSrvReplyOK) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.com", T_SRV))
    .add_answer(new DNSSrvRR("example.com", 100, 10, 20, 30, "srv.example.com"))
    .add_answer(new DNSSrvRR("example.com", 100, 11, 21, 31, "srv2.example.com"));
  std::vector<byte> data = pkt.data();

  struct ares_srv_reply* srv = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_srv_reply(data.data(), data.size(), &srv));
  ASSERT_NE(nullptr, srv);

  EXPECT_EQ("srv.example.com", std::string(srv->host));
  EXPECT_EQ(10, srv->priority);
  EXPECT_EQ(20, srv->weight);
  EXPECT_EQ(30, srv->port);

  struct ares_srv_reply* srv2 = srv->next;
  ASSERT_NE(nullptr, srv2);
  EXPECT_EQ("srv2.example.com", std::string(srv2->host));
  EXPECT_EQ(11, srv2->priority);
  EXPECT_EQ(21, srv2->weight);
  EXPECT_EQ(31, srv2->port);
  EXPECT_EQ(nullptr, srv2->next);

  ares_free_data(srv);
}

TEST_F(LibraryTest, ParseSrvReplySingle) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.abc.def.com", T_SRV))
    .add_answer(new DNSSrvRR("example.abc.def.com", 180, 0, 10, 8160, "example.abc.def.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else1.where.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else2.where.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else3.where.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else4.where.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else5.where.com"))
    .add_additional(new DNSARR("else2.where.com", 42, {172,19,0,1}))
    .add_additional(new DNSARR("else5.where.com", 42, {172,19,0,2}));
  std::vector<byte> data = pkt.data();

  struct ares_srv_reply* srv = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_srv_reply(data.data(), data.size(), &srv));
  ASSERT_NE(nullptr, srv);

  EXPECT_EQ("example.abc.def.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(10, srv->weight);
  EXPECT_EQ(8160, srv->port);
  EXPECT_EQ(nullptr, srv->next);

  ares_free_data(srv);
}

TEST_F(LibraryTest, ParseSrvReplyMalformed) {
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
    0x00, 0x21,  // type SRV
    0x00, 0x01,  // class IN
    // Answer 1
    0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e',
    0x03, 'c', 'o', 'm',
    0x00,
    0x00, 0x21,  // RR type
    0x00, 0x01,  // class IN
    0x01, 0x02, 0x03, 0x04, // TTL
    0x00, 0x04,  // rdata length -- too short
    0x02, 0x03, 0x04, 0x05,
  };

  struct ares_srv_reply* srv = nullptr;
  EXPECT_EQ(ARES_EBADRESP, ares_parse_srv_reply(data.data(), data.size(), &srv));
  ASSERT_EQ(nullptr, srv);
}

TEST_F(LibraryTest, ParseSrvReplyMultiple) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_ra().set_rd()
    .add_question(new DNSQuestion("srv.example.com", T_SRV))
    .add_answer(new DNSSrvRR("srv.example.com", 300, 0, 5, 6789, "a1.srv.example.com"))
    .add_answer(new DNSSrvRR("srv.example.com", 300, 0, 5, 4567, "a2.srv.example.com"))
    .add_answer(new DNSSrvRR("srv.example.com", 300, 0, 5, 5678, "a3.srv.example.com"))
    .add_auth(new DNSNsRR("example.com", 300, "ns1.example.com"))
    .add_auth(new DNSNsRR("example.com", 300, "ns2.example.com"))
    .add_auth(new DNSNsRR("example.com", 300, "ns3.example.com"))
    .add_additional(new DNSARR("a1.srv.example.com", 300, {172,19,1,1}))
    .add_additional(new DNSARR("a2.srv.example.com", 300, {172,19,1,2}))
    .add_additional(new DNSARR("a3.srv.example.com", 300, {172,19,1,3}))
    .add_additional(new DNSARR("n1.example.com", 300, {172,19,0,1}))
    .add_additional(new DNSARR("n2.example.com", 300, {172,19,0,2}))
    .add_additional(new DNSARR("n3.example.com", 300, {172,19,0,3}));
  std::vector<byte> data = pkt.data();

  struct ares_srv_reply* srv0 = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_srv_reply(data.data(), data.size(), &srv0));
  ASSERT_NE(nullptr, srv0);
  struct ares_srv_reply* srv = srv0;

  EXPECT_EQ("a1.srv.example.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(5, srv->weight);
  EXPECT_EQ(6789, srv->port);
  EXPECT_NE(nullptr, srv->next);
  srv = srv->next;

  EXPECT_EQ("a2.srv.example.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(5, srv->weight);
  EXPECT_EQ(4567, srv->port);
  EXPECT_NE(nullptr, srv->next);
  srv = srv->next;

  EXPECT_EQ("a3.srv.example.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(5, srv->weight);
  EXPECT_EQ(5678, srv->port);
  EXPECT_EQ(nullptr, srv->next);

  ares_free_data(srv0);
}

TEST_F(LibraryTest, ParseSrvReplyCname) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.abc.def.com", T_SRV))
    .add_answer(new DNSCnameRR("example.abc.def.com", 300, "cname.abc.def.com"))
    .add_answer(new DNSSrvRR("cname.abc.def.com", 300, 0, 10, 1234, "srv.abc.def.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else1.where.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else2.where.com"))
    .add_auth(new DNSNsRR("abc.def.com", 44, "else3.where.com"))
    .add_additional(new DNSARR("example.abc.def.com", 300, {172,19,0,1}))
    .add_additional(new DNSARR("else1.where.com", 42, {172,19,0,1}))
    .add_additional(new DNSARR("else2.where.com", 42, {172,19,0,2}))
    .add_additional(new DNSARR("else3.where.com", 42, {172,19,0,3}));
  std::vector<byte> data = pkt.data();

  struct ares_srv_reply* srv = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_srv_reply(data.data(), data.size(), &srv));
  ASSERT_NE(nullptr, srv);

  EXPECT_EQ("srv.abc.def.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(10, srv->weight);
  EXPECT_EQ(1234, srv->port);
  EXPECT_EQ(nullptr, srv->next);

  ares_free_data(srv);
}

TEST_F(LibraryTest, ParseSrvReplyCnameMultiple) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_ra().set_rd()
    .add_question(new DNSQuestion("query.example.com", T_SRV))
    .add_answer(new DNSCnameRR("query.example.com", 300, "srv.example.com"))
    .add_answer(new DNSSrvRR("srv.example.com", 300, 0, 5, 6789, "a1.srv.example.com"))
    .add_answer(new DNSSrvRR("srv.example.com", 300, 0, 5, 4567, "a2.srv.example.com"))
    .add_answer(new DNSSrvRR("srv.example.com", 300, 0, 5, 5678, "a3.srv.example.com"))
    .add_auth(new DNSNsRR("example.com", 300, "ns1.example.com"))
    .add_auth(new DNSNsRR("example.com", 300, "ns2.example.com"))
    .add_auth(new DNSNsRR("example.com", 300, "ns3.example.com"))
    .add_additional(new DNSARR("a1.srv.example.com", 300, {172,19,1,1}))
    .add_additional(new DNSARR("a2.srv.example.com", 300, {172,19,1,2}))
    .add_additional(new DNSARR("a3.srv.example.com", 300, {172,19,1,3}))
    .add_additional(new DNSARR("n1.example.com", 300, {172,19,0,1}))
    .add_additional(new DNSARR("n2.example.com", 300, {172,19,0,2}))
    .add_additional(new DNSARR("n3.example.com", 300, {172,19,0,3}));
  std::vector<byte> data = pkt.data();

  struct ares_srv_reply* srv0 = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_srv_reply(data.data(), data.size(), &srv0));
  ASSERT_NE(nullptr, srv0);
  struct ares_srv_reply* srv = srv0;

  EXPECT_EQ("a1.srv.example.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(5, srv->weight);
  EXPECT_EQ(6789, srv->port);
  EXPECT_NE(nullptr, srv->next);
  srv = srv->next;

  EXPECT_EQ("a2.srv.example.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(5, srv->weight);
  EXPECT_EQ(4567, srv->port);
  EXPECT_NE(nullptr, srv->next);
  srv = srv->next;

  EXPECT_EQ("a3.srv.example.com", std::string(srv->host));
  EXPECT_EQ(0, srv->priority);
  EXPECT_EQ(5, srv->weight);
  EXPECT_EQ(5678, srv->port);
  EXPECT_EQ(nullptr, srv->next);

  ares_free_data(srv0);
}

TEST_F(LibraryTest, ParseSrvReplyErrors) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.abc.def.com", T_SRV))
    .add_answer(new DNSSrvRR("example.abc.def.com", 180, 0, 10, 8160, "example.abc.def.com"));
  std::vector<byte> data;
  struct ares_srv_reply* srv = nullptr;

  // No question.
  pkt.questions_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_srv_reply(data.data(), data.size(), &srv));
  pkt.add_question(new DNSQuestion("example.abc.def.com", T_SRV));

#ifdef DISABLED
  // Question != answer
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("Axample.com", T_SRV));
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_srv_reply(data.data(), data.size(), &srv));
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("example.com", T_SRV));
#endif

  // Two questions.
  pkt.add_question(new DNSQuestion("example.abc.def.com", T_SRV));
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_srv_reply(data.data(), data.size(), &srv));
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR));

  // Wrong sort of answer.
  pkt.answers_.clear();
  pkt.add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"));
  data = pkt.data();
  EXPECT_EQ(ARES_SUCCESS, ares_parse_srv_reply(data.data(), data.size(), &srv));
  EXPECT_EQ(nullptr, srv);
  pkt.answers_.clear();
  pkt.add_answer(new DNSSrvRR("example.abc.def.com", 180, 0, 10, 8160, "example.abc.def.com"));

  // No answer.
  pkt.answers_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_srv_reply(data.data(), data.size(), &srv));
  pkt.add_answer(new DNSSrvRR("example.abc.def.com", 180, 0, 10, 8160, "example.abc.def.com"));

  // Truncated packets.
  data = pkt.data();
  for (size_t len = 1; len < data.size(); len++) {
    int rc = ares_parse_srv_reply(data.data(), len, &srv);
    EXPECT_TRUE(rc == ARES_EBADRESP || rc == ARES_EBADNAME);
  }
}

TEST_F(LibraryTest, ParseSrvReplyAllocFail) {
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("example.abc.def.com", T_SRV))
    .add_answer(new DNSCnameRR("example.com", 300, "c.example.com"))
    .add_answer(new DNSSrvRR("example.abc.def.com", 180, 0, 10, 8160, "example.abc.def.com"));
  std::vector<byte> data = pkt.data();
  struct ares_srv_reply* srv = nullptr;

  for (int ii = 1; ii <= 5; ii++) {
    ClearFails();
    SetAllocFail(ii);
    EXPECT_EQ(ARES_ENOMEM, ares_parse_srv_reply(data.data(), data.size(), &srv)) << ii;
  }
}

}  // namespace test
}  // namespace ares
