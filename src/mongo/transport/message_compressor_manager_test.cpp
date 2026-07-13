// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/transport/message_compressor_manager.h"

#include "mongo/base/data_range.h"
#include "mongo/base/data_range_cursor.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_compressed.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/transport/message_compressor_zlib.h"
#include "mongo/transport/message_compressor_zstd.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/shared_buffer.h"
#include "mongo/util/str.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
using namespace std::literals::string_view_literals;

const auto assertOk = [](auto&& sw) {
    ASSERT(sw.isOK());
    return sw.getValue();
};

MessageCompressorRegistry buildRegistry() {
    MessageCompressorRegistry ret;
    auto compressor = std::make_unique<NoopMessageCompressor>();

    std::vector<std::string> compressorList = {compressor->getName()};
    ret.setSupportedCompressors(std::move(compressorList));
    ret.registerImplementation(std::move(compressor));
    ret.finalizeSupportedCompressors().transitional_ignore();

    return ret;
}

void checkNegotiationResult(const BSONObj& result, const std::vector<std::string>& algos) {
    auto compressorsList = result.getField("compression");
    if (algos.empty()) {
        ASSERT_TRUE(compressorsList.eoo());
        return;
    }
    ASSERT_TRUE(!compressorsList.eoo());
    ASSERT_TRUE(compressorsList.isABSONObj());
    auto compressorsListObj = compressorsList.Obj();

    std::vector<std::string> resultAlgos;
    for (const auto& e : compressorsListObj) {
        resultAlgos.push_back(std::string{e.checkAndGetStringData()});
    }
    ASSERT_EQ(algos.size(), resultAlgos.size());
    for (size_t i = 0; i < algos.size(); i++) {
        ASSERT_EQ(algos[i], resultAlgos[i]);
    }
}

void checkServerNegotiation(const boost::optional<std::vector<std::string_view>>& input,
                            const std::vector<std::string>& expected) {
    auto registry = buildRegistry();
    MessageCompressorManager manager(&registry);

    BSONObjBuilder serverOutput;
    manager.serverNegotiate(input, &serverOutput);
    checkNegotiationResult(serverOutput.done(), expected);
}

void checkFidelity(const Message& msg, std::unique_ptr<MessageCompressorBase> compressor) {
    MessageCompressorRegistry registry;
    const auto originalView = msg.singleData();
    const auto compressorName = compressor->getName();

    std::vector<std::string> compressorList = {compressorName};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    registry.finalizeSupportedCompressors().transitional_ignore();

    MessageCompressorManager mgr(&registry);
    BSONObjBuilder negotiatorOut;
    std::vector<std::string_view> negotiator({compressorName});
    mgr.serverNegotiate(negotiator, &negotiatorOut);
    checkNegotiationResult(negotiatorOut.done(), {compressorName});

    auto swm = mgr.compressMessage(msg);
    ASSERT_OK(swm.getStatus());
    auto compressedMsg = std::move(swm.getValue());
    const auto compressedMsgView = compressedMsg.singleData();

    ASSERT_EQ(compressedMsgView.getId(), originalView.getId());
    ASSERT_EQ(compressedMsgView.getResponseToMsgId(), originalView.getResponseToMsgId());
    ASSERT_EQ(compressedMsgView.getNetworkOp(), dbCompressed);

    swm = mgr.decompressMessage(compressedMsg);
    ASSERT_OK(swm.getStatus());
    auto decompressedMsg = std::move(swm.getValue());

    const auto decompressedMsgView = decompressedMsg.singleData();
    ASSERT_EQ(decompressedMsgView.getId(), originalView.getId());
    ASSERT_EQ(decompressedMsgView.getResponseToMsgId(), originalView.getResponseToMsgId());
    ASSERT_EQ(decompressedMsgView.getNetworkOp(), originalView.getNetworkOp());
    ASSERT_EQ(decompressedMsgView.getLen(), originalView.getLen());

    ASSERT_EQ(memcmp(decompressedMsgView.data(), originalView.data(), originalView.dataLen()), 0);
}

void checkOverflow(std::unique_ptr<MessageCompressorBase> compressor) {
    // This is our test data that we're going to try to compress/decompress into a buffer that's
    // way too small.
    const std::string data =
        "We embrace reality. We apply high-quality thinking and rigor."
        "We have courage in our convictions but work hard to ensure biases "
        "or personal beliefs do not get in the way of finding the best solution.";
    ConstDataRange input(data.data(), data.size());

    // This is our tiny buffer that should cause an error.
    std::array<char, 16> smallBuffer;
    DataRange smallOutput(smallBuffer.data(), smallBuffer.size());

    // This is a normal sized buffer that we can store a compressed version of our test data safely
    std::vector<char> normalBuffer;
    normalBuffer.resize(compressor->getMaxCompressedSize(data.size()));
    auto sws = compressor->compressData(input, DataRange(normalBuffer.data(), normalBuffer.size()));
    ASSERT_OK(sws);
    DataRange normalRange = DataRange(normalBuffer.data(), sws.getValue());

    // Check that compressing the test data into a small buffer fails
    ASSERT_NOT_OK(compressor->compressData(input, smallOutput));

    // Check that decompressing compressed test data into a small buffer fails
    ASSERT_NOT_OK(compressor->decompressData(normalRange, smallOutput));

    // Check that decompressing a valid buffer that's missing data doesn't overflow the
    // source buffer.
    std::vector<char> scratch;
    scratch.resize(data.size());
    ConstDataRange tooSmallRange(normalBuffer.data(), normalBuffer.size() / 2);
    ASSERT_NOT_OK(
        compressor->decompressData(tooSmallRange, DataRange(scratch.data(), scratch.size())));
}

void checkUndersize(const Message& compressedMsg,
                    std::unique_ptr<MessageCompressorBase> compressor) {
    MessageCompressorRegistry registry;
    const auto compressorName = compressor->getName();

    std::vector<std::string> compressorList = {compressorName};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    registry.finalizeSupportedCompressors().transitional_ignore();

    MessageCompressorManager mgr(&registry);
    BSONObjBuilder negotiatorOut;
    std::vector<std::string_view> negotiator({compressorName});
    mgr.serverNegotiate(negotiator, &negotiatorOut);
    checkNegotiationResult(negotiatorOut.done(), {compressorName});

    auto swm = mgr.decompressMessage(compressedMsg);
    ASSERT_EQ(ErrorCodes::BadValue, swm.getStatus());
}

