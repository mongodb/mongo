#include "ares-test.h"
#include "dns-proto.h"

#include <sstream>
#include <vector>

namespace ares {
namespace test {

TEST_F(LibraryTest, ParsePtrReplyOK) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other.com"));
  std::vector<byte> data = pkt.data();

  struct hostent *host = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_ptr_reply(data.data(), data.size(),
                                               addrv4, sizeof(addrv4), AF_INET, &host));
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'other.com' aliases=[other.com] addrs=[16.32.48.64]}", ss.str());
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParsePtrReplyCname) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSCnameRR("64.48.32.16.in-addr.arpa", 50, "64.48.32.8.in-addr.arpa"))
    .add_answer(new DNSPtrRR("64.48.32.8.in-addr.arpa", 100, "other.com"));
  std::vector<byte> data = pkt.data();

  struct hostent *host = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_ptr_reply(data.data(), data.size(),
                                               addrv4, sizeof(addrv4), AF_INET, &host));
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'other.com' aliases=[other.com] addrs=[16.32.48.64]}", ss.str());
  ares_free_hostent(host);
}


struct DNSMalformedCnameRR : public DNSCnameRR {
  DNSMalformedCnameRR(const std::string& name, int ttl, const std::string& other)
    : DNSCnameRR(name, ttl, other) {}
  std::vector<byte> data() const {
    std::vector<byte> data = DNSRR::data();
    std::vector<byte> encname = EncodeString(other_);
    encname[0] = encname[0] + 63;  // invalid label length
    int len = encname.size();
    PushInt16(&data, len);
    data.insert(data.end(), encname.begin(), encname.end());
    return data;
  }
};

TEST_F(LibraryTest, ParsePtrReplyMalformedCname) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSMalformedCnameRR("64.48.32.16.in-addr.arpa", 50, "64.48.32.8.in-addr.arpa"))
    .add_answer(new DNSPtrRR("64.48.32.8.in-addr.arpa", 100, "other.com"));
  std::vector<byte> data = pkt.data();

  struct hostent *host = nullptr;
  EXPECT_EQ(ARES_EBADRESP, ares_parse_ptr_reply(data.data(), data.size(),
                                                addrv4, sizeof(addrv4), AF_INET, &host));
  ASSERT_EQ(nullptr, host);
}

TEST_F(LibraryTest, ParseManyPtrReply) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "main.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other1.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other2.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other3.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other4.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other5.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other6.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other7.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other8.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other9.com"));
  std::vector<byte> data = pkt.data();

  struct hostent *host = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_ptr_reply(data.data(), data.size(),
                                               addrv4, sizeof(addrv4), AF_INET, &host));
  ASSERT_NE(nullptr, host);
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParsePtrReplyAdditional) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 55, "other.com"))
    .add_auth(new DNSNsRR("16.in-addr.arpa", 234, "ns1.other.com"))
    .add_auth(new DNSNsRR("16.in-addr.arpa", 234, "bb.ns2.other.com"))
    .add_auth(new DNSNsRR("16.in-addr.arpa", 234, "ns3.other.com"))
    .add_additional(new DNSARR("ns1.other.com", 229, {10,20,30,41}))
    .add_additional(new DNSARR("bb.ns2.other.com", 229, {10,20,30,42}))
    .add_additional(new DNSARR("ns3.other.com", 229, {10,20,30,43}));
  std::vector<byte> data = pkt.data();

  struct hostent *host = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_parse_ptr_reply(data.data(), data.size(),
                                               addrv4, sizeof(addrv4), AF_INET, &host));
  ASSERT_NE(nullptr, host);
  std::stringstream ss;
  ss << HostEnt(host);
  EXPECT_EQ("{'other.com' aliases=[other.com] addrs=[16.32.48.64]}", ss.str());
  ares_free_hostent(host);
}

