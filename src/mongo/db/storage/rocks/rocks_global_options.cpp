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
#include "mongo/util/log.h"
#include "mongo/util/options_parser/constraints.h"

#include "rocks_global_options.h"

namespace mongo {

    RocksGlobalOptions rocksGlobalOptions;

    Status RocksGlobalOptions::add(moe::OptionSection* options) {
        moe::OptionSection rocksOptions("RocksDB options");

        rocksOptions.addOptionChaining("storage.rocksdb.cacheSizeGB", "rocksdbCacheSizeGB",
                                       moe::Int,
                                       "maximum amount of memory to allocate for cache; "
                                       "defaults to 1/2 of physical RAM").validRange(1, 10000);
        rocksOptions.addOptionChaining("storage.rocksdb.compression", "rocksdbCompression",
                                       moe::String,
                                       "block compression algorithm for collection data "
                                       "[none|snappy|zlib]")
            .format("(:?none)|(:?snappy)|(:?zlib)", "(none/snappy/zlib)")
            .setDefault(moe::Value(std::string("snappy")));
        rocksOptions
            .addOptionChaining(
                 "storage.rocksdb.maxWriteMBPerSec", "rocksdbMaxWriteMBPerSec", moe::Int,
                 "Maximum speed that RocksDB will write to storage. Reducing this can "
                 "help reduce read latency spikes during compactions. However, reducing this "
                 "below a certain point might slow down writes. Defaults to 1GB/sec")
            .validRange(1, 1024)
            .setDefault(moe::Value(1024));
        rocksOptions.addOptionChaining("storage.rocksdb.configString", "rocksdbConfigString",
                                       moe::String,
                                       "RocksDB storage engine custom "
                                       "configuration settings").hidden();
        rocksOptions.addOptionChaining("storage.rocksdb.crashSafeCounters",
                                       "rocksdbCrashSafeCounters", moe::Bool,
                                       "If true, numRecord and dataSize counter will be consistent "
                                       "even after power failure. If false, numRecord and dataSize "
                                       "might be a bit inconsistent after power failure, but "
                                       "should be correct under normal conditions. Setting this to "
                                       "true will make database inserts a bit slower.")
            .setDefault(moe::Value(false))
            .hidden();

        return options->addSection(rocksOptions);
    }

    Status RocksGlobalOptions::store(const moe::Environment& params,
                                     const std::vector<std::string>& args) {
        if (params.count("storage.rocksdb.cacheSizeGB")) {
            rocksGlobalOptions.cacheSizeGB = params["storage.rocksdb.cacheSizeGB"].as<int>();
            log() << "Block Cache Size GB: " << rocksGlobalOptions.cacheSizeGB;
        }
        if (params.count("storage.rocksdb.compression")) {
            rocksGlobalOptions.compression =
                params["storage.rocksdb.compression"].as<std::string>();
            log() << "Compression: " << rocksGlobalOptions.compression;
        }
        if (params.count("storage.rocksdb.maxWriteMBPerSec")) {
            rocksGlobalOptions.maxWriteMBPerSec =
                params["storage.rocksdb.maxWriteMBPerSec"].as<int>();
            log() << "MaxWriteMBPerSec: " << rocksGlobalOptions.maxWriteMBPerSec;
        }
        if (params.count("storage.rocksdb.configString")) {
            rocksGlobalOptions.configString =
                params["storage.rocksdb.configString"].as<std::string>();
            log() << "Engine custom option: " << rocksGlobalOptions.configString;
        }
        if (params.count("storage.rocksdb.crashSafeCounters")) {
            rocksGlobalOptions.crashSafeCounters =
                params["storage.rocksdb.crashSafeCounters"].as<bool>();
            log() << "Crash safe counters: " << rocksGlobalOptions.crashSafeCounters;
        }

        return Status::OK();
    }

}  // namespace mongo
