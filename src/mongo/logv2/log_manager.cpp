/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/logv2/log_manager.h"

#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/console.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_global.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/tagged_severity_filter.h"
#include "mongo/logv2/text_formatter.h"


#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks.hpp>


namespace mongo {
namespace logv2 {

struct LogManager::Impl {
    typedef boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>
        ConsoleBackend;

    typedef boost::log::sinks::unlocked_sink<RamLogSink> RamLogBackend;

    Impl() {
        _consoleBackend = boost::make_shared<ConsoleBackend>();
        _consoleBackend->set_filter(ComponentSettingsFilter(_globalDomain.settings()));
        _consoleBackend->set_formatter(TextFormatter());

        _consoleBackend->locked_backend()->add_stream(
            boost::shared_ptr<std::ostream>(&Console::out(), boost::null_deleter()));

        _consoleBackend->locked_backend()->auto_flush();

        _globalLogCacheBackend = RamLogSink::create(RamLog::get("global"));
        _globalLogCacheBackend->set_filter(ComponentSettingsFilter(_globalDomain.settings()));
        _globalLogCacheBackend->set_formatter(TextFormatter());

        _startupWarningsBackend = RamLogSink::create(RamLog::get("startupWarnings"));
        _startupWarningsBackend->set_filter(
            TaggedSeverityFilter({LogTag::kStartupWarnings}, LogSeverity::Warning()));
        _startupWarningsBackend->set_formatter(TextFormatter());
    }

    LogDomain _globalDomain{std::make_unique<LogDomainGlobal>()};
    boost::shared_ptr<ConsoleBackend> _consoleBackend;
    boost::shared_ptr<RamLogBackend> _globalLogCacheBackend;
    boost::shared_ptr<RamLogBackend> _startupWarningsBackend;
    bool _defaultBackendsAttached{false};
};


LogManager::LogManager() {
    _impl = std::make_unique<Impl>();
    reattachDefaultBackends();
}

LogManager::~LogManager() {}

LogManager& LogManager::global() {
    static LogManager globalLogManager;
    return globalLogManager;
}

LogDomain& LogManager::getGlobalDomain() {
    return _impl->_globalDomain;
}

void LogManager::detachDefaultBackends() {
    invariant(isDefaultBackendsAttached());

    _impl->_globalDomain.impl().core()->remove_sink(_impl->_startupWarningsBackend);
    _impl->_globalDomain.impl().core()->remove_sink(_impl->_globalLogCacheBackend);
    _impl->_globalDomain.impl().core()->remove_sink(_impl->_consoleBackend);
    _impl->_defaultBackendsAttached = false;
}

void LogManager::reattachDefaultBackends() {
    invariant(!isDefaultBackendsAttached());

    _impl->_globalDomain.impl().core()->add_sink(_impl->_consoleBackend);
    _impl->_globalDomain.impl().core()->add_sink(_impl->_globalLogCacheBackend);
    _impl->_globalDomain.impl().core()->add_sink(_impl->_startupWarningsBackend);
    _impl->_defaultBackendsAttached = true;
}

bool LogManager::isDefaultBackendsAttached() const {
    return _impl->_defaultBackendsAttached;
}

}  // logv2
}  // mongo
