// wiredtiger_global_options.cpp

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

#include "mongo/base/status.h"
#include "mongo/util/log.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"

namespace mongo {

    Status WiredTigerGlobalOptions::add(moe::OptionSection* options) {
        moe::OptionSection wiredTigerOptions("WiredTiger options");

        // Add WiredTiger storage engine specific options.
        wiredTigerOptions.addOptionChaining("storage.wiredTiger.engineConfig",
                "wiredTigerEngineConfig", moe::String, "WiredTiger storage engine configuration settings");
        wiredTigerOptions.addOptionChaining("storage.wiredTiger.collectionConfig",
                "wiredTigerCollectionConfig", moe::String, "WiredTiger collection configuration settings");
        wiredTigerOptions.addOptionChaining("storage.wiredTiger.indexConfig",
                "wiredTigerIndexConfig", moe::String, "WiredTiger index configuration settings");

        return options->addSection(wiredTigerOptions);
    }

    bool WiredTigerGlobalOptions::handlePreValidation(const moe::Environment& params) {
        return true;
    }

    Status WiredTigerGlobalOptions::store(const moe::Environment& params,
                                 const std::vector<std::string>& args) {
        if (params.count("storage.wiredTiger.engineConfig")) {
            wiredTigerGlobalOptions.engineConfig =
                         params["storage.wiredTiger.engineConfig"].as<string>();
            std::cerr << "Engine option: " << wiredTigerGlobalOptions.engineConfig << std::endl;
        }
        if (params.count("storage.wiredTiger.collectionConfig")) {
            wiredTigerGlobalOptions.collectionConfig =
                         params["storage.wiredTiger.collectionConfig"].as<string>();
            std::cerr << "Collection option: " << wiredTigerGlobalOptions.collectionConfig << std::endl;
        }
        if (params.count("storage.wiredTiger.indexConfig")) {
            wiredTigerGlobalOptions.indexConfig =
                         params["storage.wiredTiger.indexConfig"].as<string>();
            std::cerr << "Index option: " << wiredTigerGlobalOptions.indexConfig << std::endl;
        }
        return Status::OK();
    }
}