Message buildMessage() {
    const auto data = std::string{"Hello, world!"};
    const auto bufferSize = MsgData::MsgDataHeaderSize + data.size();
    auto buf = SharedBuffer::allocate(bufferSize);
    MsgData::View testView(buf.get());
    testView.setId(123456);
    testView.setResponseToMsgId(654321);
    testView.setOperation(dbQuery);
    testView.setLen(bufferSize);
    memcpy(testView.data(), data.data(), data.size());
    return Message{buf};
}

TEST(MessageCompressorManager, NoCompressionRequested) {
    auto input = BSON("isMaster" << 1);
    checkServerNegotiation(boost::none, {});
}

TEST(MessageCompressorManager, NormalCompressionRequested) {
    std::vector<std::string_view> input{"noop"sv};
    checkServerNegotiation(input, {"noop"});
}

TEST(MessageCompressorManager, BadCompressionRequested) {
    std::vector<std::string_view> input{"fakecompressor"sv};
    checkServerNegotiation(input, {});
}

TEST(MessageCompressorManager, BadAndGoodCompressionRequested) {
    std::vector<std::string_view> input{"fakecompressor"sv, "noop"sv};
    checkServerNegotiation(input, {"noop"});
}

// Transitional: Parse BSON "isMaster"-like docs for compressor lists.
boost::optional<std::vector<std::string_view>> parseBSON(BSONObj input) {
    auto elem = input["compression"];
    if (!elem) {
        return boost::none;
    }

    uassert(ErrorCodes::BadValue,
            str::stream() << "'compression' is not an array: " << elem,
            elem.type() == BSONType::array);

    std::vector<std::string_view> ret;
    for (const auto& e : elem.Obj()) {
        uassert(ErrorCodes::BadValue,
                str::stream() << "'compression' element is not a string: " << e,
                e.type() == BSONType::string);
        ret.push_back(e.valueStringData());
    }

    return ret;
}

TEST(MessageCompressorManager, FullNormalCompression) {
    auto registry = buildRegistry();
    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    BSONObjBuilder clientOutput;
    clientManager.clientBegin(&clientOutput);
    auto clientObj = clientOutput.done();
    checkNegotiationResult(clientObj, {"noop"});

    BSONObjBuilder serverOutput;
    serverManager.serverNegotiate(parseBSON(clientObj), &serverOutput);
    auto serverObj = serverOutput.done();
    checkNegotiationResult(serverObj, {"noop"});

    clientManager.clientFinish(serverObj);
}

// Non-replication traffic bumps only the process-wide network.compression counters; the
// replication subset counters stay at zero.
TEST(MessageCompressorManager, NonReplicationTrafficNotCountedInReplicationCompressionStats) {
    auto registry = buildRegistry();
    auto* compressor = registry.getCompressor("noop");
    ASSERT(compressor);

    MessageCompressorManager manager(&registry);
    BSONObjBuilder negotiatorOut;
    std::vector<std::string_view> negotiator({"noop"});
    manager.serverNegotiate(negotiator, &negotiatorOut);
    checkNegotiationResult(negotiatorOut.done(), {"noop"});

    auto compressed = assertOk(manager.compressMessage(buildMessage()));
    ASSERT_EQ(compressed.operation(), dbCompressed);
    ASSERT_GT(compressor->getCompressorBytesIn(), 0);
    ASSERT_EQ(compressor->getReplicationCompressorBytesIn(), 0);

    assertOk(manager.decompressMessage(compressed));
    ASSERT_GT(compressor->getDecompressorBytesIn(), 0);
    ASSERT_EQ(compressor->getReplicationDecompressorBytesIn(), 0);
}

// Replication data-plane traffic is a subset view: it bumps the replication counters AND is still
// included in the process-wide network.compression counters (repl.compression is a subset, not a
// separate accounting).
TEST(MessageCompressorManager, ReplicationTrafficCountedInBothNetworkAndReplicationStats) {
    auto registry = buildRegistry();
    auto* compressor = registry.getCompressor("noop");
    ASSERT(compressor);

    MessageCompressorManager manager(&registry);
    manager.countAsReplicationCompressionTrafficForThisSession(true);
    BSONObjBuilder negotiatorOut;
    std::vector<std::string_view> negotiator({"noop"});
    manager.serverNegotiate(negotiator, &negotiatorOut);
    checkNegotiationResult(negotiatorOut.done(), {"noop"});

    auto compressed = assertOk(manager.compressMessage(buildMessage()));
    ASSERT_EQ(compressed.operation(), dbCompressed);
    ASSERT_GT(compressor->getReplicationCompressorBytesIn(), 0);
    // Subset semantics: the same bytes are also present in the process-wide net counters, and the
    // replication portion never exceeds the process-wide total.
    ASSERT_GTE(compressor->getCompressorBytesIn(), compressor->getReplicationCompressorBytesIn());

    assertOk(manager.decompressMessage(compressed));
    ASSERT_GT(compressor->getReplicationDecompressorBytesIn(), 0);
    ASSERT_GTE(compressor->getDecompressorBytesIn(),
               compressor->getReplicationDecompressorBytesIn());
}

// A replication connection's later hello without the replicationCompressionClient marker must NOT
// clear the connection's replication accounting role: subsequent compression is still attributed to
// repl.compression. This guards the sticky-flag fix in replication_info.cpp.
TEST(MessageCompressorManager, ReplicationAccountingSurvivesLaterNonMarkerHello) {
    auto registry = buildRegistry();
    auto* compressor = registry.getCompressor("noop");
    ASSERT(compressor);

    MessageCompressorManager manager(&registry);
    manager.countAsReplicationCompressionTrafficForThisSession(true);

    std::vector<std::string_view> negotiator({"noop"});
    BSONObjBuilder initialHelloOut;
    manager.serverNegotiate(negotiator, &initialHelloOut);
    checkNegotiationResult(initialHelloOut.done(), {"noop"});

    // A later hello without replicationCompressionClient should not clear the connection's
    // replication accounting role. replication_info.cpp enforces that by not writing false for
    // non-marker hellos; this simulates that path by only re-reading the negotiated result.
    BSONObjBuilder laterHelloOut;
    manager.serverNegotiate(boost::none, &laterHelloOut);
    checkNegotiationResult(laterHelloOut.done(), {"noop"});

    auto compressed = assertOk(manager.compressMessage(buildMessage()));
    ASSERT_EQ(compressed.operation(), dbCompressed);
    ASSERT_GT(compressor->getReplicationCompressorBytesIn(), 0);
}

