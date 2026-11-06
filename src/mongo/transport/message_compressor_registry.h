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
     * Returns the net and replication compressor names together. Only these are registered;
     * negotiation decides which of them a connection may use.
     *
     * Iterators and value in this vector may be invalidated by calls to:
     *   setSupportedCompressors
     *   addReplicationCompressors
     *   finalizeSupportedCompressors
     */
    const std::vector<std::string>& getCompressorNames() const;

    /*
     * Returns net.compression.compressors. Empty when net compression is disabled.
     */
    const std::vector<std::string>& getNetCompressorNames() const;

    /*
     * Returns the compressors named in replicationNetworkCompression.
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
     * Sets the list of supported compressors for this registry. Should be called during
     * option parsing and before calling registerImplementation for any compressors. Clears the
     * replication compressors, so call it before addReplicationCompressors().
     */
    void setSupportedCompressors(std::vector<std::string>&& compressorNames);

    /*
     * Adds replication compressors to the registered set without offering them to other
     * connections. Call before compressor implementations register.
     */
    void addReplicationCompressors(const std::vector<std::string>& replCompressorNames);

    /*
     * Finalizes the list of supported compressors for this registry. Should be called after all
     * calls to registerImplementation. Returns an error for any configured compressor that is not
     * built in.
     */
    Status finalizeSupportedCompressors();

private:
    StringMap<MessageCompressorBase*> _compressorsByName;
    std::array<std::unique_ptr<MessageCompressorBase>,
               std::numeric_limits<MessageCompressorId>::max() + 1>
        _compressorsByIds;
    // Net and replication compressors combined.
    std::vector<std::string> _compressorNames;
    std::vector<std::string> _netCompressorNames;
    std::vector<std::string> _replCompressorNames;
};

Status storeMessageCompressionOptions(const std::string& compressors);
void appendMessageCompressionStats(BSONObjBuilder* b);
// Same format as appendMessageCompressionStats(), counting replication connections only.
void appendReplicationMessageCompressionStats(BSONObjBuilder* b);
}  // namespace mongo
