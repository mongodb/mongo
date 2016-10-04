/**
 *    Copyright (C) 2016 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/util/string_map.h"

#include <array>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mongo {

namespace optionenvironment {
class OptionSection;
class Environment;
}  // namespace option environment

namespace moe = mongo::optionenvironment;

/*
 * The MessageCompressorRegistry holds the global registrations of compressors for a process.
 */
class MessageCompressorRegistry {
    MONGO_DISALLOW_COPYING(MessageCompressorRegistry);

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
    MessageCompressorBase* getCompressor(StringData name) const;

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

Status addMessageCompressionOptions(moe::OptionSection* options, bool forShell);
Status storeMessageCompressionOptions(const moe::Environment& params);
void appendMessageCompressionStats(BSONObjBuilder* b);
}  // namespace mongo