// When a client opts out via disableCompressionForThisSession(), clientBegin()
// must not emit a "compression" field, the server responds without one, and clientFinish()
// leaves the negotiated compressor list empty so subsequent compressMessage() calls become
// no-ops. This is the mechanism that lets the oplog fetcher run uncompressed independently of
// net.compression.compressors.
TEST(MessageCompressorManager, ClientCanSuppressCompressionOfferForThisSession) {
    auto registry = buildRegistry();
    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    ASSERT_TRUE(clientManager.isCompressionOfferedForThisSession());
    clientManager.disableCompressionForThisSession();
    ASSERT_FALSE(clientManager.isCompressionOfferedForThisSession());

    BSONObjBuilder clientOutput;
    clientManager.clientBegin(&clientOutput);
    auto clientObj = clientOutput.done();
    // No "compression" element should be emitted at all.
    ASSERT_FALSE(clientObj.hasField("compression"));

    BSONObjBuilder serverOutput;
    serverManager.serverNegotiate(parseBSON(clientObj), &serverOutput);
    auto serverObj = serverOutput.done();
    // Server has nothing to negotiate against, so it must not echo a compression list.
    ASSERT_FALSE(serverObj.hasField("compression"));

    clientManager.clientFinish(serverObj);
    ASSERT_TRUE(clientManager.getNegotiatedCompressors().empty());
    ASSERT_TRUE(serverManager.getNegotiatedCompressors().empty());
}

// Verify that re-enabling before the next clientBegin() restores normal
// advertisement. This models a replication client that first disabled compression on its
// manager and then, on a later (re)connect, re-enabled it before the next handshake ran.
TEST(MessageCompressorManager, ClientCanReenableCompressionOfferAfterSuppressing) {
    auto registry = buildRegistry();
    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    clientManager.disableCompressionForThisSession();
    {
        BSONObjBuilder out;
        clientManager.clientBegin(&out);
        ASSERT_FALSE(out.done().hasField("compression"));
    }

    clientManager.enableCompressionForThisSession();
    ASSERT_TRUE(clientManager.isCompressionOfferedForThisSession());

    BSONObjBuilder clientOutput;
    clientManager.clientBegin(&clientOutput);
    auto clientObj = clientOutput.done();
    checkNegotiationResult(clientObj, {"noop"});

    BSONObjBuilder serverOutput;
    serverManager.serverNegotiate(parseBSON(clientObj), &serverOutput);
    auto serverObj = serverOutput.done();
    checkNegotiationResult(serverObj, {"noop"});

    clientManager.clientFinish(serverObj);
    ASSERT_FALSE(clientManager.getNegotiatedCompressors().empty());
}

