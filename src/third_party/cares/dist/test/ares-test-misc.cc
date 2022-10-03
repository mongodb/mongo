#include "ares-test.h"
#include "dns-proto.h"

#include <string>
#include <vector>

namespace ares {
namespace test {

TEST_F(DefaultChannelTest, GetServers) {
  std::vector<std::string> servers = GetNameServers(channel_);
  if (verbose) {
    for (const std::string& server : servers) {
      std::cerr << "Nameserver: " << server << std::endl;
    }
  }
}

TEST_F(DefaultChannelTest, GetServersFailures) {
  EXPECT_EQ(ARES_SUCCESS,
            ares_set_servers_csv(channel_, "1.2.3.4,2.3.4.5"));
  struct ares_addr_node* servers = nullptr;
  SetAllocFail(1);
  EXPECT_EQ(ARES_ENOMEM, ares_get_servers(channel_, &servers));
  SetAllocFail(2);
  EXPECT_EQ(ARES_ENOMEM, ares_get_servers(channel_, &servers));
  EXPECT_EQ(ARES_ENODATA, ares_get_servers(nullptr, &servers));
}

TEST_F(DefaultChannelTest, SetServers) {
  EXPECT_EQ(ARES_SUCCESS, ares_set_servers(channel_, nullptr));
  std::vector<std::string> empty;
  EXPECT_EQ(empty, GetNameServers(channel_));

  struct ares_addr_node server1;
  struct ares_addr_node server2;
  server1.next = &server2;
  server1.family = AF_INET;
  server1.addr.addr4.s_addr = htonl(0x01020304);
  server2.next = nullptr;
  server2.family = AF_INET;
  server2.addr.addr4.s_addr = htonl(0x02030405);
  EXPECT_EQ(ARES_ENODATA, ares_set_servers(nullptr, &server1));

  EXPECT_EQ(ARES_SUCCESS, ares_set_servers(channel_, &server1));
  std::vector<std::string> expected = {"1.2.3.4", "2.3.4.5"};
  EXPECT_EQ(expected, GetNameServers(channel_));
}

TEST_F(DefaultChannelTest, SetServersPorts) {
  EXPECT_EQ(ARES_SUCCESS, ares_set_servers_ports(channel_, nullptr));
  std::vector<std::string> empty;
  EXPECT_EQ(empty, GetNameServers(channel_));

  struct ares_addr_port_node server1;
  struct ares_addr_port_node server2;
  server1.next = &server2;
  server1.family = AF_INET;
  server1.addr.addr4.s_addr = htonl(0x01020304);
  server1.udp_port = 111;
  server1.tcp_port = 111;
  server2.next = nullptr;
  server2.family = AF_INET;
  server2.addr.addr4.s_addr = htonl(0x02030405);
  server2.udp_port = 0;
  server2.tcp_port = 0;;
  EXPECT_EQ(ARES_ENODATA, ares_set_servers_ports(nullptr, &server1));

  EXPECT_EQ(ARES_SUCCESS, ares_set_servers_ports(channel_, &server1));
  std::vector<std::string> expected = {"1.2.3.4:111", "2.3.4.5"};
  EXPECT_EQ(expected, GetNameServers(channel_));
}

TEST_F(DefaultChannelTest, SetServersCSV) {
  EXPECT_EQ(ARES_ENODATA, ares_set_servers_csv(nullptr, "1.2.3.4"));
  EXPECT_EQ(ARES_ENODATA, ares_set_servers_csv(nullptr, "xyzzy,plugh"));
  EXPECT_EQ(ARES_ENODATA, ares_set_servers_csv(nullptr, "256.1.2.3"));
  EXPECT_EQ(ARES_ENODATA, ares_set_servers_csv(nullptr, "1.2.3.4.5"));
  EXPECT_EQ(ARES_ENODATA, ares_set_servers_csv(nullptr, "1:2:3:4:5"));

  EXPECT_EQ(ARES_SUCCESS,
            ares_set_servers_csv(channel_, "1.2.3.4,0102:0304:0506:0708:0910:1112:1314:1516,2.3.4.5"));
  std::vector<std::string> expected = {"1.2.3.4", "0102:0304:0506:0708:0910:1112:1314:1516", "2.3.4.5"};
  EXPECT_EQ(expected, GetNameServers(channel_));

  // Same, with spaces
  EXPECT_EQ(ARES_EBADSTR,
            ares_set_servers_csv(channel_, "1.2.3.4 , 0102:0304:0506:0708:0910:1112:1314:1516, 2.3.4.5"));

  // Same, with ports
  EXPECT_EQ(ARES_SUCCESS,
            ares_set_servers_csv(channel_, "1.2.3.4:54,[0102:0304:0506:0708:0910:1112:1314:1516]:80,2.3.4.5:55"));
  EXPECT_EQ(expected, GetNameServers(channel_));
  EXPECT_EQ(ARES_SUCCESS,
            ares_set_servers_ports_csv(channel_, "1.2.3.4:54,[0102:0304:0506:0708:0910:1112:1314:1516]:80,2.3.4.5:55"));
  std::vector<std::string> expected2 = {"1.2.3.4:54", "[0102:0304:0506:0708:0910:1112:1314:1516]:80", "2.3.4.5:55"};
  EXPECT_EQ(expected2, GetNameServers(channel_));

  // Should survive duplication
  ares_channel channel2;
  EXPECT_EQ(ARES_SUCCESS, ares_dup(&channel2, channel_));
  EXPECT_EQ(expected2, GetNameServers(channel2));
  ares_destroy(channel2);

  // Allocation failure cases
  for (int fail = 1; fail <= 5; fail++) {
    SetAllocFail(fail);
    EXPECT_EQ(ARES_ENOMEM,
              ares_set_servers_csv(channel_, "1.2.3.4,0102:0304:0506:0708:0910:1112:1314:1516,2.3.4.5"));
  }

  // Blank servers
  EXPECT_EQ(ARES_SUCCESS, ares_set_servers_csv(channel_, ""));
  std::vector<std::string> none;
  EXPECT_EQ(none, GetNameServers(channel_));

  EXPECT_EQ(ARES_EBADSTR, ares_set_servers_csv(channel_, "2.3.4.5,1.2.3.4:,3.4.5.6"));
  EXPECT_EQ(ARES_EBADSTR, ares_set_servers_csv(channel_, "2.3.4.5,1.2.3.4:Z,3.4.5.6"));
}

TEST_F(DefaultChannelTest, TimeoutValue) {
  struct timeval tinfo;
  tinfo.tv_sec = 0;
  tinfo.tv_usec = 0;
  struct timeval tmax;
  tmax.tv_sec = 0;
  tmax.tv_usec = 10;
  struct timeval* pt;

  // No timers => get max back.
  pt = ares_timeout(channel_, &tmax, &tinfo);
  EXPECT_EQ(&tmax, pt);
  EXPECT_EQ(0, pt->tv_sec);
  EXPECT_EQ(10, pt->tv_usec);

  pt = ares_timeout(channel_, nullptr, &tinfo);
  EXPECT_EQ(nullptr, pt);

  HostResult result;
  ares_gethostbyname(channel_, "www.google.com.", AF_INET, HostCallback, &result);

  // Now there's a timer running.
  pt = ares_timeout(channel_, &tmax, &tinfo);
  EXPECT_EQ(&tmax, pt);
  EXPECT_EQ(0, pt->tv_sec);
  EXPECT_EQ(10, pt->tv_usec);

  tmax.tv_sec = 100;
  pt = ares_timeout(channel_, &tmax, &tinfo);
  EXPECT_EQ(&tinfo, pt);

  pt = ares_timeout(channel_, nullptr, &tinfo);
  EXPECT_EQ(&tinfo, pt);

  Process();
}

TEST_F(LibraryTest, InetNtoP) {
  struct in_addr addr;
  addr.s_addr = htonl(0x01020304);
  char buffer[256];
  EXPECT_EQ(buffer, ares_inet_ntop(AF_INET, &addr, buffer, sizeof(buffer)));
  EXPECT_EQ("1.2.3.4", std::string(buffer));
}

TEST_F(LibraryTest, Mkquery) {
  byte* p;
  int len;
  ares_mkquery("example.com", C_IN, T_A, 0x1234, 0, &p, &len);
  std::vector<byte> data(p, p + len);
  ares_free_string(p);

  std::string actual = PacketToString(data);
  DNSPacket pkt;
  pkt.set_qid(0x1234).add_question(new DNSQuestion("example.com", T_A));
  std::string expected = PacketToString(pkt.data());
  EXPECT_EQ(expected, actual);
}

TEST_F(LibraryTest, CreateQuery) {
  byte* p;
  int len;
  EXPECT_EQ(ARES_SUCCESS,
            ares_create_query("exam\\@le.com", C_IN, T_A, 0x1234, 0,
                              &p, &len, 0));
  std::vector<byte> data(p, p + len);
  ares_free_string(p);

  std::string actual = PacketToString(data);
  DNSPacket pkt;
  pkt.set_qid(0x1234).add_question(new DNSQuestion("exam@le.com", T_A));
  std::string expected = PacketToString(pkt.data());
  EXPECT_EQ(expected, actual);
}

TEST_F(LibraryTest, CreateQueryTrailingEscapedDot) {
  byte* p;
  int len;
  EXPECT_EQ(ARES_SUCCESS,
            ares_create_query("example.com\\.", C_IN, T_A, 0x1234, 0,
                              &p, &len, 0));
  std::vector<byte> data(p, p + len);
  ares_free_string(p);

  std::string actual = PacketToString(data);
  EXPECT_EQ("REQ QRY  Q:{'example.com\\.' IN A}", actual);
}

TEST_F(LibraryTest, CreateQueryNameTooLong) {
  byte* p;
  int len;
  EXPECT_EQ(ARES_EBADNAME,
            ares_create_query(
              "a1234567890123456789.b1234567890123456789.c1234567890123456789.d1234567890123456789."
              "a1234567890123456789.b1234567890123456789.c1234567890123456789.d1234567890123456789."
              "a1234567890123456789.b1234567890123456789.c1234567890123456789.d1234567890123456789."
              "x1234567890123456789.y1234567890123456789.",
              C_IN, T_A, 0x1234, 0, &p, &len, 0));
}

TEST_F(LibraryTest, CreateQueryFailures) {
  byte* p;
  int len;
  // RC1035 has a 255 byte limit on names.
  std::string longname;
  for (int ii = 0; ii < 17; ii++) {
    longname += "fedcba9876543210";
  }
  p = nullptr;
  EXPECT_EQ(ARES_EBADNAME,
            ares_create_query(longname.c_str(), C_IN, T_A, 0x1234, 0,
                    &p, &len, 0));
  if (p) ares_free_string(p);

  SetAllocFail(1);

  p = nullptr;
  EXPECT_EQ(ARES_ENOMEM,
            ares_create_query("example.com", C_IN, T_A, 0x1234, 0,
                    &p, &len, 0));
  if (p) ares_free_string(p);

  // 63-char limit on a single label
  std::string longlabel = "a.a123456789b123456789c123456789d123456789e123456789f123456789g123456789.org";
  p = nullptr;
  EXPECT_EQ(ARES_EBADNAME,
            ares_create_query(longlabel.c_str(), C_IN, T_A, 0x1234, 0,
                    &p, &len, 0));
  if (p) ares_free_string(p);

  // Empty non-terminal label
  p = nullptr;
  EXPECT_EQ(ARES_EBADNAME,
            ares_create_query("example..com", C_IN, T_A, 0x1234, 0,
                    &p, &len, 0));
  if (p) ares_free_string(p);
}

TEST_F(LibraryTest, CreateQueryOnionDomain) {
  byte* p;
  int len;
  EXPECT_EQ(ARES_ENOTFOUND,
            ares_create_query("dontleak.onion", C_IN, T_A, 0x1234, 0,
                              &p, &len, 0));
}

TEST_F(DefaultChannelTest, HostByNameOnionDomain) {
  HostResult result;
  ares_gethostbyname(channel_, "dontleak.onion", AF_INET, HostCallback, &result);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENOTFOUND, result.status_);
}

TEST_F(DefaultChannelTest, HostByNameFileOnionDomain) {
  struct hostent *h;
  EXPECT_EQ(ARES_ENOTFOUND,
            ares_gethostbyname_file(channel_, "dontleak.onion", AF_INET, &h));
}

TEST_F(DefaultChannelTest, GetAddrinfoOnionDomain) {
  AddrInfoResult result;
  struct ares_addrinfo_hints hints = {};
  hints.ai_family = AF_UNSPEC;
  ares_getaddrinfo(channel_, "dontleak.onion", NULL, &hints, AddrInfoCallback, &result);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENOTFOUND, result.status_);
}

