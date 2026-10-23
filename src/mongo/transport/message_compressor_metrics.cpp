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
const auto kIngress = "ingress"sv;
const auto kEgress = "egress"sv;
const auto kCompressor = "compressor"sv;
const auto kDecompressor = "decompressor"sv;

void appendBytesSection(BSONObjBuilder& parent,
                        std::string_view fieldName,
                        int64_t bytesIn,
                        int64_t bytesOut) {
    BSONObjBuilder section(parent.subobjStart(fieldName));
    section << kBytesIn << bytesIn << kBytesOut << bytesOut;
    section.doneFast();
}

void appendDirectionStats(BSONObjBuilder& base,
                          const MessageCompressorBase& compressor,
                          MessageCompressionDirection direction,
                          std::string_view fieldName) {
    BSONObjBuilder directionSection(base.subobjStart(fieldName));
    appendBytesSection(directionSection,
                       kCompressor,
                       compressor.getCompressorBytesIn(direction),
                       compressor.getCompressorBytesOut(direction));
    appendBytesSection(directionSection,
                       kDecompressor,
                       compressor.getDecompressorBytesIn(direction),
                       compressor.getDecompressorBytesOut(direction));
    directionSection.doneFast();
}
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

        appendBytesSection(base,
                           kCompressor,
                           compressor->getCompressorBytesIn(),
                           compressor->getCompressorBytesOut());
        appendBytesSection(base,
                           kDecompressor,
                           compressor->getDecompressorBytesIn(),
                           compressor->getDecompressorBytesOut());

        appendDirectionStats(base, *compressor, MessageCompressionDirection::kIngress, kIngress);
        appendDirectionStats(base, *compressor, MessageCompressionDirection::kEgress, kEgress);
        base.doneFast();
    }
    compressionSection.doneFast();
}

}  // namespace mongo