// An allow-list restricts advertisement to the intersection of the caller's
// list and the process-wide compressor list, preserving the caller's ordering so it becomes
// the client's negotiation preference order.
TEST(MessageCompressorManager, ClientAllowListNarrowsAdvertisedCompressors) {
    std::unique_ptr<MessageCompressorBase> zstdCompressor =
        std::make_unique<ZstdMessageCompressor>();
    std::unique_ptr<MessageCompressorBase> zlibCompressor =
        std::make_unique<ZlibMessageCompressor>();
    std::unique_ptr<MessageCompressorBase> snappyCompressor =
        std::make_unique<SnappyMessageCompressor>();

    MessageCompressorRegistry registry;
    registry.setSupportedCompressors(
        {snappyCompressor->getName(), zlibCompressor->getName(), zstdCompressor->getName()});
    registry.registerImplementation(std::move(zlibCompressor));
    registry.registerImplementation(std::move(zstdCompressor));
    registry.registerImplementation(std::move(snappyCompressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());

    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    // Allow-list is a strict subset; order given by the caller must be preserved verbatim.
    clientManager.setCompressorAllowListForThisSession({"zstd", "snappy"});

    BSONObjBuilder clientOutput;
    clientManager.clientBegin(&clientOutput);
    auto clientObj = clientOutput.done();
    checkNegotiationResult(clientObj, {"zstd", "snappy"});

    BSONObjBuilder serverOutput;
    serverManager.serverNegotiate(parseBSON(clientObj), &serverOutput);
    auto serverObj = serverOutput.done();
    // Server echoes the mutually supported compressors in client preference order.
    checkNegotiationResult(serverObj, {"zstd", "snappy"});

    clientManager.clientFinish(serverObj);
    ASSERT_FALSE(clientManager.getNegotiatedCompressors().empty());
}

// Unknown names in the allow-list are silently dropped so an operator cannot
// use the per-session hook to widen the process-wide compressor list. If every name is
// unknown, the connection is negotiated uncompressed rather than falling back to the full
// process list, so the operator gets a predictable failure mode.
TEST(MessageCompressorManager, ClientAllowListDropsUnknownNamesAndCanCollapseToDisabled) {
    auto registry = buildRegistry();  // registers only "noop"
    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    // "zstd" is not in this registry; "noop" is. Only "noop" should be advertised.
    clientManager.setCompressorAllowListForThisSession({"zstd", "noop"});
    {
        BSONObjBuilder out;
        clientManager.clientBegin(&out);
        checkNegotiationResult(out.done(), {"noop"});
    }

    // Every name unknown => nothing advertised, connection stays uncompressed.
    clientManager.setCompressorAllowListForThisSession({"zstd", "brotli"});
    {
        BSONObjBuilder out;
        clientManager.clientBegin(&out);
        ASSERT_FALSE(out.done().hasField("compression"));
    }

    // Clearing the allow-list restores full advertisement of the process-wide list.
    clientManager.setCompressorAllowListForThisSession({});
    {
        BSONObjBuilder out;
        clientManager.clientBegin(&out);
        checkNegotiationResult(out.done(), {"noop"});
    }

    // disableCompressionForThisSession() must still win over any allow-list state.
    clientManager.setCompressorAllowListForThisSession({"noop"});
    clientManager.disableCompressionForThisSession();
    {
        BSONObjBuilder out;
        clientManager.clientBegin(&out);
        ASSERT_FALSE(out.done().hasField("compression"));
    }
}

// Builds a registry that mimics `net.compression.compressors: disabled` combined
// with `replication.networkCompression.compressors: snappy`. The net set is empty, snappy is
// folded into the process-wide capability set (union) via addReplicationCompressors(), and snappy
// is registered so it can be negotiated for internal connections and decoded on any connection.
MessageCompressorRegistry buildNetDisabledReplSnappyRegistry() {
    MessageCompressorRegistry registry;
    registry.setSupportedCompressors(std::vector<std::string>{});  // net disabled
    registry.addReplicationCompressors({"snappy"});                // replication-only
    registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
    ASSERT_OK(registry.finalizeSupportedCompressors());
    return registry;
}

// An external (client-facing) negotiation uses the net set. With net disabled it is empty, so a
// client advertising a replication-only compressor must be negotiated uncompressed. This is the
// invariant that keeps net.compression.compressors: disabled meaningful for external clients even
// though the union registry now contains snappy.
TEST(MessageCompressorManager, ServerNegotiateExternalIgnoresReplicationOnlyCompressor) {
    auto registry = buildNetDisabledReplSnappyRegistry();
    MessageCompressorManager manager(&registry);

    std::vector<std::string_view> clientList{"snappy"};
    BSONObjBuilder out;
    manager.serverNegotiate(clientList, &out);  // default => external / net set (empty)
    checkNegotiationResult(out.done(), {});
}

// An internal replica-set negotiation passes the replication candidate set. Even with net
// disabled, snappy is accepted because it is both in the candidate set and registered.
TEST(MessageCompressorManager, ServerNegotiateInternalUsesReplicationCandidateSet) {
    auto registry = buildNetDisabledReplSnappyRegistry();
    MessageCompressorManager manager(&registry);

    std::vector<std::string_view> clientList{"snappy"};
    BSONObjBuilder out;
    std::vector<std::string> replCandidates{"snappy"};
    manager.serverNegotiate(clientList, &out, replCandidates);
    checkNegotiationResult(out.done(), {"snappy"});
}

// A replication connection's candidate set is sticky for the connection's lifetime. After the
// initial marker-bearing hello negotiates the replication candidate set, a later hello that
// re-advertises compression WITHOUT the replicationCompressionClient marker (so serverNegotiate()
// is called with no allow-list) must NOT fall back to the empty net.compression set and silently
// drop the replication compression. This guards the sticky-candidate-set fix in
// MessageCompressorManager::serverNegotiate().
TEST(MessageCompressorManager, ServerRenegotiateKeepsReplicationCandidateSetSticky) {
    auto registry = buildNetDisabledReplSnappyRegistry();  // net disabled, repl = snappy
    MessageCompressorManager manager(&registry);

    std::vector<std::string_view> clientList{"snappy"};
    std::vector<std::string> replCandidates{"snappy"};

    // Initial hello with the replication allow-list negotiates snappy.
    BSONObjBuilder initialOut;
    manager.serverNegotiate(clientList, &initialOut, replCandidates);
    checkNegotiationResult(initialOut.done(), {"snappy"});

    // Later hello on the same connection re-advertises compression but supplies no allow-list
    // (marker absent). Without stickiness this would use the empty net set and drop snappy.
    BSONObjBuilder laterOut;
    manager.serverNegotiate(clientList, &laterOut);
    checkNegotiationResult(laterOut.done(), {"snappy"});
}

// An empty internal candidate set (replication.networkCompression.compressors: disabled) forces
// internal connections uncompressed regardless of what the client advertises.
TEST(MessageCompressorManager, ServerNegotiateInternalDisabledForcesUncompressed) {
    auto registry = buildNetDisabledReplSnappyRegistry();
    MessageCompressorManager manager(&registry);

    std::vector<std::string_view> clientList{"snappy"};
    BSONObjBuilder out;
    std::vector<std::string> emptyCandidates{};
    manager.serverNegotiate(clientList, &out, emptyCandidates);
    checkNegotiationResult(out.done(), {});
}

// The client default path (no per-session allow-list) advertises only the net set, so a node with
// net disabled offers nothing on ordinary outbound connections even though snappy is in the union.
TEST(MessageCompressorManager, ClientBeginDefaultAdvertisesNetSetOnly) {
    auto registry = buildNetDisabledReplSnappyRegistry();
    MessageCompressorManager manager(&registry);

    BSONObjBuilder out;
    manager.clientBegin(&out);
    ASSERT_FALSE(out.done().hasField("compression"));
}

// The oplog fetcher path (per-session allow-list) intersects against the union, so a
// replication-only compressor can be advertised even when net.compression.compressors: disabled.
TEST(MessageCompressorManager, ClientBeginAllowListAdvertisesReplicationOnlyCompressor) {
    auto registry = buildNetDisabledReplSnappyRegistry();
    MessageCompressorManager manager(&registry);

    manager.setCompressorAllowListForThisSession({"snappy"});
    BSONObjBuilder out;
    manager.clientBegin(&out);
    checkNegotiationResult(out.done(), {"snappy"});
}

// Mimics `net.compression.compressors: snappy` combined with
// `replication.networkCompression.compressors: zlib`. Both algorithms are registered process-wide,
// so the union registry contains {snappy, zlib}, but the net candidate set is only {snappy} and the
// replication candidate set is only {zlib}. This is the configuration where the union registry is
// strictly larger than any single connection's candidate set.
MessageCompressorRegistry buildNetSnappyReplZlibRegistry() {
    MessageCompressorRegistry registry;
    registry.setSupportedCompressors({"snappy"});  // net
    registry.addReplicationCompressors({"zlib"});  // replication-only
    registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
    registry.registerImplementation(std::make_unique<ZlibMessageCompressor>());
    ASSERT_OK(registry.finalizeSupportedCompressors());
    return registry;
}

// Produces an OP_COMPRESSED frame compressed with 'name' using a throwaway, un-negotiated manager.
// Because that manager never ran serverNegotiate(), its permit list is unengaged and it can emit any
// registered algorithm - exactly what a peer (or a hand-crafted malicious client) could put on the
// wire.
Message compressWith(MessageCompressorRegistry* registry, const Message& msg, std::string_view name) {
    MessageCompressorManager producer(registry);
    auto id = registry->getCompressor(name)->getId();
    return assertOk(producer.compressMessage(msg, &id));
}

// An external connection negotiates the net set (snappy) only. A frame
// compressed with a replication-only algorithm (zlib) must be rejected at decompression even though
// zlib is registered process-wide, so the union registry can no longer be used to smuggle an
// out-of-domain algorithm onto an external connection.
TEST(MessageCompressorManager, ServerExternalRejectsDecompressOfReplicationOnlyFrame) {
    auto registry = buildNetSnappyReplZlibRegistry();
    auto zlibFrame = compressWith(&registry, buildMessage(), "zlib");

    MessageCompressorManager server(&registry);
    BSONObjBuilder out;
    std::vector<std::string_view> clientList{"snappy"};
    server.serverNegotiate(clientList, &out);  // default => external / net set
    checkNegotiationResult(out.done(), {"snappy"});

    MessageCompressorId cid;
    auto sw = server.decompressMessage(zlibFrame, &cid);
    ASSERT_EQ(ErrorCodes::BadValue, sw.getStatus());
}

// The server echo-back path must never emit a compressor outside this
// connection's candidate set. Forcing the zlib id on an external connection returns the message
// uncompressed instead of producing an OP_COMPRESSED zlib frame.
TEST(MessageCompressorManager, ServerExternalEchoBackOfReplicationOnlyCompressorFallsBackUncompressed) {
    auto registry = buildNetSnappyReplZlibRegistry();
    MessageCompressorManager server(&registry);
    BSONObjBuilder out;
    std::vector<std::string_view> clientList{"snappy"};
    server.serverNegotiate(clientList, &out);

    auto zlibId = registry.getCompressor("zlib")->getId();
    auto msg = buildMessage();
    auto sw = server.compressMessage(msg, &zlibId);
    ASSERT_OK(sw.getStatus());
    // Not permitted for this connection => returned as-is, still the original opcode (not compressed).
    ASSERT_EQ(sw.getValue().operation(), dbQuery);
}

// The same registry, but a replication connection (candidate set {zlib}) both
// decompresses a zlib frame and echoes it back with zlib. This confirms the permit list keeps
// in-domain behavior fully working and only blocks cross-domain use.
TEST(MessageCompressorManager, ServerReplicationConnectionAcceptsReplicationCompressor) {
    auto registry = buildNetSnappyReplZlibRegistry();
    auto zlibFrame = compressWith(&registry, buildMessage(), "zlib");

    MessageCompressorManager server(&registry);
    BSONObjBuilder out;
    std::vector<std::string_view> clientList{"zlib"};
    std::vector<std::string> replCandidates{"zlib"};
    server.serverNegotiate(clientList, &out, replCandidates);
    checkNegotiationResult(out.done(), {"zlib"});

    MessageCompressorId cid;
    auto recvd = assertOk(server.decompressMessage(zlibFrame, &cid));
    ASSERT_EQ(cid, registry.getCompressor("zlib")->getId());
    auto echoed = assertOk(server.compressMessage(recvd, &cid));
    ASSERT_EQ(echoed.operation(), dbCompressed);
}

// With net.compression.compressors: disabled the external candidate set is
// empty, so the permit list is empty and even a registered, replication-attributed algorithm (snappy)
// cannot be decompressed on an external connection. This makes "disabled" truly mean uncompressed
// for external clients despite the union registry containing snappy.
TEST(MessageCompressorManager, ServerExternalWithNetDisabledRejectsAnyCompressedFrame) {
    auto registry = buildNetDisabledReplSnappyRegistry();  // net empty, repl = snappy
    auto snappyFrame = compressWith(&registry, buildMessage(), "snappy");

    MessageCompressorManager server(&registry);
    BSONObjBuilder out;
    std::vector<std::string_view> clientList{"snappy"};
    server.serverNegotiate(clientList, &out);  // external, net set empty
    checkNegotiationResult(out.done(), {});

    MessageCompressorId cid;
    auto sw = server.decompressMessage(snappyFrame, &cid);
    ASSERT_EQ(ErrorCodes::BadValue, sw.getStatus());
}

// Client-side inbound decompression is also constrained by the negotiated result.
// A server response compressed with a replication-only algorithm that the client did not advertise
// or negotiate must be rejected even though that algorithm is registered process-wide in the union.
TEST(MessageCompressorManager, ClientSideDecompressionRejectsUnnegotiatedCompressor) {
    auto registry = buildNetSnappyReplZlibRegistry();
    auto zlibFrame = compressWith(&registry, buildMessage(), "zlib");

    MessageCompressorManager client(&registry);
    BSONObjBuilder clientOut;
    client.clientBegin(&clientOut);  // advertises the net set (snappy)
    // Server responds negotiating snappy only.
    client.clientFinish(BSON("compression" << BSON_ARRAY("snappy")));

    MessageCompressorId cid;
    auto sw = client.decompressMessage(zlibFrame, &cid);
    ASSERT_EQ(ErrorCodes::BadValue, sw.getStatus());
}

// A client connection that negotiated no compression (for example replicationNetworkCompression:
// disabled) must reject any compressed response, including a registered replication-only algorithm.
TEST(MessageCompressorManager, ClientSideDisabledNegotiationRejectsCompressedResponse) {
    auto registry = buildNetDisabledReplSnappyRegistry();
    auto snappyFrame = compressWith(&registry, buildMessage(), "snappy");

    MessageCompressorManager client(&registry);
    client.disableCompressionForThisSession();
    BSONObjBuilder clientOut;
    client.clientBegin(&clientOut);
    ASSERT_FALSE(clientOut.done().hasField("compression"));
    client.clientFinish(BSONObj{});

    MessageCompressorId cid;
    auto sw = client.decompressMessage(snappyFrame, &cid);
    ASSERT_EQ(ErrorCodes::BadValue, sw.getStatus());
}

// A manager NOT marked as a replication client must never emit the
// "replicationCompressionClient" hello marker, so the server treats it like any other internal or
// external connection (heartbeats, shard RPC, external clients all keep using the net set). This
// is the guard that scopes replication.networkCompression to actual sync-source connections.
TEST(MessageCompressorManager, ClientBeginDoesNotMarkReplicationByDefault) {
    auto registry = buildRegistry();  // registers "noop" in the net set
    MessageCompressorManager manager(&registry);

    BSONObjBuilder out;
    manager.clientBegin(&out);
    auto obj = out.done();
    ASSERT_FALSE(obj.hasField(MessageCompressorManager::kReplicationCompressionClientFieldName));
}

// Once marked as a replication client, clientBegin() must emit
// "replicationCompressionClient": true so the server routes this connection (and only this
// connection) through the replication candidate set. The marker must be present in every state
// (inherit / allow-list / suppressed) because the server needs to identify the connection type
// regardless of whether it ends up advertising compression.
TEST(MessageCompressorManager, ClientBeginMarksReplicationClientInEveryState) {
    auto registry = buildNetDisabledReplSnappyRegistry();
    MessageCompressorManager manager(&registry);
    manager.markReplicationClientForThisSession(true);

    // (a) inherit (no allow-list, net disabled => advertises nothing) still carries the marker.
    {
        BSONObjBuilder out;
        manager.clientBegin(&out);
        auto obj = out.done();
        ASSERT_TRUE(
            obj.getField(MessageCompressorManager::kReplicationCompressionClientFieldName)
                .booleanSafe());
        // net disabled + inherit => nothing advertised.
        ASSERT_FALSE(obj.hasField("compression"));
    }

    // (b) allow-list (advertises snappy from the union) carries the marker.
    {
        manager.setCompressorAllowListForThisSession({"snappy"});
        BSONObjBuilder out;
        manager.clientBegin(&out);
        auto obj = out.done();
        ASSERT_TRUE(
            obj.getField(MessageCompressorManager::kReplicationCompressionClientFieldName)
                .booleanSafe());
        checkNegotiationResult(obj, {"snappy"});
    }

    // (c) suppressed (disabled) still carries the marker even though it advertises nothing.
    {
        manager.setCompressorAllowListForThisSession({});
        manager.disableCompressionForThisSession();
        BSONObjBuilder out;
        manager.clientBegin(&out);
        auto obj = out.done();
        ASSERT_TRUE(
            obj.getField(MessageCompressorManager::kReplicationCompressionClientFieldName)
                .booleanSafe());
        ASSERT_FALSE(obj.hasField("compression"));
    }
}

// A replication compressor that this build does not provide is a hard startup error, exactly like
// an unavailable net compressor. This keeps replication.networkCompression.compressors consistent
// with net.compression.compressors: a configured-but-uncompiled algorithm can never be silently
// ignored (which would leave the operator believing replication compression is active when it is
// not).
TEST(MessageCompressorRegistryTest, FinalizeRejectsUnavailableReplicationCompressor) {
    MessageCompressorRegistry registry;
    registry.setSupportedCompressors(std::vector<std::string>{});
    // "brotli" is not a real compressor in this build; naming it for replication must fail startup.
    registry.addReplicationCompressors({"snappy", "brotli"});
    registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
    ASSERT_NOT_OK(registry.finalizeSupportedCompressors());
}

// A replication compressor that IS compiled in survives finalize and is reported in both the union
// and the replication attribution set, while the net set stays empty (disabled).
TEST(MessageCompressorRegistryTest, FinalizeKeepsAvailableReplicationOnlyCompressor) {
    MessageCompressorRegistry registry;
    registry.setSupportedCompressors(std::vector<std::string>{});
    registry.addReplicationCompressors({"snappy"});
    registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
    ASSERT_OK(registry.finalizeSupportedCompressors());

    const auto& unionNames = registry.getCompressorNames();
    ASSERT_TRUE(std::find(unionNames.begin(), unionNames.end(), "snappy") != unionNames.end());

    const auto& replNames = registry.getReplCompressorNames();
    ASSERT_TRUE(std::find(replNames.begin(), replNames.end(), "snappy") != replNames.end());

    // The net set stays empty (disabled).
    ASSERT_TRUE(registry.getNetCompressorNames().empty());
}

TEST(MessageCompressorRegistryTest, FinalizeRejectsUnavailableNetCompressor) {
    MessageCompressorRegistry registry;
    // "brotli" as a net compressor is a hard error because no implementation registers it.
    registry.setSupportedCompressors({"brotli"});
    ASSERT_NOT_OK(registry.finalizeSupportedCompressors());
}

TEST(NoopMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<NoopMessageCompressor>());
}

