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

void appendReplicationMessageCompressionStats(BSONObjBuilder* b) {
    // Replication-data-plane subset of appendMessageCompressionStats(). Same structure as
    // serverStatus().network.compression, but the byte counts include ONLY traffic on connections
    // attributed to replication data-plane traffic (oplog fetcher, initial-sync cloner, rollback
    // remote oplog reader, and the sync source's responses to them). These bytes are also present
    // in network.compression; this is an independent view, not a separate accounting.
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
        compressorSection << kBytesIn << compressor->getReplicationCompressorBytesIn() << kBytesOut
                          << compressor->getReplicationCompressorBytesOut();
        compressorSection.doneFast();

        BSONObjBuilder decompressorSection(base.subobjStart("decompressor"));
        decompressorSection << kBytesIn << compressor->getReplicationDecompressorBytesIn()
                            << kBytesOut << compressor->getReplicationDecompressorBytesOut();
        decompressorSection.doneFast();
        base.doneFast();
    }
    compressionSection.doneFast();
}

}  // namespace mongo
