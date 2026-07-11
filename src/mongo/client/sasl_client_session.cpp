// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/sasl_client_session.h"

#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

#include <cstddef>
#include <limits>
#include <string_view>
#include <type_traits>

namespace mongo {
SaslClientSession::SaslClientSessionFactoryFn SaslClientSession::create;

SaslClientSession::SaslClientSession() {}

SaslClientSession::~SaslClientSession() {}

void SaslClientSession::setParameter(Parameter id, std::string_view value) {
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

std::string_view SaslClientSession::getParameter(Parameter id) {
    if (!hasParameter(id))
        return std::string_view();

    DataBuffer& buffer = _parameters[id];
    return std::string_view(buffer.data.get(), buffer.size);
}

}  // namespace mongo