// Interesting question: should tacking on a search domain let the query
// through? It seems safer to reject it because "supersecret.onion.search"
// still leaks information about the query to malicious resolvers.
TEST_F(DefaultChannelTest, SearchOnionDomain) {
  SearchResult result;
  ares_search(channel_, "dontleak.onion", C_IN, T_A,
              SearchCallback, &result);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_ENOTFOUND, result.status_);
}

TEST_F(DefaultChannelTest, SendFailure) {
  unsigned char buf[2];
  SearchResult result;
  ares_send(channel_, buf, sizeof(buf), SearchCallback, &result);
  EXPECT_TRUE(result.done_);
  EXPECT_EQ(ARES_EBADQUERY, result.status_);
}

std::string ExpandName(const std::vector<byte>& data, int offset,
                       long *enclen) {
  char *name = nullptr;
  int rc = ares_expand_name(data.data() + offset, data.data(), data.size(),
                            &name, enclen);
  EXPECT_EQ(ARES_SUCCESS, rc);
  std::string result;
  if (rc == ARES_SUCCESS) {
    result = name;
  } else {
    result = "<error>";
  }
  ares_free_string(name);
  return result;
}

TEST_F(LibraryTest, ExpandName) {
  long enclen;
  std::vector<byte> data1 = {1, 'a', 2, 'b', 'c', 3, 'd', 'e', 'f', 0};
  EXPECT_EQ("a.bc.def", ExpandName(data1, 0, &enclen));
  EXPECT_EQ(data1.size(), enclen);

  std::vector<byte> data2 = {0};
  EXPECT_EQ("", ExpandName(data2, 0, &enclen));
  EXPECT_EQ(1, enclen);

  // Complete name indirection
  std::vector<byte> data3 = {0x12, 0x23,
                             3, 'd', 'e', 'f', 0,
                             0xC0, 2};
  EXPECT_EQ("def", ExpandName(data3, 2, &enclen));
  EXPECT_EQ(5, enclen);
  EXPECT_EQ("def", ExpandName(data3, 7, &enclen));
  EXPECT_EQ(2, enclen);

  // One label then indirection
  std::vector<byte> data4 = {0x12, 0x23,
                             3, 'd', 'e', 'f', 0,
                             1, 'a', 0xC0, 2};
  EXPECT_EQ("def", ExpandName(data4, 2, &enclen));
  EXPECT_EQ(5, enclen);
  EXPECT_EQ("a.def", ExpandName(data4, 7, &enclen));
  EXPECT_EQ(4, enclen);

  // Two labels then indirection
  std::vector<byte> data5 = {0x12, 0x23,
                             3, 'd', 'e', 'f', 0,
                             1, 'a', 1, 'b', 0xC0, 2};
  EXPECT_EQ("def", ExpandName(data5, 2, &enclen));
  EXPECT_EQ(5, enclen);
  EXPECT_EQ("a.b.def", ExpandName(data5, 7, &enclen));
  EXPECT_EQ(6, enclen);

  // Empty name, indirection to empty name
  std::vector<byte> data6 = {0x12, 0x23,
                             0,
                             0xC0, 2};
  EXPECT_EQ("", ExpandName(data6, 2, &enclen));
  EXPECT_EQ(1, enclen);
  EXPECT_EQ("", ExpandName(data6, 3, &enclen));
  EXPECT_EQ(2, enclen);
}

