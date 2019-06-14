/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/base/init.h"

#include <memory>

#include "mongo/db/storage/kv/kv_engine_test_harness.h"
#include "mongo/db/storage/mobile/mobile_kv_engine.h"
#include "mongo/db/storage/mobile/mobile_options_gen.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/util/options_parser/options_parser.h"
#include "mongo/util/options_parser/startup_options.h"

namespace mongo {
namespace {

class MobileKVHarnessHelper : public KVHarnessHelper {
public:
    MobileKVHarnessHelper() : _dbPath("mobile_kv_engine_harness") {
        addMobileStorageOptionDefinitions(&optionenvironment::startupOptions).ignore();
        optionenvironment::OptionsParser parser;
        std::vector<std::string> args;
        std::map<std::string, std::string> env;
        parser
            .run(optionenvironment::startupOptions,
                 args,
                 env,
                 &optionenvironment::startupOptionsParsed)
            .ignore();
        storeMobileStorageOptionDefinitions(optionenvironment::startupOptionsParsed).ignore();

        embedded::mobileGlobalOptions.disableVacuumJob = true;
        _engine = std::make_unique<MobileKVEngine>(
            _dbPath.path(), embedded::mobileGlobalOptions, nullptr);
    }

    virtual KVEngine* restartEngine() {
        _engine.reset(new MobileKVEngine(
            _dbPath.path(), embedded::mobileGlobalOptions, _serviceContext.get()));
        return _engine.get();
    }

    virtual KVEngine* getEngine() {
        return _engine.get();
    }

private:
    unittest::TempDir _dbPath;
    std::unique_ptr<MobileKVEngine> _engine;
    ServiceContext::UniqueServiceContext _serviceContext;
};

std::unique_ptr<KVHarnessHelper> makeHelper() {
    return std::make_unique<MobileKVHarnessHelper>();
}

MONGO_INITIALIZER(RegisterKVHarnessFactory)(InitializerContext*) {
    KVHarnessHelper::registerFactory(makeHelper);
    return Status::OK();
}

}  // namespace
}  // namespace mongo
