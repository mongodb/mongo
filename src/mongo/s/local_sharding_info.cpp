/*
 *    Copyright (C) 2016 10gen Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/s/local_sharding_info.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"

namespace mongo {
namespace {

using Handler = stdx::function<bool(OperationContext*, const std::string&)>;

class LocalShardingInfo {
public:
    void registerHandler(Handler handler) {
        _handler = handler;
    }

    const Handler& getHandler() const {
        return _handler;
    }

private:
    Handler _handler;
};

const ServiceContext::Decoration<LocalShardingInfo> forService =
    ServiceContext::declareDecoration<LocalShardingInfo>();

}  // namespace

void enableLocalShardingInfo(ServiceContext* context, Handler handler) {
    forService(context).registerHandler(handler);
}

bool haveLocalShardingInfo(OperationContext* txn, const std::string& ns) {
    auto handler = forService(txn->getServiceContext()).getHandler();
    if (handler)
        return handler(txn, ns);
    return false;
}

}  // namespace mongo