TEST_F(LibraryTest, ExpandNameFailure) {
  std::vector<byte> data1 = {0x03, 'c', 'o', 'm', 0x00};
  char *name = nullptr;
  long enclen;
  SetAllocFail(1);
  EXPECT_EQ(ARES_ENOMEM,
            ares_expand_name(data1.data(), data1.data(), data1.size(),
                             &name, &enclen));

  // Empty packet
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data1.data(), data1.data(), 0, &name, &enclen));

  // Start beyond enclosing data
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data1.data() + data1.size(), data1.data(), data1.size(),
                             &name, &enclen));

  // Length beyond size of enclosing data
  std::vector<byte> data2a = {0x13, 'c', 'o', 'm', 0x00};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data2a.data(), data2a.data(), data2a.size(),
                             &name, &enclen));
  std::vector<byte> data2b = {0x1};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data2b.data(), data2b.data(), data2b.size(),
                             &name, &enclen));
  std::vector<byte> data2c = {0xC0};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data2c.data(), data2c.data(), data2c.size(),
                             &name, &enclen));

  // Indirection beyond enclosing data
  std::vector<byte> data3a = {0xC0, 0x02};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data3a.data(), data3a.data(), data3a.size(),
                             &name, &enclen));
  std::vector<byte> data3b = {0xC0, 0x0A, 'c', 'o', 'm', 0x00};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data3b.data(), data3b.data(), data3b.size(),
                             &name, &enclen));

  // Invalid top bits in label length
  std::vector<byte> data4 = {0x03, 'c', 'o', 'm', 0x00, 0x80, 0x00};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data4.data() + 5, data4.data(), data4.size(),
                             &name, &enclen));

  // Label too long: 64-byte label, with invalid top 2 bits of length (01).
  std::vector<byte> data5 = {0x40,
                             '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
                             '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
                             '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
                             '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
                             0x00};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data5.data(), data5.data(), data5.size(),
                             &name, &enclen)) << name;

  // Incomplete indirect length
  std::vector<byte> data6 = {0x03, 'c', 'o', 'm', 0x00, 0xC0};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data6.data() + 5, data6.data(), data6.size(),
                             &name, &enclen));

  // Indirection loops
  std::vector<byte> data7 = {0xC0, 0x02, 0xC0, 0x00};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data7.data(), data7.data(), data7.size(),
                             &name, &enclen));
  std::vector<byte> data8 = {3, 'd', 'e', 'f', 0xC0, 0x08, 0x00, 0x00,
                             3, 'a', 'b', 'c', 0xC0, 0x00};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data8.data(), data8.data(), data8.size(),
                             &name, &enclen));
  std::vector<byte> data9 = {0x12, 0x23,  // start 2 bytes in
                             3, 'd', 'e', 'f', 0xC0, 0x02};
  EXPECT_EQ(ARES_EBADNAME,
            ares_expand_name(data9.data() + 2, data9.data(), data9.size(),
                             &name, &enclen));
}