TEST_F(LibraryTest, ParsePtrReplyErrors) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other.com"));
  std::vector<byte> data;
  struct hostent *host = nullptr;

  // No question.
  pkt.questions_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_ptr_reply(data.data(), data.size(),
                                                addrv4, sizeof(addrv4), AF_INET, &host));
  pkt.add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR));

  // Question != answer
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("99.48.32.16.in-addr.arpa", T_PTR));
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_ptr_reply(data.data(), data.size(),
                                               addrv4, sizeof(addrv4), AF_INET, &host));
  EXPECT_EQ(nullptr, host);
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR));

  // Two questions.
  pkt.add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR));
  data = pkt.data();
  EXPECT_EQ(ARES_EBADRESP, ares_parse_ptr_reply(data.data(), data.size(),
                                                addrv4, sizeof(addrv4), AF_INET, &host));
  EXPECT_EQ(nullptr, host);
  pkt.questions_.clear();
  pkt.add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR));

  // Wrong sort of answer.
  pkt.answers_.clear();
  pkt.add_answer(new DNSMxRR("example.com", 100, 100, "mx1.example.com"));
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_ptr_reply(data.data(), data.size(),
                                               addrv4, sizeof(addrv4), AF_INET, &host));
  EXPECT_EQ(nullptr, host);
  pkt.answers_.clear();
  pkt.add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other.com"));

  // No answer.
  pkt.answers_.clear();
  data = pkt.data();
  EXPECT_EQ(ARES_ENODATA, ares_parse_ptr_reply(data.data(), data.size(),
                                               addrv4, sizeof(addrv4), AF_INET, &host));
  EXPECT_EQ(nullptr, host);
  pkt.add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other.com"));

  // Truncated packets.
  data = pkt.data();
  for (size_t len = 1; len < data.size(); len++) {
    EXPECT_EQ(ARES_EBADRESP, ares_parse_ptr_reply(data.data(), len,
                                                  addrv4, sizeof(addrv4), AF_INET, &host));
    EXPECT_EQ(nullptr, host);
  }

  // Truncated packets with CNAME.
  pkt.add_answer(new DNSCnameRR("64.48.32.16.in-addr.arpa", 50, "64.48.32.8.in-addr.arpa"));
  data = pkt.data();
  for (size_t len = 1; len < data.size(); len++) {
    EXPECT_EQ(ARES_EBADRESP, ares_parse_ptr_reply(data.data(), len,
                                                  addrv4, sizeof(addrv4), AF_INET, &host));
    EXPECT_EQ(nullptr, host);
  }
}

TEST_F(LibraryTest, ParsePtrReplyAllocFailSome) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "main.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other1.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other2.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other3.com"));
  std::vector<byte> data = pkt.data();
  struct hostent *host = nullptr;

  for (int ii = 1; ii <= 18; ii++) {
    ClearFails();
    SetAllocFail(ii);
    EXPECT_EQ(ARES_ENOMEM, ares_parse_ptr_reply(data.data(), data.size(),
                                                addrv4, sizeof(addrv4), AF_INET, &host)) << ii;
  }
}

TEST_F(LibraryTest, ParsePtrReplyAllocFailMany) {
  byte addrv4[4] = {0x10, 0x20, 0x30, 0x40};
  DNSPacket pkt;
  pkt.set_qid(0x1234).set_response().set_aa()
    .add_question(new DNSQuestion("64.48.32.16.in-addr.arpa", T_PTR))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "main.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other1.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other2.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other3.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other4.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other5.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other6.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other7.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other8.com"))
    .add_answer(new DNSPtrRR("64.48.32.16.in-addr.arpa", 100, "other9.com"));
  std::vector<byte> data = pkt.data();
  struct hostent *host = nullptr;

  for (int ii = 1; ii <= 63; ii++) {
    ClearFails();
    SetAllocFail(ii);
    int rc = ares_parse_ptr_reply(data.data(), data.size(),
                                  addrv4, sizeof(addrv4), AF_INET, &host);
    if (rc != ARES_ENOMEM) {
      EXPECT_EQ(ARES_SUCCESS, rc);
      ares_free_hostent(host);
      host = nullptr;
    }
  }
}


}  // namespace test
}  // namespace ares
