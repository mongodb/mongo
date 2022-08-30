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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/wiredtiger/wiredtiger_global_options.h"
#include "mongo/db/storage/wiredtiger/wiredtiger_parameters_gen.h"
#include "mongo/logv2/log.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

using std::string;

void WiredTigerEngineRuntimeConfigParameter::append(OperationContext* opCtx,
                                                    BSONObjBuilder* b,
                                                    StringData name,
                                                    const boost::optional<TenantId>&) {
    *b << name << _data.first;
}

Status WiredTigerEngineRuntimeConfigParameter::setFromString(StringData str,
                                                             const boost::optional<TenantId>&) {
    size_t pos = str.find('\0');
    if (pos != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      (str::stream()
                       << "WiredTiger configuration strings cannot have embedded null characters. "
                          "Embedded null found at position "
                       << pos));
    }

    LOGV2(22376,
          "Reconfiguring WiredTiger storage engine with config string: \"{config}\"",
          "Reconfiguring WiredTiger storage engine",
          "config"_attr = str);

    invariant(_data.second);
    int ret = _data.second->reconfigure(str.toString().c_str());
    if (ret != 0) {
        const char* errorStr = wiredtiger_strerror(ret);
        string result = (str::stream() << "WiredTiger reconfiguration failed with error code ("
                                       << ret << "): " << errorStr);
        LOGV2_ERROR(22378,
                    "WiredTiger reconfiguration failed",
                    "error"_attr = ret,
                    "message"_attr = errorStr);

        return Status(ErrorCodes::BadValue, result);
    }

    _data.first = str.toString();
    return Status::OK();
}

Status WiredTigerDirectoryForIndexesParameter::setFromString(StringData,
                                                             const boost::optional<TenantId>&) {
    return {ErrorCodes::IllegalOperation,
            str::stream() << name() << " cannot be set via setParameter"};
};
void WiredTigerDirectoryForIndexesParameter::append(OperationContext* opCtx,
                                                    BSONObjBuilder* builder,
                                                    StringData name,
                                                    const boost::optional<TenantId>&) {
    builder->append(name, wiredTigerGlobalOptions.directoryForIndexes);
}

}  // namespace mongo
