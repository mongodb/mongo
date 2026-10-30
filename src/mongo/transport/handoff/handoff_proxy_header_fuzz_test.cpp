// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/initializer.h"
#include "mongo/transport/handoff/handoff_session.h"
#include "mongo/transport/handoff/handoff_test_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/scopeguard.h"

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include "fuzztest/fuzztest.h"
#include "gtest/gtest.h"
#include <sys/socket.h>

namespace mongo::transport {
namespace {

/**
 * FuzzTest's gtest main does not run MongoDB's MONGO_INITIALIZERs, and does not ignore SIGPIPE (as
 * mongod/s do). Register a gtest Environment so both are set up before any test body.
 */
class MongoInfrastructureEnv : public ::testing::Environment {
public:
    void SetUp() override {
        runGlobalInitializersOrDie({});
        signal(SIGPIPE, SIG_IGN);
    }
};

[[maybe_unused]] const auto kRegisterMongoInfraEnv =
    ::testing::AddGlobalTestEnvironment(new MongoInfrastructureEnv);

/** Widens header bytes to the fuzzer's int-typed input. */
std::vector<int> headerBytes(const std::vector<uint8_t>& bytes) {
    return std::vector<int>(bytes.begin(), bytes.end());
}

/** A valid v2 header whose source address block is built from `sourceAddr`. */
struct ValidHeaderSpec {
    std::vector<int> sourceAddr;
};

/** A raw header whose bytes are sent verbatim. */
struct RawHeaderSpec {
    std::vector<int> raw;
};

/**
 * A fuzz input for the PROXY header read, as one of two shapes. The variant keeps the domain from
 * generating the field that the selected shape does not use.
 */
using ProxyHeaderSpec = std::variant<ValidHeaderSpec, RawHeaderSpec>;

/**
 * Builds a valid v2 header whose source address block carries `sourceAddr` (4 IPv4 bytes + 2
 * big-endian port bytes), so the fuzzer can vary the parsed source endpoint.
 */
std::vector<int> buildValidHeader(const std::vector<int>& sourceAddr) {
    auto header = headerBytes(buildProxyHeader(nullptr, nullptr, nullptr, 0));
    // The address block follows the 16-byte fixed header: 4 source-IPv4 bytes, 4 destination-IPv4
    // bytes, 2 big-endian source-port bytes, 2 destination-port bytes.
    for (int i = 0; i < 4; ++i) {
        header[16 + i] = sourceAddr[i];
    }
    header[24] = sourceAddr[4];
    header[25] = sourceAddr[5];
    return header;
}

/**
 * Builds seed PROXY v2 header cases. The v2 signature is a magic value that mutation alone will
 * not produce, so without structurally valid seeds the fuzzer only reaches the early-rejection
 * paths.
 */
std::vector<ProxyHeaderSpec> buildProxyHeaderSeeds() {
    // DER-encoded role data for a single role: role="role_name", db="Third field". This was taken
    // from proxy_protocol_tlv_ssl_peer_info_test.cpp.
    const std::vector<uint8_t> roleBytes = {
        0x31, 0x1a, 0x30, 0x18, 0x0c, 0x09, 0x72, 0x6f, 0x6c, 0x65, 0x5f, 0x6e, 0x61, 0x6d,
        0x65, 0x0c, 0x0b, 0x54, 0x68, 0x69, 0x72, 0x64, 0x20, 0x66, 0x69, 0x65, 0x6c, 0x64};

    // The test source address, 127.0.0.1:12345 (kTestClientAddr:kTestClientPort), as 4 IPv4 bytes
    // followed by 2 big-endian port bytes.
    const std::vector<int> testSourceAddr = {
        127, 0, 0, 1, (kTestClientPort >> 8) & 0xFF, kTestClientPort & 0xFF};

    // A valid v2 header with no optional TLVs. The builder still appends a NOOP TLV, since the
    // parser requires a non-empty TLV block.
    const std::vector<int> minimalValidHeader =
        headerBytes(buildProxyHeader(nullptr, nullptr, nullptr, 0));

    // The same header with the declared length set to 0xFFFF, which exceeds the maximum allowed.
    std::vector<int> oversizedAddrLenHeader = minimalValidHeader;
    oversizedAddrLenHeader[14] = 0xFF;
    oversizedAddrLenHeader[15] = 0xFF;

    // The first 10 bytes of a valid header, so the reader hits EOF during the fixed-header read.
    const std::vector<int> truncatedHeader(minimalValidHeader.begin(),
                                           minimalValidHeader.begin() + 10);

    return {
        // Valid. Exercises the success path: the source endpoint must round-trip.
        ValidHeaderSpec{.sourceAddr = testSourceAddr},
        // Invalid. Empty input, which tests immediate EOF.
        RawHeaderSpec{},
        // Valid, sent as raw bytes: exercises optional TLV parsing.
        RawHeaderSpec{.raw = headerBytes(buildProxyHeader(kTestSniName, nullptr, nullptr, 0))},
        // Valid, sent as raw bytes: exercises nested SSL TLV parsing.
        RawHeaderSpec{.raw = headerBytes(buildProxyHeader(
                          nullptr, "CN=test.example.com", roleBytes.data(), roleBytes.size()))},
        // Invalid. A declared length above the maximum, which exercises size limit validation.
        RawHeaderSpec{.raw = std::move(oversizedAddrLenHeader)},
        // Invalid. A truncated header, which exercises EOF during the fixed-header read.
        RawHeaderSpec{.raw = truncatedHeader},
    };
}

/**
 * Builds the fuzztest domain for ProxyHeaderSpec, seeded with the cases above.
 */
auto buildProxyHeaderDomain() {
    using namespace fuzztest;

    return VariantOf<ProxyHeaderSpec>(
               StructOf<ValidHeaderSpec>(VectorOf(InRange(0, 255)).WithMaxSize(6)),
               StructOf<RawHeaderSpec>(VectorOf(InRange(0, 255)).WithMaxSize(512)))
        .WithSeeds(buildProxyHeaderSeeds);
}

/**
 * Fuzzes the PROXY v2 header that HandoffSession::prelude() reads before any wire message.
 *
 * The fuzzed input is a ProxyHeaderSpec that selects one of two shapes.
 * - A header with valid v2 framing, built by buildProxyHeader() with the source address block
 *   overwritten from fuzzed bytes, so the parsed source endpoint varies.
 * - A header with arbitrary raw bytes, sent verbatim, which reaches the parser's peek, length, and
 *   TLV stages without passing through the v2 magic gate.
 *
 * The fuzzer feeds bytes and then half-closes the proxy end, so a short header is seen as EOF
 * instead of blocking until the read timeout.
 *
 * The assertions are as follows.
 * - A header with valid v2 framing is accepted and populates the source endpoint from its address
 *   block.
 * - A header with arbitrary bytes is either parsed or rejected with a Status and never faults,
 *   over-reads, or hangs.
 * - No file descriptors leak across the session's lifetime.
 *
 * The v2 signature is a fixed magic value that random bytes and mutation will not produce, so the
 * domain is seeded with valid, truncated, and oversized-length headers. Without those seeds, the
 * fuzzer only reaches the early-rejection paths.
 */
class ProxyHeaderFuzz {
public:
    void ProxyHeaderParsing(ProxyHeaderSpec spec) {
        const auto numFdsBefore = numberOfOpenFileDescriptors();
        {
            int fds[2];
            ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0)
                << "socketpair failed: " << errorMessage(lastSocketError());
            // The session takes ownership of fds[0] and closes it in its destructor.
            ScopeGuard closeProxy([&] { ::close(fds[1]); });

            MockPOSIXInterface posix;
            auto session = std::make_shared<HandoffSession>(
                HandoffSession::Params{.fd = fds[0],
                                       .localAddress = std::filesystem::path("/dummy/mongod"),
                                       .transportLayer = nullptr,
                                       .acceptedOnLoadBalancedSocket = false,
                                       .acceptedOnPrioritySocket = false,
                                       .s2nConfig = nullptr,
                                       .posix = posix});

            const ValidHeaderSpec* validSpec = std::get_if<ValidHeaderSpec>(&spec);
            std::vector<int> sourceAddr(6, 0);
            std::vector<int> bytes;
            if (validSpec) {
                // The address block is a fixed 6 bytes (4 IPv4 + 2 big-endian port). Zero-pad short
                // input, so the domain also accepts the fuzzer's empty default seed.
                std::copy_n(validSpec->sourceAddr.begin(),
                            std::min(validSpec->sourceAddr.size(), sourceAddr.size()),
                            sourceAddr.begin());
                bytes = buildValidHeader(sourceAddr);
            } else {
                bytes = std::get<RawHeaderSpec>(spec).raw;
            }

            // Write the bytes, then half-close so a short header is seen as EOF.
            const std::string payload(bytes.begin(), bytes.end());
            writeAll(fds[1], payload.data(), payload.size());
            ::shutdown(fds[1], SHUT_WR);

            if (validSpec) {
                // A valid v2 header must be accepted, and its address block must match
                // the proxied source endpoint's address and port.
                ASSERT_NO_THROW(session->prelude());
                const auto& endpoint = session->getSourceRemoteEndpoint();
                ASSERT_EQ(
                    endpoint.host(),
                    fmt::format(
                        "{}.{}.{}.{}", sourceAddr[0], sourceAddr[1], sourceAddr[2], sourceAddr[3]));
                ASSERT_EQ(endpoint.port(), (sourceAddr[4] << 8) | sourceAddr[5]);
            } else {
                // Arbitrary bytes must either parse as a valid v2 header or fail with a Status.
                // Never fault, over-read, or hang. We do not assert the exact error code, since for
                // arbitrary input there is no ground-truth code and which check fires depends on
                // the input's shape.
                try {
                    session->prelude();
                } catch (const DBException&) {
                    // Expected for malformed headers.
                }
            }
        }
        // The session and socketpair are gone. No fds should have leaked.
        ASSERT_EQ(numberOfOpenFileDescriptors(), numFdsBefore);
    }
};

FUZZ_TEST_F(ProxyHeaderFuzz, ProxyHeaderParsing).WithDomains(buildProxyHeaderDomain());

}  // namespace
}  // namespace mongo::transport
