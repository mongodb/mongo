// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/base/data_range_cursor.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/query/write_ops/write_ops.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/message.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/message_compressor_noop.h"
#include "mongo/transport/message_compressor_registry.h"
#include "mongo/transport/message_compressor_snappy.h"
#include "mongo/transport/message_compressor_zlib.h"
#include "mongo/transport/message_compressor_zstd.h"
#include "mongo/util/shared_buffer.h"

namespace mongo {

struct CompressionInfrastructure {
    CompressionInfrastructure() : manager(&registry) {
        registry.setSupportedCompressors({"noop", "snappy", "zlib", "zstd"});
        registry.registerImplementation(std::make_unique<NoopMessageCompressor>());
        registry.registerImplementation(std::make_unique<SnappyMessageCompressor>());
        registry.registerImplementation(std::make_unique<ZlibMessageCompressor>());
        registry.registerImplementation(std::make_unique<ZstdMessageCompressor>());
        uassertStatusOK(registry.finalizeSupportedCompressors());
    }

    MessageCompressorRegistry registry;
    MessageCompressorManager manager;
};

void doFuzzing(ConstDataRangeCursor fuzzedData) try {
    if (fuzzedData.length() < sizeof(MSGHEADER::Layout)) {
        return;
    }

    // Make sure that we do BSON validation on all incoming messages
    serverGlobalParams.objcheck = true;

    auto sb = SharedBuffer::allocate(fuzzedData.length());
    memcpy(sb.get(), fuzzedData.data(), fuzzedData.length());
    Message msg(std::move(sb));
    if (static_cast<size_t>(msg.size()) != fuzzedData.length()) {
        return;
    }

    if (msg.operation() == dbCompressed) {
        static CompressionInfrastructure compression = {};
        msg = uassertStatusOK(compression.manager.decompressMessage(msg));
    }

    switch (msg.operation()) {
        case dbMsg: {
            auto request = OpMsgRequest::parseOwned(msg);
            validateBSON(request.body).ignore();
            for (const auto& docSeq : request.sequences) {
                for (const auto& doc : docSeq.objs) {
                    validateBSON(doc).ignore();
                }
            }
        } break;
        case dbInsert: {
            auto op = InsertOp::parseLegacy(msg);
        } break;
        case dbQuery: {
            DbMessage dbMsgObj(msg);
            QueryMessage q(dbMsgObj);
        } break;
        // These message types don't have parsers because they are no longer supported.
        case dbGetMore:
        case dbKillCursors:
        case dbDelete:
        case dbUpdate:
            break;
        default:
            invariant(!isSupportedRequestNetworkOp(msg.operation()));
            break;
    }
} catch (const DBException&) {
    // Ignore all errors from our assert macros
}

}  // namespace mongo

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    mongo::doFuzzing(mongo::ConstDataRangeCursor(Data, Size));
    return 0;
}
