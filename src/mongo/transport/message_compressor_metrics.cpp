// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/transport/message_compressor_base.h"
#include "mongo/transport/message_compressor_registry.h"

#include <string_view>
#include <vector>

namespace mongo {
namespace {
using namespace std::literals::string_view_literals;
const auto kBytesIn = "bytesIn"sv;
const auto kBytesOut = "bytesOut"sv;
}  // namespace

void appendMessageCompressionStats(BSONObjBuilder* b) {
    auto& registry = MessageCompressorRegistry::get();
    const auto& names = registry.getCompressorNames();
    if (names.empty()) {
        return;
    }
    BSONObjBuilder compressionSection(b->subobjStart("compression"));

    for (auto&& name : names) {
        auto&& compressor = registry.getCompressor(name);
        BSONObjBuilder base(compressionSection.subobjStart(name));

        BSONObjBuilder compressorSection(base.subobjStart("compressor"));
        compressorSection << kBytesIn << compressor->getCompressorBytesIn() << kBytesOut
                          << compressor->getCompressorBytesOut();
        compressorSection.doneFast();

        BSONObjBuilder decompressorSection(base.subobjStart("decompressor"));
        decompressorSection << kBytesIn << compressor->getDecompressorBytesIn() << kBytesOut
                            << compressor->getDecompressorBytesOut();
        decompressorSection.doneFast();
        base.doneFast();
    }
    compressionSection.doneFast();
}

}  // namespace mongo
