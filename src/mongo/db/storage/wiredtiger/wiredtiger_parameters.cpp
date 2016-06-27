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

#include "mongo/db/storage/wiredtiger/wiredtiger_parameters.h"

#include "mongo/logger/parse_log_component_settings.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

using std::string;

WiredTigerEngineRuntimeConfigParameter::WiredTigerEngineRuntimeConfigParameter(
    WiredTigerKVEngine* engine)
    : ServerParameter(
          ServerParameterSet::getGlobal(), "wiredTigerEngineRuntimeConfig", false, true),
      _engine(engine) {}


void WiredTigerEngineRuntimeConfigParameter::append(OperationContext* txn,
                                                    BSONObjBuilder& b,
                                                    const std::string& name) {
    b << name << "";
}

Status WiredTigerEngineRuntimeConfigParameter::set(const BSONElement& newValueElement) {
    try {
        return setFromString(newValueElement.String());
    } catch (const MsgAssertionException& msg) {
        return Status(
            ErrorCodes::BadValue,
            mongoutils::str::stream()
                << "Invalid value for wiredTigerEngineRuntimeConfig via setParameter command: "
                << newValueElement
                << ", exception: "
                << msg.what());
    }
}

Status WiredTigerEngineRuntimeConfigParameter::setFromString(const std::string& str) {
    size_t pos = str.find('\0');
    if (pos != std::string::npos) {
        return Status(ErrorCodes::BadValue,
                      (str::stream()
                       << "WiredTiger configuration strings cannot have embedded null characters. "
                          "Embedded null found at position "
                       << pos));
    }

    log() << "Reconfiguring WiredTiger storage engine with config string: \"" << str << "\"";

    int ret = _engine->reconfigure(str.c_str());
    if (ret != 0) {
        string result =
            (mongoutils::str::stream() << "WiredTiger reconfiguration failed with error code ("
                                       << ret
                                       << "): "
                                       << wiredtiger_strerror(ret));
        error() << result;

        return Status(ErrorCodes::BadValue, result);
    }

    return Status::OK();
}
}
