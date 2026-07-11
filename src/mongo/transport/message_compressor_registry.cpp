// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include <algorithm>
#include <memory>
#include <ostream>
#include <string_view>
#include <utility>

#include <absl/container/flat_hash_map.h>
#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
// IWYU pragma: no_include "boost/algorithm/string/detail/classification.hpp"
#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/initializer.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/util/assert_util.h"

#include <boost/core/addressof.hpp>
#include <boost/function/function_base.hpp>
#include <boost/iterator/iterator_facade.hpp>
#include <boost/type_index/type_index_facade.hpp>

namespace mongo {
using namespace std::literals::string_view_literals;
namespace {
constexpr auto kDisabledConfigValue = "disabled"sv;
}  // namespace

std::string_view getMessageCompressorName(MessageCompressor id) {
    switch (id) {
        case MessageCompressor::kNoop:
            return "noop"sv;
        case MessageCompressor::kSnappy:
            return "snappy"sv;
        case MessageCompressor::kZlib:
            return "zlib"sv;
        case MessageCompressor::kZstd:
            return "zstd"sv;
        default:
            fasserted(40269);  // Invalid message compressor ID
    }
    MONGO_UNREACHABLE;
}

MessageCompressorRegistry& MessageCompressorRegistry::get() {
    static MessageCompressorRegistry globalRegistry;
    return globalRegistry;
}

void MessageCompressorRegistry::registerImplementation(
    std::unique_ptr<MessageCompressorBase> impl) {
    // It's an error to register a compressor that's already been registered
    fassert(40270,
            _compressorsByName.find(impl->getName()) == _compressorsByName.end() &&
                _compressorsByIds[impl->getId()] == nullptr);

    // Check to see if this compressor is allowed by configuration
    auto it = std::find(_compressorNames.begin(), _compressorNames.end(), impl->getName());
    if (it == _compressorNames.end())
        return;

    _compressorsByName[impl->getName()] = impl.get();
    _compressorsByIds[impl->getId()] = std::move(impl);
}

Status MessageCompressorRegistry::finalizeSupportedCompressors() {
    for (auto it = _compressorNames.begin(); it != _compressorNames.end(); ++it) {
        if (_compressorsByName.find(*it) == _compressorsByName.end()) {
            std::stringstream ss;
            ss << "Invalid network message compressor specified in configuration: " << *it;
            return {ErrorCodes::BadValue, ss.str()};
        }
    }
    return Status::OK();
}

const std::vector<std::string>& MessageCompressorRegistry::getCompressorNames() const {
    return _compressorNames;
}

MessageCompressorBase* MessageCompressorRegistry::getCompressor(MessageCompressorId id) const {
    return _compressorsByIds.at(id).get();
}

MessageCompressorBase* MessageCompressorRegistry::getCompressor(std::string_view name) const {
    auto it = _compressorsByName.find(std::string{name});
    if (it == _compressorsByName.end())
        return nullptr;
    return it->second;
}

void MessageCompressorRegistry::setSupportedCompressors(std::vector<std::string>&& names) {
    _compressorNames = std::move(names);
}

Status storeMessageCompressionOptions(const std::string& compressors) {
    std::vector<std::string> restrict;
    if (compressors != kDisabledConfigValue) {
        boost::algorithm::split(restrict, compressors, boost::is_any_of(", "));
    }

    auto& compressorFactory = MessageCompressorRegistry::get();
    compressorFactory.setSupportedCompressors(std::move(restrict));

    return Status::OK();
}

// This instantiates and registers the "noop" compressor. It must happen after option storage
// because that's when the configuration of the compressors gets set.
MONGO_INITIALIZER_GENERAL(NoopMessageCompressorInit,
                          ("EndStartupOptionStorage"),
                          ("AllCompressorsRegistered"))
(InitializerContext* context) {
    auto& compressorRegistry = MessageCompressorRegistry::get();
    compressorRegistry.registerImplementation(std::make_unique<NoopMessageCompressor>());
}

// This cleans up any compressors that were requested by the user, but weren't registered by
// any compressor. It must be run after all the compressors have registered themselves with
// the global registry.
MONGO_INITIALIZER(AllCompressorsRegistered)(InitializerContext* context) {
    uassertStatusOK(MessageCompressorRegistry::get().finalizeSupportedCompressors());
}
}  // namespace mongo