TEST(SnappyMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<SnappyMessageCompressor>());
}

TEST(ZlibMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<ZlibMessageCompressor>());
}

TEST(ZstdMessageCompressor, Fidelity) {
    auto testMessage = buildMessage();
    checkFidelity(testMessage, std::make_unique<ZstdMessageCompressor>());
}

TEST(SnappyMessageCompressor, Overflow) {
    checkOverflow(std::make_unique<SnappyMessageCompressor>());
}

TEST(ZlibMessageCompressor, Overflow) {
    checkOverflow(std::make_unique<ZlibMessageCompressor>());
}

TEST(ZstdMessageCompressor, Overflow) {
    checkOverflow(std::make_unique<ZstdMessageCompressor>());
}

TEST(ZlibMessageCompressor, Mismatch) {
    checkOverflow(std::make_unique<ZlibMessageCompressor>());
}

TEST(SnappyMessageCompressor, Undersize) {
    std::vector<std::uint8_t> payload = {
        0x41, 0x0, 0x0,  0x0,  0xad, 0xde, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0xdc,
        0x7,  0x0, 0x0,  0xdd, 0x7,  0x0,  0x0,  0x0,  0x20, 0x0,  0x0,  0x1,  0x27,
        0x0,  0x0, 0x1,  0x1,  0x84, 0xfb, 0x1f, 0x0,  0x0,  0x5,  0x5f, 0x69, 0x64,
        0x0,  0x0, 0x10, 0x0,  0x0,  0x0,  0x48, 0x45, 0x41, 0x50, 0x4c, 0x45, 0x41,
        0x4b, 0x0, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0x0};


    auto buffer = SharedBuffer::allocate(payload.size());
    std::copy(payload.begin(), payload.end(), buffer.get());

    checkUndersize(Message(buffer), std::make_unique<SnappyMessageCompressor>());
}

