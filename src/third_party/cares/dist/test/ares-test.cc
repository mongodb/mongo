#include "ares_setup.h"
#include "ares.h"
#include "ares_nameser.h"
#include "ares-test.h"
#include "ares-test-ai.h"
#include "dns-proto.h"

// Include ares internal files for DNS protocol details
#include "ares_dns.h"

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <stdio.h>
#include <stdlib.h>

#include <functional>
#include <sstream>

#ifdef WIN32
#define BYTE_CAST (char *)
#define mkdir_(d, p) mkdir(d)
#else
#define BYTE_CAST
#define mkdir_(d, p) mkdir(d, p)
#endif

namespace ares {
namespace test {

bool verbose = false;
static constexpr int dynamic_port = 0;
int mock_port = dynamic_port;

const std::vector<int> both_families = {AF_INET, AF_INET6};
const std::vector<int> ipv4_family = {AF_INET};
const std::vector<int> ipv6_family = {AF_INET6};

const std::vector<std::pair<int, bool>> both_families_both_modes = {
  std::make_pair<int, bool>(AF_INET, false),
  std::make_pair<int, bool>(AF_INET, true),
  std::make_pair<int, bool>(AF_INET6, false),
  std::make_pair<int, bool>(AF_INET6, true)
};
const std::vector<std::pair<int, bool>> ipv4_family_both_modes = {
  std::make_pair<int, bool>(AF_INET, false),
  std::make_pair<int, bool>(AF_INET, true)
};
const std::vector<std::pair<int, bool>> ipv6_family_both_modes = {
  std::make_pair<int, bool>(AF_INET6, false),
  std::make_pair<int, bool>(AF_INET6, true)
};

// Which parameters to use in tests
std::vector<int> families = both_families;
std::vector<std::pair<int, bool>> families_modes = both_families_both_modes;

unsigned long long LibraryTest::fails_ = 0;
std::map<size_t, int> LibraryTest::size_fails_;

void ProcessWork(ares_channel channel,
                 std::function<std::set<int>()> get_extrafds,
                 std::function<void(int)> process_extra) {
  int nfds, count;
  fd_set readers, writers;
  struct timeval tv;
  while (true) {
    // Retrieve the set of file descriptors that the library wants us to monitor.
    FD_ZERO(&readers);
    FD_ZERO(&writers);
    nfds = ares_fds(channel, &readers, &writers);
    if (nfds == 0)  // no work left to do in the library
      return;

    // Add in the extra FDs if present.
    std::set<int> extrafds = get_extrafds();
    for (int extrafd : extrafds) {
      FD_SET(extrafd, &readers);
      if (extrafd >= nfds) {
        nfds = extrafd + 1;
      }
    }

    // Wait for activity or timeout.
    tv.tv_sec = 0;
    tv.tv_usec = 100000;  // 100ms
    count = select(nfds, &readers, &writers, nullptr, &tv);
    if (count < 0) {
      fprintf(stderr, "select() failed, errno %d\n", errno);
      return;
    }

    // Let the library process any activity.
    ares_process(channel, &readers, &writers);

    // Let the provided callback process any activity on the extra FD.
    for (int extrafd : extrafds) {
      if (FD_ISSET(extrafd, &readers)) {
        process_extra(extrafd);
      }
    }
  }
}

// static
void LibraryTest::SetAllocFail(int nth) {
  assert(nth > 0);
  assert(nth <= (int)(8 * sizeof(fails_)));
  fails_ |= (1LL << (nth - 1));
}

// static
void LibraryTest::SetAllocSizeFail(size_t size) {
  size_fails_[size]++;
}

// static
void LibraryTest::ClearFails() {
  fails_ = 0;
  size_fails_.clear();
}


// static
bool LibraryTest::ShouldAllocFail(size_t size) {
  bool fail = (fails_ & 0x01);
  fails_ >>= 1;
  if (size_fails_[size] > 0) {
    size_fails_[size]--;
    fail = true;
  }
  return fail;
}

// static
void* LibraryTest::amalloc(size_t size) {
  if (ShouldAllocFail(size) || size == 0) {
    if (verbose) std::cerr << "Failing malloc(" << size << ") request" << std::endl;
    return nullptr;
  } else {
    return malloc(size);
  }
}

// static
void* LibraryTest::arealloc(void *ptr, size_t size) {
  if (ShouldAllocFail(size)) {
    if (verbose) std::cerr << "Failing realloc(" << ptr << ", " << size << ") request" << std::endl;
    return nullptr;
  } else {
    return realloc(ptr, size);
  }
}

// static
void LibraryTest::afree(void *ptr) {
  free(ptr);
}

std::set<int> NoExtraFDs() {
  return std::set<int>();
}

void DefaultChannelTest::Process() {
  ProcessWork(channel_, NoExtraFDs, nullptr);
}

void DefaultChannelModeTest::Process() {
  ProcessWork(channel_, NoExtraFDs, nullptr);
}

MockServer::MockServer(int family, int port)
  : udpport_(port), tcpport_(port), qid_(-1) {
  // Create a TCP socket to receive data on.
  tcpfd_ = socket(family, SOCK_STREAM, 0);
  EXPECT_NE(-1, tcpfd_);
  int optval = 1;
  setsockopt(tcpfd_, SOL_SOCKET, SO_REUSEADDR,
             BYTE_CAST &optval , sizeof(int));
  // Send TCP data right away.
  setsockopt(tcpfd_, IPPROTO_TCP, TCP_NODELAY,
             BYTE_CAST &optval , sizeof(int));

  // Create a UDP socket to receive data on.
  udpfd_ = socket(family, SOCK_DGRAM, 0);
  EXPECT_NE(-1, udpfd_);

  // Bind the sockets to the given port.
  if (family == AF_INET) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(tcpport_);
    int tcprc = bind(tcpfd_, (struct sockaddr*)&addr, sizeof(addr));
    EXPECT_EQ(0, tcprc) << "Failed to bind AF_INET to TCP port " << tcpport_;
    addr.sin_port = htons(udpport_);
    int udprc = bind(udpfd_, (struct sockaddr*)&addr, sizeof(addr));
    EXPECT_EQ(0, udprc) << "Failed to bind AF_INET to UDP port " << udpport_;
    // retrieve system-assigned port
    if (udpport_ == dynamic_port) {
      ares_socklen_t len = sizeof(addr);
      auto result = getsockname(udpfd_, (struct sockaddr*)&addr, &len);
      EXPECT_EQ(0, result);
      udpport_ = ntohs(addr.sin_port);
      EXPECT_NE(dynamic_port, udpport_);
    }
    if (tcpport_ == dynamic_port) {
      ares_socklen_t len = sizeof(addr);
      auto result = getsockname(tcpfd_, (struct sockaddr*)&addr, &len);
      EXPECT_EQ(0, result);
      tcpport_ = ntohs(addr.sin_port);
      EXPECT_NE(dynamic_port, tcpport_);
    }
  } else {
    EXPECT_EQ(AF_INET6, family);
    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    memset(&addr.sin6_addr, 0, sizeof(addr.sin6_addr));  // in6addr_any
    addr.sin6_port = htons(tcpport_);
    int tcprc = bind(tcpfd_, (struct sockaddr*)&addr, sizeof(addr));
    EXPECT_EQ(0, tcprc) << "Failed to bind AF_INET6 to TCP port " << tcpport_;
    addr.sin6_port = htons(udpport_);
    int udprc = bind(udpfd_, (struct sockaddr*)&addr, sizeof(addr));
    EXPECT_EQ(0, udprc) << "Failed to bind AF_INET6 to UDP port " << udpport_;
    // retrieve system-assigned port
    if (udpport_ == dynamic_port) {
      ares_socklen_t len = sizeof(addr);
      auto result = getsockname(udpfd_, (struct sockaddr*)&addr, &len);
      EXPECT_EQ(0, result);
      udpport_ = ntohs(addr.sin6_port);
      EXPECT_NE(dynamic_port, udpport_);
    }
    if (tcpport_ == dynamic_port) {
      ares_socklen_t len = sizeof(addr);
      auto result = getsockname(tcpfd_, (struct sockaddr*)&addr, &len);
      EXPECT_EQ(0, result);
      tcpport_ = ntohs(addr.sin6_port);
      EXPECT_NE(dynamic_port, tcpport_);
    }
  }
  if (verbose) std::cerr << "Configured "
                         << (family == AF_INET ? "IPv4" : "IPv6")
                         << " mock server with TCP socket " << tcpfd_
                         << " on port " << tcpport_
                         << " and UDP socket " << udpfd_
                         << " on port " << udpport_ << std::endl;

