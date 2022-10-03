// -*- mode: c++ -*-
#ifndef ARES_TEST_H
#define ARES_TEST_H

#include "ares_setup.h"
#include "ares.h"

#include "dns-proto.h"
// Include ares internal file for DNS protocol constants
#include "ares_nameser.h"

#include "gtest/gtest.h"
#include "gmock/gmock.h"

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#if defined(HAVE_USER_NAMESPACE) && defined(HAVE_UTS_NAMESPACE)
#define HAVE_CONTAINER
#endif

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace ares {

typedef unsigned char byte;

namespace test {

extern bool verbose;
extern int mock_port;
extern const std::vector<int> both_families;
extern const std::vector<int> ipv4_family;
extern const std::vector<int> ipv6_family;

extern const std::vector<std::pair<int, bool>> both_families_both_modes;
extern const std::vector<std::pair<int, bool>> ipv4_family_both_modes;
extern const std::vector<std::pair<int, bool>> ipv6_family_both_modes;

// Which parameters to use in tests
extern std::vector<int> families;
extern std::vector<std::pair<int, bool>> families_modes;

// Process all pending work on ares-owned file descriptors, plus
// optionally the given set-of-FDs + work function.
void ProcessWork(ares_channel channel,
                 std::function<std::set<int>()> get_extrafds,
                 std::function<void(int)> process_extra);
std::set<int> NoExtraFDs();

// Test fixture that ensures library initialization, and allows
// memory allocations to be failed.
class LibraryTest : public ::testing::Test {
 public:
  LibraryTest() {
    EXPECT_EQ(ARES_SUCCESS,
              ares_library_init_mem(ARES_LIB_INIT_ALL,
                                    &LibraryTest::amalloc,
                                    &LibraryTest::afree,
                                    &LibraryTest::arealloc));
  }
  ~LibraryTest() {
    ares_library_cleanup();
    ClearFails();
  }
  // Set the n-th malloc call (of any size) from the library to fail.
  // (nth == 1 means the next call)
  static void SetAllocFail(int nth);
  // Set the next malloc call for the given size to fail.
  static void SetAllocSizeFail(size_t size);
  // Remove any pending alloc failures.
  static void ClearFails();

  static void *amalloc(size_t size);
  static void* arealloc(void *ptr, size_t size);
  static void afree(void *ptr);
 private:
  static bool ShouldAllocFail(size_t size);
  static unsigned long long fails_;
  static std::map<size_t, int> size_fails_;
};

// Test fixture that uses a default channel.
class DefaultChannelTest : public LibraryTest {
 public:
  DefaultChannelTest() : channel_(nullptr) {
    EXPECT_EQ(ARES_SUCCESS, ares_init(&channel_));
    EXPECT_NE(nullptr, channel_);
  }

  ~DefaultChannelTest() {
    ares_destroy(channel_);
    channel_ = nullptr;
  }

  // Process all pending work on ares-owned file descriptors.
  void Process();

 protected:
  ares_channel channel_;
};

// Test fixture that uses a default channel with the specified lookup mode.
class DefaultChannelModeTest
    : public LibraryTest,
      public ::testing::WithParamInterface<std::string> {
 public:
  DefaultChannelModeTest() : channel_(nullptr) {
    struct ares_options opts = {0};
    opts.lookups = strdup(GetParam().c_str());
    int optmask = ARES_OPT_LOOKUPS;
    EXPECT_EQ(ARES_SUCCESS, ares_init_options(&channel_, &opts, optmask));
    EXPECT_NE(nullptr, channel_);
    free(opts.lookups);
  }

  ~DefaultChannelModeTest() {
    ares_destroy(channel_);
    channel_ = nullptr;
  }

  // Process all pending work on ares-owned file descriptors.
  void Process();

 protected:
  ares_channel channel_;
};

// Mock DNS server to allow responses to be scripted by tests.
class MockServer {
 public:
  MockServer(int family, int port);
  ~MockServer();

  // Mock method indicating the processing of a particular <name, RRtype>
  // request.
  MOCK_METHOD2(OnRequest, void(const std::string& name, int rrtype));

  // Set the reply to be sent next; the query ID field will be overwritten
  // with the value from the request.
  void SetReplyData(const std::vector<byte>& reply) { reply_ = reply; }
  void SetReply(const DNSPacket* reply) { SetReplyData(reply->data()); }
  void SetReplyQID(int qid) { qid_ = qid; }

  // The set of file descriptors that the server handles.
  std::set<int> fds() const;

  // Process activity on a file descriptor.
  void ProcessFD(int fd);

  // Ports the server is responding to
  int udpport() const { return udpport_; }
  int tcpport() const { return tcpport_; }

 private:
  void ProcessRequest(int fd, struct sockaddr_storage* addr, int addrlen,
                      int qid, const std::string& name, int rrtype);

  int udpport_;
  int tcpport_;
  int udpfd_;
  int tcpfd_;
  std::set<int> connfds_;
  std::vector<byte> reply_;
  int qid_;
};

// Test fixture that uses a mock DNS server.
class MockChannelOptsTest : public LibraryTest {
 public:
  MockChannelOptsTest(int count, int family, bool force_tcp, struct ares_options* givenopts, int optmask);
  ~MockChannelOptsTest();

  // Process all pending work on ares-owned and mock-server-owned file descriptors.
  void Process();

 protected:
  // NiceMockServer doesn't complain about uninteresting calls.
  typedef testing::NiceMock<MockServer> NiceMockServer;
  typedef std::vector< std::unique_ptr<NiceMockServer> > NiceMockServers;

  std::set<int> fds() const;
  void ProcessFD(int fd);

  static NiceMockServers BuildServers(int count, int family, int base_port);

  NiceMockServers servers_;
  // Convenience reference to first server.
  NiceMockServer& server_;
  ares_channel channel_;
};

class MockChannelTest
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface< std::pair<int, bool> > {
 public:
  MockChannelTest() : MockChannelOptsTest(1, GetParam().first, GetParam().second, nullptr, 0) {}
};

class MockUDPChannelTest
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface<int> {
 public:
  MockUDPChannelTest() : MockChannelOptsTest(1, GetParam(), false, nullptr, 0) {}
};

class MockTCPChannelTest
    : public MockChannelOptsTest,
      public ::testing::WithParamInterface<int> {
 public:
  MockTCPChannelTest() : MockChannelOptsTest(1, GetParam(), true, nullptr, 0) {}
};

// gMock action to set the reply for a mock server.
ACTION_P2(SetReplyData, mockserver, data) {
  mockserver->SetReplyData(data);
}
ACTION_P2(SetReply, mockserver, reply) {
  mockserver->SetReply(reply);
}
ACTION_P2(SetReplyQID, mockserver, qid) {
  mockserver->SetReplyQID(qid);
}
// gMock action to cancel a channel.
ACTION_P2(CancelChannel, mockserver, channel) {
  ares_cancel(channel);
}

// C++ wrapper for struct hostent.
struct HostEnt {
  HostEnt() : addrtype_(-1) {}
  HostEnt(const struct hostent* hostent);
  std::string name_;
  std::vector<std::string> aliases_;
  int addrtype_;  // AF_INET or AF_INET6
  std::vector<std::string> addrs_;
};
std::ostream& operator<<(std::ostream& os, const HostEnt& result);

// Structure that describes the result of an ares_host_callback invocation.
struct HostResult {
  // Whether the callback has been invoked.
  bool done_;
  // Explicitly provided result information.
  int status_;
  int timeouts_;
  // Contents of the hostent structure, if provided.
  HostEnt host_;
};
std::ostream& operator<<(std::ostream& os, const HostResult& result);

// Structure that describes the result of an ares_callback invocation.
struct SearchResult {
  // Whether the callback has been invoked.
  bool done_;
  // Explicitly provided result information.
  int status_;
  int timeouts_;
  std::vector<byte> data_;
};
std::ostream& operator<<(std::ostream& os, const SearchResult& result);

// Structure that describes the result of an ares_nameinfo_callback invocation.
struct NameInfoResult {
  // Whether the callback has been invoked.
  bool done_;
  // Explicitly provided result information.
  int status_;
  int timeouts_;
  std::string node_;
  std::string service_;
};
std::ostream& operator<<(std::ostream& os, const NameInfoResult& result);

struct AddrInfoDeleter {
  void operator() (ares_addrinfo *ptr) {
    if (ptr) ares_freeaddrinfo(ptr);
  }
};

// C++ wrapper for struct ares_addrinfo.
using AddrInfo = std::unique_ptr<ares_addrinfo, AddrInfoDeleter>;

std::ostream& operator<<(std::ostream& os, const AddrInfo& result);

// Structure that describes the result of an ares_addrinfo_callback invocation.
struct AddrInfoResult {
  AddrInfoResult() : done_(false), status_(-1), timeouts_(0) {}
  // Whether the callback has been invoked.
  bool done_;
  // Explicitly provided result information.
  int status_;
  int timeouts_;
  // Contents of the ares_addrinfo structure, if provided.
  AddrInfo ai_;
};
std::ostream& operator<<(std::ostream& os, const AddrInfoResult& result);

// Standard implementation of ares callbacks that fill out the corresponding
// structures.
void HostCallback(void *data, int status, int timeouts,
                  struct hostent *hostent);
void SearchCallback(void *data, int status, int timeouts,
                    unsigned char *abuf, int alen);
void NameInfoCallback(void *data, int status, int timeouts,
                      char *node, char *service);
void AddrInfoCallback(void *data, int status, int timeouts,
                      struct ares_addrinfo *res);

// Retrieve the name servers used by a channel.
std::vector<std::string> GetNameServers(ares_channel channel);


// RAII class to temporarily create a directory of a given name.
class TransientDir {
 public:
  TransientDir(const std::string& dirname);
  ~TransientDir();

 private:
  std::string dirname_;
};

// C++ wrapper around tempnam()
std::string TempNam(const char *dir, const char *prefix);

// RAII class to temporarily create file of a given name and contents.
class TransientFile {
 public:
  TransientFile(const std::string &filename, const std::string &contents);
  ~TransientFile();

 protected:
  std::string filename_;
};

// RAII class for a temporary file with the given contents.
class TempFile : public TransientFile {
 public:
  TempFile(const std::string& contents);
  const char* filename() const { return filename_.c_str(); }
};

#ifdef _WIN32
extern "C" {

static int setenv(const char *name, const char *value, int overwrite)
{
  char  *buffer;
  size_t buf_size;

  if (name == NULL)
    return -1;

  if (value == NULL)
    value = ""; /* For unset */

  if (!overwrite && getenv(name) != NULL) {
    return -1;
  }

  buf_size = strlen(name) + strlen(value) + 1 /* = */ + 1 /* NULL */;
  buffer   = (char *)malloc(buf_size);
  _snprintf(buffer, buf_size, "%s=%s", name, value);
  _putenv(buffer);
  free(buffer);
  return 0;
}

static int unsetenv(const char *name)
{
  return setenv(name, NULL, 1);
}

} /* extern "C" */
#endif

// RAII class for a temporary environment variable value.
class EnvValue {
 public:
  EnvValue(const char *name, const char *value) : name_(name), restore_(false) {
    char *original = getenv(name);
    if (original) {
      restore_ = true;
      original_ = original;
    }
    setenv(name_.c_str(), value, 1);
  }
  ~EnvValue() {
    if (restore_) {
      setenv(name_.c_str(), original_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }
 private:
  std::string name_;
  bool restore_;
  std::string original_;
};


#ifdef HAVE_CONTAINER
// Linux-specific functionality for running code in a container, implemented
// in ares-test-ns.cc
typedef std::function<int(void)> VoidToIntFn;
typedef std::vector<std::pair<std::string, std::string>> NameContentList;

class ContainerFilesystem {
 public:
  ContainerFilesystem(NameContentList files, const std::string& mountpt);
  ~ContainerFilesystem();
  std::string root() const { return rootdir_; };
  std::string mountpt() const { return mountpt_; };
 private:
  void EnsureDirExists(const std::string& dir);
  std::string rootdir_;
  std::string mountpt_;
  std::list<std::string> dirs_;
  std::vector<std::unique_ptr<TransientFile>> files_;
};

int RunInContainer(ContainerFilesystem* fs, const std::string& hostname,
                   const std::string& domainname, VoidToIntFn fn);

#define ICLASS_NAME(casename, testname) Contained##casename##_##testname
#define CONTAINED_TEST_F(casename, testname, hostname, domainname, files)       \
  class ICLASS_NAME(casename, testname) : public casename {                     \
   public:                                                                      \
    ICLASS_NAME(casename, testname)() {}                                        \
    static int InnerTestBody();                                                 \
  };                                                                            \
  TEST_F(ICLASS_NAME(casename, testname), _) {                                  \
    ContainerFilesystem chroot(files, "..");                                    \
    VoidToIntFn fn(ICLASS_NAME(casename, testname)::InnerTestBody);             \
    EXPECT_EQ(0, RunInContainer(&chroot, hostname, domainname, fn));            \
  }                                                                             \
  int ICLASS_NAME(casename, testname)::InnerTestBody()

#endif

/* Assigns virtual IO functions to a channel. These functions simply call
 * the actual system functions.
 */
class VirtualizeIO {
public:
  VirtualizeIO(ares_channel);
  ~VirtualizeIO();

  static const ares_socket_functions default_functions;
private:
  ares_channel channel_;
};

/*
 * Slightly white-box macro to generate two runs for a given test case:
 * One with no modifications, and one with all IO functions set to use
 * the virtual io structure.
 * Since no magic socket setup or anything is done in the latter case
 * this should probably only be used for test with very vanilla IO
 * requirements.
 */
#define VCLASS_NAME(casename, testname) Virt##casename##_##testname
#define VIRT_NONVIRT_TEST_F(casename, testname)                                 \
  class VCLASS_NAME(casename, testname) : public casename {                     \
  public:                                                                       \
    VCLASS_NAME(casename, testname)() {}                                        \
    void InnerTestBody();                                                       \
  };                                                                            \
  GTEST_TEST_(casename, testname, VCLASS_NAME(casename, testname),              \
              ::testing::internal::GetTypeId<casename>()) {                     \
    InnerTestBody();                                                            \
  }                                                                             \
  GTEST_TEST_(casename, testname##_virtualized,                                 \
              VCLASS_NAME(casename, testname),                                  \
              ::testing::internal::GetTypeId<casename>()) {                     \
    VirtualizeIO vio(channel_);                                                 \
    InnerTestBody();                                                            \
  }                                                                             \
  void VCLASS_NAME(casename, testname)::InnerTestBody()

}  // namespace test
}  // namespace ares

#endif