TEST_F(LibraryTest, CreateEDNSQuery) {
  byte* p;
  int len;
  EXPECT_EQ(ARES_SUCCESS,
            ares_create_query("example.com", C_IN, T_A, 0x1234, 0,
                              &p, &len, 1280));
  std::vector<byte> data(p, p + len);
  ares_free_string(p);

  std::string actual = PacketToString(data);
  DNSPacket pkt;
  pkt.set_qid(0x1234).add_question(new DNSQuestion("example.com", T_A))
    .add_additional(new DNSOptRR(0, 1280));
  std::string expected = PacketToString(pkt.data());
  EXPECT_EQ(expected, actual);
}

TEST_F(LibraryTest, CreateRootQuery) {
  byte* p;
  int len;
  ares_create_query(".", C_IN, T_A, 0x1234, 0, &p, &len, 0);
  std::vector<byte> data(p, p + len);
  ares_free_string(p);

  std::string actual = PacketToString(data);
  DNSPacket pkt;
  pkt.set_qid(0x1234).add_question(new DNSQuestion("", T_A));
  std::string expected = PacketToString(pkt.data());
  EXPECT_EQ(expected, actual);
}

TEST_F(LibraryTest, Version) {
  // Assume linked to same version
  EXPECT_EQ(std::string(ARES_VERSION_STR),
            std::string(ares_version(nullptr)));
  int version;
  ares_version(&version);
  EXPECT_EQ(ARES_VERSION, version);
}

