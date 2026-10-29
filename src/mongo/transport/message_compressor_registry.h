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
     * Returns the list of compressor names that have been registered and configured.
     *
     * Iterators and value in this vector may be invalidated by calls to:
     *   setSupportedCompressors
     *   finalizeSupportedCompressors
     */
    const std::vector<std::string>& getCompressorNames() const;

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
     * option parsing and before calling registerImplementation for any compressors.
     */
    void setSupportedCompressors(std::vector<std::string>&& compressorNames);

    /*
     * Finalizes the list of supported compressors for this registry. Should be called after all
     * calls to registerImplementation. It will remove any compressor names that aren't keys in
     * the _compressors map.
     */
    Status finalizeSupportedCompressors();

private:
    StringMap<MessageCompressorBase*> _compressorsByName;
    std::array<std::unique_ptr<MessageCompressorBase>,
               std::numeric_limits<MessageCompressorId>::max() + 1>
        _compressorsByIds;
    std::vector<std::string> _compressorNames;
};

Status storeMessageCompressionOptions(const std::string& compressors);
void appendMessageCompressionStats(BSONObjBuilder* b);
}  // namespace mongo
