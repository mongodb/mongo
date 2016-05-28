/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_record_store.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/constraints.h"

namespace mongo {

WiredTigerGlobalOptions wiredTigerGlobalOptions;

Status WiredTigerGlobalOptions::add(moe::OptionSection* options) {
    moe::OptionSection wiredTigerOptions("WiredTiger options");

    // WiredTiger storage engine options
    wiredTigerOptions.addOptionChaining("storage.wiredTiger.engineConfig.cacheSizeGB",
                                        "wiredTigerCacheSizeGB",
                                        moe::Double,
                                        "maximum amount of memory to allocate for cache; "
                                        "defaults to 1/2 of physical RAM");
    wiredTigerOptions
        .addOptionChaining("storage.wiredTiger.engineConfig.statisticsLogDelaySecs",
                           "wiredTigerStatisticsLogDelaySecs",
                           moe::Int,
                           "seconds to wait between each write to a statistics file in the dbpath; "
                           "0 means do not log statistics")
        .validRange(0, 100000)
        .setDefault(moe::Value(0));
    wiredTigerOptions
        .addOptionChaining("storage.wiredTiger.engineConfig.journalCompressor",
                           "wiredTigerJournalCompressor",
                           moe::String,
                           "use a compressor for log records [none|snappy|zlib]")
        .format("(:?none)|(:?snappy)|(:?zlib)", "(none/snappy/zlib)")
        .setDefault(moe::Value(std::string("snappy")));
    wiredTigerOptions.addOptionChaining("storage.wiredTiger.engineConfig.directoryForIndexes",
                                        "wiredTigerDirectoryForIndexes",
                                        moe::Switch,
                                        "Put indexes and data in different directories");
    wiredTigerOptions
        .addOptionChaining("storage.wiredTiger.engineConfig.configString",
                           "wiredTigerEngineConfigString",
                           moe::String,
                           "WiredTiger storage engine custom "
                           "configuration settings")
        .hidden();

    // WiredTiger collection options
    wiredTigerOptions
        .addOptionChaining("storage.wiredTiger.collectionConfig.blockCompressor",
                           "wiredTigerCollectionBlockCompressor",
                           moe::String,
                           "block compression algorithm for collection data "
                           "[none|snappy|zlib]")
        .format("(:?none)|(:?snappy)|(:?zlib)", "(none/snappy/zlib)")
        .setDefault(moe::Value(std::string("snappy")));
    wiredTigerOptions
        .addOptionChaining("storage.wiredTiger.collectionConfig.configString",
                           "wiredTigerCollectionConfigString",
                           moe::String,
                           "WiredTiger custom collection configuration settings")
        .hidden();


    // WiredTiger index options
    wiredTigerOptions
        .addOptionChaining("storage.wiredTiger.indexConfig.prefixCompression",
                           "wiredTigerIndexPrefixCompression",
                           moe::Bool,
                           "use prefix compression on row-store leaf pages")
        .setDefault(moe::Value(true));
    wiredTigerOptions
        .addOptionChaining("storage.wiredTiger.indexConfig.configString",
                           "wiredTigerIndexConfigString",
                           moe::String,
                           "WiredTiger custom index configuration settings")
        .hidden();

    return options->addSection(wiredTigerOptions);
}

Status WiredTigerGlobalOptions::store(const moe::Environment& params,
                                      const std::vector<std::string>& args) {
    // WiredTiger storage engine options
    if (params.count("storage.wiredTiger.engineConfig.cacheSizeGB")) {
        wiredTigerGlobalOptions.cacheSizeGB =
            params["storage.wiredTiger.engineConfig.cacheSizeGB"].as<double>();
    }
    if (params.count("storage.syncPeriodSecs")) {
        wiredTigerGlobalOptions.checkpointDelaySecs =
            static_cast<size_t>(params["storage.syncPeriodSecs"].as<double>());
    }
    if (params.count("storage.wiredTiger.engineConfig.statisticsLogDelaySecs")) {
        wiredTigerGlobalOptions.statisticsLogDelaySecs =
            params["storage.wiredTiger.engineConfig.statisticsLogDelaySecs"].as<int>();
    }
    if (params.count("storage.wiredTiger.engineConfig.journalCompressor")) {
        wiredTigerGlobalOptions.journalCompressor =
            params["storage.wiredTiger.engineConfig.journalCompressor"].as<std::string>();
    }
    if (params.count("storage.wiredTiger.engineConfig.directoryForIndexes")) {
        wiredTigerGlobalOptions.directoryForIndexes =
            params["storage.wiredTiger.engineConfig.directoryForIndexes"].as<bool>();
    }
    if (params.count("storage.wiredTiger.engineConfig.configString")) {
        wiredTigerGlobalOptions.engineConfig =
            params["storage.wiredTiger.engineConfig.configString"].as<std::string>();
        log() << "Engine custom option: " << wiredTigerGlobalOptions.engineConfig;
    }

    // WiredTiger collection options
    if (params.count("storage.wiredTiger.collectionConfig.blockCompressor")) {
        wiredTigerGlobalOptions.collectionBlockCompressor =
            params["storage.wiredTiger.collectionConfig.blockCompressor"].as<std::string>();
    }
    if (params.count("storage.wiredTiger.collectionConfig.configString")) {
        wiredTigerGlobalOptions.collectionConfig =
            params["storage.wiredTiger.collectionConfig.configString"].as<std::string>();
        log() << "Collection custom option: " << wiredTigerGlobalOptions.collectionConfig;
    }

    // WiredTiger index options
    if (params.count("storage.wiredTiger.indexConfig.prefixCompression")) {
        wiredTigerGlobalOptions.useIndexPrefixCompression =
            params["storage.wiredTiger.indexConfig.prefixCompression"].as<bool>();
    }
    if (params.count("storage.wiredTiger.indexConfig.configString")) {
        wiredTigerGlobalOptions.indexConfig =
            params["storage.wiredTiger.indexConfig.configString"].as<std::string>();
        log() << "Index custom option: " << wiredTigerGlobalOptions.indexConfig;
    }

    return Status::OK();
}

}  // namespace mongo