TEST(ZlibMessageCompressor, Undersize) {
    std::vector<std::uint8_t> payload = {
        0x3c, 0x00, 0x00, 0x00, 0xad, 0xde, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xdc, 0x07, 0x00,
        0x00, 0xdd, 0x07, 0x00, 0x00, 0x00, 0x20, 0x00, 0x00, 0x02, 0x78, 0xda, 0x63, 0x60, 0x00,
        0x82, 0xdf, 0xf2, 0x0c, 0x0c, 0xac, 0xf1, 0x99, 0x29, 0x0c, 0x0c, 0x02, 0x40, 0x9e, 0x87,
        0xab, 0x63, 0x80, 0x8f, 0xab, 0xa3, 0x37, 0x03, 0x12, 0x00, 0x00, 0x6d, 0x26, 0x04, 0x97};

    auto buffer = SharedBuffer::allocate(payload.size());
    std::copy(payload.begin(), payload.end(), buffer.get());

    checkUndersize(Message(buffer), std::make_unique<ZlibMessageCompressor>());
}

TEST(ZstdMessageCompressor, Undersize) {
    std::vector<std::uint8_t> payload = {
        0x44, 0x0,  0x0,  0x0,  0xad, 0xde, 0x0,  0x0,  0x0,  0x0,  0x0,  0x0,  0xdc, 0x7,
        0x0,  0x0,  0xdd, 0x7,  0x0,  0x0,  0x0,  0x20, 0x0,  0x0,  0x3,  0x28, 0xb5, 0x2f,
        0xfd, 0x20, 0x27, 0x15, 0x1,  0x0,  0xe0, 0x0,  0x0,  0x0,  0x0,  0x0,  0xfb, 0x1f,
        0x0,  0x0,  0x5,  0x5f, 0x69, 0x64, 0x0,  0x0,  0x10, 0x0,  0x0,  0x0,  0x48, 0x45,
        0x41, 0x50, 0x4c, 0x45, 0x41, 0x4b, 0x0,  0x1,  0x0,  0x18, 0xc0, 0x9};

    auto buffer = SharedBuffer::allocate(payload.size());
    std::copy(payload.begin(), payload.end(), buffer.get());

    checkUndersize(Message(buffer), std::make_unique<ZstdMessageCompressor>());
}