  // For TCP, also need to listen for connections.
  EXPECT_EQ(0, listen(tcpfd_, 5)) << "Failed to listen for TCP connections";
}

MockServer::~MockServer() {
  for (int fd : connfds_) {
    sclose(fd);
  }
  sclose(tcpfd_);
  sclose(udpfd_);
}

void MockServer::ProcessFD(int fd) {
  if (fd != tcpfd_ && fd != udpfd_ && connfds_.find(fd) == connfds_.end()) {
    // Not one of our FDs.
    return;
  }
  if (fd == tcpfd_) {
    int connfd = accept(tcpfd_, NULL, NULL);
    if (connfd < 0) {
      std::cerr << "Error accepting connection on fd " << fd << std::endl;
    } else {
      connfds_.insert(connfd);
    }
    return;
  }

  // Activity on a data-bearing file descriptor.
  struct sockaddr_storage addr;
  socklen_t addrlen = sizeof(addr);
  byte buffer[2048];
  int len = recvfrom(fd, BYTE_CAST buffer, sizeof(buffer), 0,
                     (struct sockaddr *)&addr, &addrlen);
  byte* data = buffer;
  if (fd != udpfd_) {
    if (len == 0) {
      connfds_.erase(std::find(connfds_.begin(), connfds_.end(), fd));
      sclose(fd);
      return;
    }
    if (len < 2) {
      std::cerr << "Packet too short (" << len << ")" << std::endl;
      return;
    }
    int tcplen = (data[0] << 8) + data[1];
    data += 2;
    len -= 2;
    if (tcplen != len) {
      std::cerr << "Warning: TCP length " << tcplen
                << " doesn't match remaining data length " << len << std::endl;
    }
  }

  // Assume the packet is a well-formed DNS request and extract the request
  // details.
  if (len < NS_HFIXEDSZ) {
    std::cerr << "Packet too short (" << len << ")" << std::endl;
    return;
  }
  int qid = DNS_HEADER_QID(data);
  if (DNS_HEADER_QR(data) != 0) {
    std::cerr << "Not a request" << std::endl;
    return;
  }
  if (DNS_HEADER_OPCODE(data) != O_QUERY) {
    std::cerr << "Not a query (opcode " << DNS_HEADER_OPCODE(data)
              << ")" << std::endl;
    return;
  }
  if (DNS_HEADER_QDCOUNT(data) != 1) {
    std::cerr << "Unexpected question count (" << DNS_HEADER_QDCOUNT(data)
              << ")" << std::endl;
    return;
  }
  byte* question = data + 12;
  int qlen = len - 12;

  char *name = nullptr;
  long enclen;
  ares_expand_name(question, data, len, &name, &enclen);
  if (!name) {
    std::cerr << "Failed to retrieve name" << std::endl;
    return;
  }
  qlen -= enclen;
  question += enclen;
  std::string namestr(name);
  ares_free_string(name);

  if (qlen < 4) {
    std::cerr << "Unexpected question size (" << qlen
              << " bytes after name)" << std::endl;
    return;
  }
  if (DNS_QUESTION_CLASS(question) != C_IN) {
    std::cerr << "Unexpected question class (" << DNS_QUESTION_CLASS(question)
              << ")" << std::endl;
    return;
  }
  int rrtype = DNS_QUESTION_TYPE(question);

  if (verbose) {
    std::vector<byte> req(data, data + len);
    std::cerr << "received " << (fd == udpfd_ ? "UDP" : "TCP") << " request " << PacketToString(req)
              << " on port " << (fd == udpfd_ ? udpport_ : tcpport_) << std::endl;
    std::cerr << "ProcessRequest(" << qid << ", '" << namestr
              << "', " << RRTypeToString(rrtype) << ")" << std::endl;
  }
  ProcessRequest(fd, &addr, addrlen, qid, namestr, rrtype);
}

std::set<int> MockServer::fds() const {
  std::set<int> result = connfds_;
  result.insert(tcpfd_);
  result.insert(udpfd_);
  return result;
}

void MockServer::ProcessRequest(int fd, struct sockaddr_storage* addr, int addrlen,
                                int qid, const std::string& name, int rrtype) {
  // Before processing, let gMock know the request is happening.
  OnRequest(name, rrtype);

  if (reply_.size() == 0) {
    return;
  }

  // Make a local copy of the current pending reply.
  std::vector<byte> reply = reply_;

  if (qid_ >= 0) {
    // Use the explicitly specified query ID.
    qid = qid_;
  }
  if (reply.size() >=  2) {
    // Overwrite the query ID if space to do so.
    reply[0] = (byte)((qid >> 8) & 0xff);
    reply[1] = (byte)(qid & 0xff);
  }
  if (verbose) std::cerr << "sending reply " << PacketToString(reply)
                         << " on port " << ((fd == udpfd_) ? udpport_ : tcpport_) << std::endl;

  // Prefix with 2-byte length if TCP.
  if (fd != udpfd_) {
    int len = reply.size();
    std::vector<byte> vlen = {(byte)((len & 0xFF00) >> 8), (byte)(len & 0xFF)};
    reply.insert(reply.begin(), vlen.begin(), vlen.end());
    // Also, don't bother with the destination address.
    addr = nullptr;
    addrlen = 0;
  }

  int rc = sendto(fd, BYTE_CAST reply.data(), reply.size(), 0,
                  (struct sockaddr *)addr, addrlen);
  if (rc < static_cast<int>(reply.size())) {
    std::cerr << "Failed to send full reply, rc=" << rc << std::endl;
  }
}

// static
MockChannelOptsTest::NiceMockServers MockChannelOptsTest::BuildServers(int count, int family, int base_port) {
  NiceMockServers servers;
  assert(count > 0);
  for (int ii = 0; ii < count; ii++) {
    int port = base_port == dynamic_port ? dynamic_port : base_port + ii;
    std::unique_ptr<NiceMockServer> server(new NiceMockServer(family, port));
    servers.push_back(std::move(server));
  }
  return servers;
}

MockChannelOptsTest::MockChannelOptsTest(int count,
                                         int family,
                                         bool force_tcp,
                                         struct ares_options* givenopts,
                                         int optmask)
  : servers_(BuildServers(count, family, mock_port)),
    server_(*servers_[0].get()), channel_(nullptr) {
  // Set up channel options.
  struct ares_options opts;
  if (givenopts) {
    memcpy(&opts, givenopts, sizeof(opts));
  } else {
    memset(&opts, 0, sizeof(opts));
  }

  // Point the library at the first mock server by default (overridden below).
  opts.udp_port = server_.udpport();
  optmask |= ARES_OPT_UDP_PORT;
  opts.tcp_port = server_.tcpport();
  optmask |= ARES_OPT_TCP_PORT;

  // If not already overridden, set short-ish timeouts.
  if (!(optmask & (ARES_OPT_TIMEOUTMS|ARES_OPT_TIMEOUT))) {
    opts.timeout = 1500;
    optmask |= ARES_OPT_TIMEOUTMS;
  }
  // If not already overridden, set 3 retries.
  if (!(optmask & ARES_OPT_TRIES)) {
    opts.tries = 3;
    optmask |= ARES_OPT_TRIES;
  }
  // If not already overridden, set search domains.
  const char *domains[3] = {"first.com", "second.org", "third.gov"};
  if (!(optmask & ARES_OPT_DOMAINS)) {
    opts.ndomains = 3;
    opts.domains = (char**)domains;
    optmask |= ARES_OPT_DOMAINS;
  }
  if (force_tcp) {
    opts.flags |= ARES_FLAG_USEVC;
    optmask |= ARES_OPT_FLAGS;
  }

  EXPECT_EQ(ARES_SUCCESS, ares_init_options(&channel_, &opts, optmask));
  EXPECT_NE(nullptr, channel_);

  // Set up servers after construction so we can set individual ports
  struct ares_addr_port_node* prev = nullptr;
  struct ares_addr_port_node* first = nullptr;
  for (const auto& server : servers_) {
    struct ares_addr_port_node* node = (struct ares_addr_port_node*)malloc(sizeof(*node));
    if (prev) {
      prev->next = node;
    } else {
      first = node;
    }
    node->next = nullptr;
    node->family = family;
    node->udp_port = server->udpport();
    node->tcp_port = server->tcpport();
    if (family == AF_INET) {
      node->addr.addr4.s_addr = htonl(0x7F000001);
    } else {
      memset(&node->addr.addr6, 0, sizeof(node->addr.addr6));
      node->addr.addr6._S6_un._S6_u8[15] = 1;
    }
    prev = node;
  }
  EXPECT_EQ(ARES_SUCCESS, ares_set_servers_ports(channel_, first));

  while (first) {
    prev = first;
    first = first->next;
    free(prev);
  }
  if (verbose) {
    std::cerr << "Configured library with servers:";
    std::vector<std::string> servers = GetNameServers(channel_);
    for (const auto& server : servers) {
      std::cerr << " " << server;
    }
    std::cerr << std::endl;
  }
}

MockChannelOptsTest::~MockChannelOptsTest() {
  if (channel_) {
    ares_destroy(channel_);
  }
  channel_ = nullptr;
}

std::set<int> MockChannelOptsTest::fds() const {
  std::set<int> fds;
  for (const auto& server : servers_) {
    std::set<int> serverfds = server->fds();
    fds.insert(serverfds.begin(), serverfds.end());
  }
  return fds;
}

void MockChannelOptsTest::ProcessFD(int fd) {
  for (auto& server : servers_) {
    server->ProcessFD(fd);
  }
}

void MockChannelOptsTest::Process() {
  using namespace std::placeholders;
  ProcessWork(channel_,
              std::bind(&MockChannelOptsTest::fds, this),
              std::bind(&MockChannelOptsTest::ProcessFD, this, _1));
}

std::ostream& operator<<(std::ostream& os, const HostResult& result) {
  os << '{';
  if (result.done_) {
    os << StatusToString(result.status_) << " " << result.host_;
  } else {
    os << "(incomplete)";
  }
  os << '}';
  return os;
}

HostEnt::HostEnt(const struct hostent *hostent) : addrtype_(-1) {
  if (!hostent)
    return;
  if (hostent->h_name)
    name_ = hostent->h_name;
  if (hostent->h_aliases) {
    char** palias = hostent->h_aliases;
    while (*palias != nullptr) {
      aliases_.push_back(*palias);
      palias++;
    }
  }
  addrtype_ = hostent->h_addrtype;
  if (hostent->h_addr_list) {
    char** paddr = hostent->h_addr_list;
    while (*paddr != nullptr) {
      std::string addr = AddressToString(*paddr, hostent->h_length);
      addrs_.push_back(addr);
      paddr++;
    }
  }
}

std::ostream& operator<<(std::ostream& os, const HostEnt& host) {
  os << '{';
  os << "'" << host.name_ << "' "
     << "aliases=[";
  for (size_t ii = 0; ii < host.aliases_.size(); ii++) {
    if (ii > 0) os << ", ";
    os << host.aliases_[ii];
  }
  os << "] ";
  os << "addrs=[";
  for (size_t ii = 0; ii < host.addrs_.size(); ii++) {
    if (ii > 0) os << ", ";
    os << host.addrs_[ii];
  }
  os << "]";
  os << '}';
  return os;
}

void HostCallback(void *data, int status, int timeouts,
                  struct hostent *hostent) {
  EXPECT_NE(nullptr, data);
  HostResult* result = reinterpret_cast<HostResult*>(data);
  result->done_ = true;
  result->status_ = status;
  result->timeouts_ = timeouts;
  result->host_ = HostEnt(hostent);
  if (verbose) std::cerr << "HostCallback(" << *result << ")" << std::endl;
}

std::ostream& operator<<(std::ostream& os, const AddrInfoResult& result) {
  os << '{';
  if (result.done_ && result.ai_) {
    os << StatusToString(result.status_) << " " << result.ai_;
  } else {
    os << "(incomplete)";
  }
  os << '}';
  return os;
}

std::ostream& operator<<(std::ostream& os, const AddrInfo& ai) {
  os << '{';
  if (ai == nullptr) {
    os << "nullptr}";
    return os;
  }

  struct ares_addrinfo_cname *next_cname = ai->cnames;
  while(next_cname) {
    if(next_cname->alias) {
      os << next_cname->alias << "->";
    }
    if(next_cname->name) {
      os << next_cname->name;
    }
    if((next_cname = next_cname->next))
      os << ", ";
    else
      os << " ";
  }

  struct ares_addrinfo_node *next = ai->nodes;
  while(next) {
    //if(next->ai_canonname) {
      //os << "'" << next->ai_canonname << "' ";
    //}
    unsigned short port = 0;
    os << "addr=[";
    if(next->ai_family == AF_INET) {
      sockaddr_in* sin = (sockaddr_in*)next->ai_addr;
      port = ntohs(sin->sin_port);
      os << AddressToString(&sin->sin_addr, 4);
    }
    else if (next->ai_family == AF_INET6) {
      sockaddr_in6* sin = (sockaddr_in6*)next->ai_addr;
      port = ntohs(sin->sin6_port);
      os << "[" << AddressToString(&sin->sin6_addr, 16) << "]";
    }
    else
      os << "unknown family";
    if(port) {
      os << ":" << port;
    }
    os << "]";
    if((next = next->ai_next))
      os << ", ";
  }
  os << '}';
  return os;
}

void AddrInfoCallback(void *data, int status, int timeouts,
                      struct ares_addrinfo *ai) {
  EXPECT_NE(nullptr, data);
  AddrInfoResult* result = reinterpret_cast<AddrInfoResult*>(data);
  result->done_ = true;
  result->status_ = status;
  result->timeouts_= timeouts;
  result->ai_ = AddrInfo(ai);
  if (verbose) std::cerr << "AddrInfoCallback(" << *result << ")" << std::endl;
}

std::ostream& operator<<(std::ostream& os, const SearchResult& result) {
  os << '{';
  if (result.done_) {
    os << StatusToString(result.status_) << " " << PacketToString(result.data_);
  } else {
    os << "(incomplete)";
  }
  os << '}';
  return os;
}

void SearchCallback(void *data, int status, int timeouts,
                    unsigned char *abuf, int alen) {
  EXPECT_NE(nullptr, data);
  SearchResult* result = reinterpret_cast<SearchResult*>(data);
  result->done_ = true;
  result->status_ = status;
  result->timeouts_ = timeouts;
  result->data_.assign(abuf, abuf + alen);
  if (verbose) std::cerr << "SearchCallback(" << *result << ")" << std::endl;
}

std::ostream& operator<<(std::ostream& os, const NameInfoResult& result) {
  os << '{';
  if (result.done_) {
    os << StatusToString(result.status_) << " " << result.node_ << " " << result.service_;
  } else {
    os << "(incomplete)";
  }
  os << '}';
  return os;
}

void NameInfoCallback(void *data, int status, int timeouts,
                      char *node, char *service) {
  EXPECT_NE(nullptr, data);
  NameInfoResult* result = reinterpret_cast<NameInfoResult*>(data);
  result->done_ = true;
  result->status_ = status;
  result->timeouts_ = timeouts;
  result->node_ = std::string(node ? node : "");
  result->service_ = std::string(service ? service : "");
  if (verbose) std::cerr << "NameInfoCallback(" << *result << ")" << std::endl;
}

std::vector<std::string> GetNameServers(ares_channel channel) {
  struct ares_addr_port_node* servers = nullptr;
  EXPECT_EQ(ARES_SUCCESS, ares_get_servers_ports(channel, &servers));
  struct ares_addr_port_node* server = servers;
  std::vector<std::string> results;
  while (server) {
    std::stringstream ss;
    switch (server->family) {
    case AF_INET:
      ss << AddressToString((char*)&server->addr.addr4, 4);
      break;
    case AF_INET6:
      if (server->udp_port != 0) {
        ss << '[';
      }
      ss << AddressToString((char*)&server->addr.addr6, 16);
      if (server->udp_port != 0) {
        ss << ']';
      }
      break;
    default:
      results.push_back("<unknown family>");
      break;
    }
    if (server->udp_port != 0) {
      ss << ":" << server->udp_port;
    }
    results.push_back(ss.str());
    server = server->next;
  }
  if (servers) ares_free_data(servers);
  return results;
}

TransientDir::TransientDir(const std::string& dirname) : dirname_(dirname) {
  if (mkdir_(dirname_.c_str(), 0755) != 0) {
    std::cerr << "Failed to create subdirectory '" << dirname_ << "'" << std::endl;
  }
}

TransientDir::~TransientDir() {
  rmdir(dirname_.c_str());
}

TransientFile::TransientFile(const std::string& filename,
                             const std::string& contents)
    : filename_(filename) {
  FILE *f = fopen(filename.c_str(), "w");
  if (f == nullptr) {
    std::cerr << "Error: failed to create '" << filename << "'" << std::endl;
    return;
  }
  int rc = fwrite(contents.data(), 1, contents.size(), f);
  if (rc != (int)contents.size()) {
    std::cerr << "Error: failed to write contents of '" << filename << "'" << std::endl;
  }
  fclose(f);
}

TransientFile::~TransientFile() {
  unlink(filename_.c_str());
}

std::string TempNam(const char *dir, const char *prefix) {
  char *p = tempnam(dir, prefix);
  std::string result(p);
  free(p);
  return result;
}

TempFile::TempFile(const std::string& contents)
  : TransientFile(TempNam(nullptr, "ares"), contents) {

}

VirtualizeIO::VirtualizeIO(ares_channel c)
  : channel_(c)
{
  ares_set_socket_functions(channel_, &default_functions, 0);
}

VirtualizeIO::~VirtualizeIO() {
  ares_set_socket_functions(channel_, 0, 0);
}

}  // namespace test
}  // namespace ares
