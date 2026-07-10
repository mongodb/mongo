// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/error_extra_info.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/util/modules.h"
#include "mongo/util/string_map.h"

#include <array>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace optionenvironment

namespace moe = mongo::optionenvironment;

/*
 * The MessageCompressorRegistry holds the global registrations of compressors for a process.
 */
class MessageCompressorRegistry {
    MessageCompressorRegistry(const MessageCompressorRegistry&) = delete;
    MessageCompressorRegistry& operator=(const MessageCompressorRegistry&) = delete;

public:
    MessageCompressorRegistry() = default;

    MessageCompressorRegistry(MessageCompressorRegistry&&) = default;
    MessageCompressorRegistry& operator=(MessageCompressorRegistry&&) = default;

    /*
     * Returns the global MessageCompressorRegistry
     */
    static MessageCompressorRegistry& get();

    /*
     * Registers a new implementation of a MessageCompressor with the registry. This only gets
     * called during startup. It is an error to call this twice with compressors with the same name
     * or ID numbers.
     *
     * This method is not thread-safe and should only be called from a single-threaded context
     * (a MONGO_INITIALIZER).
     */
    void registerImplementation(std::unique_ptr<MessageCompressorBase> impl);

    /*
     * Returns the process-wide capability set: the union of net.compression.compressors and
     * replication.networkCompression.compressors startup lists. Compressor implementations are
     * registered only if named here. Per-connection negotiation still restricts which registered
     * compressors may appear on a given connection.
     *
     * Used as the intersection base for per-session allow-lists so a replication-configured
     * compressor can be advertised even when net.compression.compressors is "disabled".
     *
     * Iterators and value in this vector may be invalidated by calls to:
     *   setSupportedCompressors
     *   addReplicationCompressors
     *   finalizeSupportedCompressors
     */
    const std::vector<std::string>& getCompressorNames() const;

    /*
     * Returns the compressor names sourced from net.compression.compressors. This is the candidate
     * set for external, client-facing connections. It is empty when net.compression.compressors is
     * "disabled", which is what keeps external connections uncompressed independently of any
     * replication.networkCompression.compressors setting (SERVER-130410).
     */
    const std::vector<std::string>& getNetCompressorNames() const;

    /*
     * Returns the compressor names sourced from replication.networkCompression.compressors. After
     * startup finalization, any name here has been validated against the compressors compiled into
     * this build. Provided for observability; server-side replication negotiation uses the
     * per-handshake candidate list supplied by the caller rather than this snapshot.
     */
    const std::vector<std::string>& getReplCompressorNames() const;

    /*
     * Returns a compressor given an ID number. If no compressor exists with the ID number, it
     * returns nullptr
     */
    MessageCompressorBase* getCompressor(MessageCompressorId id) const;

    /* Returns a compressor given a name. If no compressor with that name exists, it returns
     * nullptr
     */
    MessageCompressorBase* getCompressor(std::string_view name) const;

    /*
     * Sets the compressor names sourced from net.compression.compressors. This seeds both the
     * client-facing net candidate set and the process-wide capability union, and clears the
     * replication attribution set. Should be called during option parsing and before calling
     * registerImplementation for any compressors. Call addReplicationCompressors() afterwards to
     * fold in replication-configured names.
     */
    void setSupportedCompressors(std::vector<std::string>&& compressorNames);

    /*
     * Merges replication-configured compressor names into the process-wide capability union so
     * their implementations get registered, without adding them to the client-facing net candidate
     * set. Names already in the union are deduplicated. Must run after setSupportedCompressors()
     * and before compressor implementations register. See SERVER-130410.
     */
    void addReplicationCompressors(const std::vector<std::string>& replCompressorNames);

    /*
     * Finalizes the list of supported compressors for this registry. Should be called after all
     * calls to registerImplementation.
     *
     * Any name that was requested but is not provided by this build is a hard startup error
     * (fail-fast), regardless of whether it came from net.compression.compressors or
     * replication.networkCompression.compressors. The two sources are treated identically so a
     * compressor named in the replication configuration can never be silently ignored, which would
     * otherwise leave the operator believing replication compression is active when it is not.
     * See SERVER-130410.
     */
    Status finalizeSupportedCompressors();

private:
    StringMap<MessageCompressorBase*> _compressorsByName;
    std::array<std::unique_ptr<MessageCompressorBase>,
               std::numeric_limits<MessageCompressorId>::max() + 1>
        _compressorsByIds;
    // Union of the net and replication startup lists: the process-wide capability ceiling used for
    // compressor registration and lookup. Per-connection policy still gates wire use.
    std::vector<std::string> _compressorNames;
    // Names from net.compression.compressors. Candidate set for external client-facing
    // connections; empty when net.compression.compressors: disabled.
    std::vector<std::string> _netCompressorNames;
    // Names from replication.networkCompression.compressors. Recorded for observability
    // (getReplCompressorNames()); after startup finalization, these have been validated the same
    // way as net.compression algorithms.
    std::vector<std::string> _replCompressorNames;
};

Status storeMessageCompressionOptions(const std::string& compressors);
void appendMessageCompressionStats(BSONObjBuilder* b);
}  // namespace mongo