TEST(MessageCompressorManager, SERVER_28008) {

    // Create a client and server that will negotiate the same compressors,
    // but with a different ordering for the preferred compressor.

    std::unique_ptr<MessageCompressorBase> zstdCompressor =
        std::make_unique<ZstdMessageCompressor>();
    const auto zstdId = zstdCompressor->getId();

    std::unique_ptr<MessageCompressorBase> zlibCompressor =
        std::make_unique<ZlibMessageCompressor>();
    const auto zlibId = zlibCompressor->getId();

    std::unique_ptr<MessageCompressorBase> snappyCompressor =
        std::make_unique<SnappyMessageCompressor>();
    const auto snappyId = snappyCompressor->getId();

    MessageCompressorRegistry registry;
    registry.setSupportedCompressors(
        {snappyCompressor->getName(), zlibCompressor->getName(), zstdCompressor->getName()});
    registry.registerImplementation(std::move(zlibCompressor));
    registry.registerImplementation(std::move(zstdCompressor));
    registry.registerImplementation(std::move(snappyCompressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());

    MessageCompressorManager clientManager(&registry);
    MessageCompressorManager serverManager(&registry);

    // Do negotiation
    BSONObjBuilder clientOutput;
    clientManager.clientBegin(&clientOutput);
    auto clientObj = clientOutput.done();
    BSONObjBuilder serverOutput;
    serverManager.serverNegotiate(parseBSON(clientObj), &serverOutput);
    auto serverObj = serverOutput.done();
    clientManager.clientFinish(serverObj);

    // The preferred compressor is snappy. Check that we round trip as snappy by default.
    auto toSend = buildMessage();
    toSend = assertOk(clientManager.compressMessage(toSend, nullptr));
    MessageCompressorId compressorId;
    auto recvd = assertOk(serverManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, snappyId);
    toSend = assertOk(serverManager.compressMessage(recvd, nullptr));
    recvd = assertOk(clientManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, snappyId);

    // Now, force the client to send as zLib. We should round trip as
    // zlib if we feed the out compresor id parameter from
    // decompressMessage back in to compressMessage.
    toSend = buildMessage();
    toSend = assertOk(clientManager.compressMessage(toSend, &zlibId));
    recvd = assertOk(serverManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zlibId);
    toSend = assertOk(serverManager.compressMessage(recvd, &compressorId));
    recvd = assertOk(clientManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zlibId);

    // Then, force the client to send as zstd. We should round trip as
    // zstd if we feed the out compresor id parameter from
    // decompressMessage back in to compressMessage.
    toSend = buildMessage();
    toSend = assertOk(clientManager.compressMessage(toSend, &zstdId));
    recvd = assertOk(serverManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zstdId);
    toSend = assertOk(serverManager.compressMessage(recvd, &compressorId));
    recvd = assertOk(clientManager.decompressMessage(toSend, &compressorId));
    ASSERT_EQ(compressorId, zstdId);
}

TEST(MessageCompressorManager, MessageSizeTooLarge) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(128);

    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(MaxMessageSizeBytes + 1);
    cursor.writeAndAdvance<LittleEndian<uint8_t>>(registry.getCompressor("noop")->getId());

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

TEST(MessageCompressorManager, MessageSizeMax32Bit) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(128);

    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(std::numeric_limits<int32_t>::max());
    cursor.writeAndAdvance<LittleEndian<uint8_t>>(registry.getCompressor("noop")->getId());

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

TEST(MessageCompressorManager, MessageSizeTooSmall) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(128);

    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(-1);
    cursor.writeAndAdvance<LittleEndian<uint8_t>>(registry.getCompressor("noop")->getId());

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

TEST(MessageCompressorManager, RuntMessage) {
    auto registry = buildRegistry();
    MessageCompressorManager compManager(&registry);

    auto badMessageBuffer = SharedBuffer::allocate(128);
    MsgData::View badMessage(badMessageBuffer.get());
    badMessage.setId(1);
    badMessage.setResponseToMsgId(0);
    badMessage.setOperation(dbCompressed);
    badMessage.setLen(MsgData::MsgDataHeaderSize + 8);

    // This is a totally bogus compression header of just the original opcode + 0 byte uncompressed
    // size
    DataRangeCursor cursor(badMessage.data(), badMessage.data() + badMessage.dataLen());
    cursor.writeAndAdvance<LittleEndian<int32_t>>(dbQuery);
    cursor.writeAndAdvance<LittleEndian<int32_t>>(0);

    auto status = compManager.decompressMessage(Message(badMessageBuffer), nullptr).getStatus();
    ASSERT_NOT_OK(status);
}

class ZlibDecompressTest : public unittest::Test {
public:
    Status doDecompress(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
        ConstDataRange inRange(in.data(), in.size());
        DataRange outRange(out.data(), out.size());
        auto swSize = compressor->decompressData(inRange, outRange);
        if (swSize.isOK())
            out.resize(swSize.getValue());
        return swSize.getStatus();
    }

    Status doDecompress(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out(1024);
        return doDecompress(in, out);
    }

    std::vector<uint8_t> doCompress(const std::vector<uint8_t>& in) {
        std::vector<uint8_t> out(compressor->getMaxCompressedSize(in.size()));
        DataRange outRange(out.data(), out.size());
        auto swSz = compressor->compressData(in, outRange);
        ASSERT_OK(swSz);
        out.resize(swSz.getValue());
        return out;
    }

    static std::vector<uint8_t> strVec(std::string_view s) {
        return std::vector<uint8_t>(s.begin(), s.end());
    }

    std::unique_ptr<ZlibMessageCompressor> compressor{std::make_unique<ZlibMessageCompressor>()};
};

TEST_F(ZlibDecompressTest, RejectsEmptyPayload) {
    ASSERT_EQ(doDecompress({}), ErrorCodes::BadValue);
}

TEST_F(ZlibDecompressTest, RejectsUndersizedPayload) {
    ASSERT_EQ(doDecompress({0x78, 0x9c, 0x03, 0x00}), ErrorCodes::BadValue);
}

TEST_F(ZlibDecompressTest, RejectsBadCompressionMethod) {
    const uint8_t cm = 0;  // Expected to be 8.
    const uint8_t cmf = 0x70 | (cm & 0xf);
    ASSERT_EQ(doDecompress({cmf, 0x9c, 0x63, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00}),
              ErrorCodes::BadValue);
}

TEST_F(ZlibDecompressTest, RoundTripLongerData) {
    const auto data = strVec("Hello, MongoDB! Compression hardening test.");
    std::vector<uint8_t> out(data.size() + 100);
    ASSERT_OK(doDecompress(doCompress(data), out));
    ASSERT_EQ(out, data);
}

TEST_F(ZlibDecompressTest, RoundTripShortData) {
    const auto data = strVec("Short");
    std::vector<uint8_t> out(10000);
    ASSERT_OK(doDecompress(doCompress(data), out));
    ASSERT_EQ(out, data);
}

TEST_F(ZlibDecompressTest, ByteCounts) {
    const auto data = strVec("Hello, MongoDB! Compression hardening test.");
    uint64_t compressIn = 0;
    uint64_t compressOut = 0;
    uint64_t decompressIn = 0;
    uint64_t decompressOut = 0;
    for (int i = 0;; ++i) {
        ASSERT_EQ(compressor->getCompressorBytesIn(), compressIn);
        ASSERT_EQ(compressor->getCompressorBytesOut(), compressOut);
        ASSERT_EQ(compressor->getDecompressorBytesIn(), decompressIn);
        ASSERT_EQ(compressor->getDecompressorBytesOut(), decompressOut);
        if (i >= 5)
            break;

        std::vector<uint8_t> compressed = doCompress(data);
        compressIn += data.size();
        compressOut += compressed.size();

        std::vector<uint8_t> out(data.size() + 100);
        ASSERT_OK(doDecompress(compressed, out));
        ASSERT_EQ(out, data);
        decompressIn += compressed.size();
        decompressOut += data.size();
    }
}

void checkWrongUncompressedSize(std::unique_ptr<MessageCompressorBase> compressor,
                                std::string expectedError) {
    MessageCompressorRegistry registry;
    const auto compressorId = compressor->getId();

    std::vector<std::string> compressorList = {compressor->getName()};
    registry.setSupportedCompressors(std::move(compressorList));
    registry.registerImplementation(std::move(compressor));
    ASSERT_OK(registry.finalizeSupportedCompressors());
    MessageCompressorManager manager(&registry);

    OpMsgBuilder msgBuilder;
    msgBuilder.setBody(BSON("ping" << 1));
    Message originalMsg = msgBuilder.finishWithoutSizeChecking();
    const auto originalView = originalMsg.singleData();
    const size_t originalDataSize = originalView.dataLen();

    auto swCompressed = manager.compressMessage(originalMsg, &compressorId);
    ASSERT_OK(swCompressed);
    Message properlyCompressedMsg = std::move(swCompressed.getValue());

    const auto compressedView = properlyCompressedMsg.singleData();
    ConstDataRangeCursor input(compressedView.data(),
                               compressedView.data() + compressedView.dataLen());

    CompressionHeader originalHeader(&input);

    const size_t compressedDataSize = input.length();
    const char* compressedDataPtr = input.data();

    ASSERT_EQ(originalHeader.compressorId, compressorId);
    ASSERT_EQ(originalHeader.originalOpCode, dbMsg);

    const int32_t wrongUncompressedSize = static_cast<int32_t>(originalDataSize * 2);

    const size_t malformedMessageSize =
        MsgData::MsgDataHeaderSize + CompressionHeader::size() + compressedDataSize;
    auto malformedBuffer = SharedBuffer::allocate(malformedMessageSize);

    MsgData::View malformedView(malformedBuffer.get());
    malformedView.setId(originalView.getId());
    malformedView.setResponseToMsgId(originalView.getResponseToMsgId());
    malformedView.setOperation(dbCompressed);
    malformedView.setLen(malformedMessageSize);

    DataRangeCursor output(malformedView.data(), malformedView.data() + malformedView.dataLen());
    output.writeAndAdvance<LittleEndian<int32_t>>(originalHeader.originalOpCode);
    output.writeAndAdvance<LittleEndian<int32_t>>(wrongUncompressedSize);
    output.writeAndAdvance<LittleEndian<uint8_t>>(originalHeader.compressorId);

    std::memcpy(output.data(), compressedDataPtr, compressedDataSize);

    Message malformedMessage(malformedBuffer);

    auto swDecompressed = manager.decompressMessage(malformedMessage);
    ASSERT_NOT_OK(swDecompressed.getStatus());
    ASSERT_EQ(swDecompressed.getStatus().code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(swDecompressed.getStatus().reason(), expectedError);
}

TEST(SnappyMessageCompressor, WrongUncompressedSize) {
    checkWrongUncompressedSize(std::make_unique<SnappyMessageCompressor>(),
                               "Uncompressed message size does not match expected size");
}

TEST(ZstdMessageCompressor, WrongUncompressedSize) {
    checkWrongUncompressedSize(std::make_unique<ZstdMessageCompressor>(),
                               "Uncompressed message size does not match expected size");
}

TEST(ZlibMessageCompressor, WrongUncompressedSize) {
    checkWrongUncompressedSize(std::make_unique<ZlibMessageCompressor>(),
                               "Decompressing message returned less data than expected");
}

}  // namespace
}  // namespace mongo
