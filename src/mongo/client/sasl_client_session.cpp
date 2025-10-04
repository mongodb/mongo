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

#include "mongo/client/sasl_client_session.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <limits>
#include <type_traits>

namespace mongo {
SaslClientSession::SaslClientSessionFactoryFn SaslClientSession::create;

SaslClientSession::SaslClientSession() {}

SaslClientSession::~SaslClientSession() {}

void SaslClientSession::setParameter(Parameter id, StringData value) {
    fassert(16807, id >= 0 && id < numParameters);
    fassert(28583, value.size() < std::numeric_limits<std::size_t>::max());

    DataBuffer& buffer = _parameters[id];
    buffer.size = value.size();
    buffer.data.reset(new char[buffer.size + 1]);

    // Note that we append a terminal NUL to buffer.data, so it may be treated as a C-style
    // string.  This is required for parameterServiceName, parameterServiceHostname,
    // parameterMechanism and parameterUser.
    str::copyAsCString(buffer.data.get(), value);
}

bool SaslClientSession::hasParameter(Parameter id) {
    // The Parameter enum may be unsigned depending on compiler,
    // force it into a signed value for the purpose of bounds checking.
    const auto sid = static_cast<std::make_signed<Parameter>::type>(id);
    if (sid < 0 || id >= numParameters)
        return false;
    return static_cast<bool>(_parameters[id].data);
}

StringData SaslClientSession::getParameter(Parameter id) {
    if (!hasParameter(id))
        return StringData();

    DataBuffer& buffer = _parameters[id];
    return StringData(buffer.data.get(), buffer.size);
}

}  // namespace mongo