TEST_F(LibraryTest, Strerror) {
  EXPECT_EQ("Successful completion",
            std::string(ares_strerror(ARES_SUCCESS)));
  EXPECT_EQ("DNS query cancelled",
            std::string(ares_strerror(ARES_ECANCELLED)));
  EXPECT_EQ("unknown",
            std::string(ares_strerror(99)));
}

TEST_F(LibraryTest, ExpandString) {
  std::vector<byte> s1 = { 3, 'a', 'b', 'c'};
  char* result = nullptr;
  long len;
  EXPECT_EQ(ARES_SUCCESS,
            ares_expand_string(s1.data(), s1.data(), s1.size(),
                               (unsigned char**)&result, &len));
  EXPECT_EQ("abc", std::string(result));
  EXPECT_EQ(1 + 3, len);  // amount of data consumed includes 1 byte len
  ares_free_string(result);
  result = nullptr;
  EXPECT_EQ(ARES_EBADSTR,
            ares_expand_string(s1.data() + 1, s1.data(), s1.size(),
                               (unsigned char**)&result, &len));
  EXPECT_EQ(ARES_EBADSTR,
            ares_expand_string(s1.data() + 4, s1.data(), s1.size(),
                               (unsigned char**)&result, &len));
  SetAllocSizeFail(3 + 1);
  EXPECT_EQ(ARES_ENOMEM,
            ares_expand_string(s1.data(), s1.data(), s1.size(),
                               (unsigned char**)&result, &len));
}

}  // namespace test
}  // namespace ares
