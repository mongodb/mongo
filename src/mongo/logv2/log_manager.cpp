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
#include "mongo/util/time_support.h"

#include <boost/core/null_deleter.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks.hpp>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <boost/log/sinks/syslog_backend.hpp>
#endif

namespace mongo {
namespace logv2 {

namespace {

class RotateCollector : public boost::log::sinks::file::collector {
public:
    explicit RotateCollector(bool renameOnRotate) : _renameOnRotate{renameOnRotate} {}

    void store_file(boost::filesystem::path const& file) override {
        if (_renameOnRotate) {
            auto renameTarget = file.string() + "." + terseCurrentTime(false);
            boost::system::error_code ec;
            boost::filesystem::rename(file, renameTarget, ec);
            if (ec) {
                // throw here or propagate this error in another way?
            }
        }
    }

    uintmax_t scan_for_files(boost::log::sinks::file::scan_method,
                             boost::filesystem::path const&,
                             unsigned int*) override {
        return 0;
    }

private:
    bool _renameOnRotate;
};

}  // namespace


struct LogManager::Impl {
    typedef boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>
        ConsoleBackend;
    typedef boost::log::sinks::unlocked_sink<RamLogSink> RamLogBackend;
#ifndef _WIN32
    typedef boost::log::sinks::synchronous_sink<boost::log::sinks::syslog_backend> SyslogBackend;
#endif
    typedef boost::log::sinks::synchronous_sink<boost::log::sinks::text_file_backend>
        RotatableFileBackend;

    Impl() {
        _consoleBackend = boost::make_shared<ConsoleBackend>();
        _consoleBackend->set_filter(ComponentSettingsFilter(_globalDomain));
        _consoleBackend->set_formatter(TextFormatter());

        _consoleBackend->locked_backend()->add_stream(
            boost::shared_ptr<std::ostream>(&Console::out(), boost::null_deleter()));

        _consoleBackend->locked_backend()->auto_flush();

        _globalLogCacheBackend = RamLogSink::create(RamLog::get("global"));
        _globalLogCacheBackend->set_filter(ComponentSettingsFilter(_globalDomain));
        _globalLogCacheBackend->set_formatter(TextFormatter());

        _startupWarningsBackend = RamLogSink::create(RamLog::get("startupWarnings"));
        _startupWarningsBackend->set_filter(TaggedSeverityFilter(
            _globalDomain, {LogTag::kStartupWarnings}, LogSeverity::Warning()));
        _startupWarningsBackend->set_formatter(TextFormatter());
    }

    void setupSyslogBackend(int syslogFacility) {
#ifndef _WIN32
        // Create a backend
        auto backend = boost::make_shared<boost::log::sinks::syslog_backend>(
            boost::log::keywords::facility =
                boost::log::sinks::syslog::make_facility(syslogFacility),
            boost::log::keywords::use_impl = boost::log::sinks::syslog::native);

        boost::log::sinks::syslog::custom_severity_mapping<LogSeverity> mapping(
            attributes::severity());

        mapping[LogSeverity::Debug(5)] = boost::log::sinks::syslog::debug;
        mapping[LogSeverity::Debug(4)] = boost::log::sinks::syslog::debug;
        mapping[LogSeverity::Debug(3)] = boost::log::sinks::syslog::debug;
        mapping[LogSeverity::Debug(2)] = boost::log::sinks::syslog::debug;
        mapping[LogSeverity::Debug(1)] = boost::log::sinks::syslog::debug;
        mapping[LogSeverity::Log()] = boost::log::sinks::syslog::debug;
        mapping[LogSeverity::Info()] = boost::log::sinks::syslog::info;
        mapping[LogSeverity::Warning()] = boost::log::sinks::syslog::warning;
        mapping[LogSeverity::Error()] = boost::log::sinks::syslog::critical;
        mapping[LogSeverity::Severe()] = boost::log::sinks::syslog::alert;

        backend->set_severity_mapper(mapping);

        _syslogBackend = boost::make_shared<SyslogBackend>(backend);
        _syslogBackend->set_filter(ComponentSettingsFilter(_globalDomain));
        _syslogBackend->set_formatter(TextFormatter());
#endif
    }

    void setupRotatableFileBackend(std::string path, bool append) {

        auto backend = boost::make_shared<boost::log::sinks::text_file_backend>(
            boost::log::keywords::file_name = path);
        backend->auto_flush(true);

        backend->set_file_collector(boost::make_shared<RotateCollector>(!append));

        _rotatableFileBackend = boost::make_shared<RotatableFileBackend>(backend);
        _rotatableFileBackend->set_filter(ComponentSettingsFilter(_globalDomain));
        _rotatableFileBackend->set_formatter(TextFormatter());
    }

    template <class Formatter>
    void setFormatterToAllBackends() {
        _consoleBackend->set_formatter(Formatter());
        _globalLogCacheBackend->set_formatter(Formatter());
        _startupWarningsBackend->set_formatter(Formatter());
        if (_rotatableFileBackend)
            _rotatableFileBackend->set_formatter(Formatter());
#ifndef _WIN32
        if (_syslogBackend)
            _syslogBackend->set_formatter(Formatter());
#endif
    }

    LogDomain _globalDomain{std::make_unique<LogDomainGlobal>()};
    // I think that, technically, these are logging front ends
    // and that they get to hold or wrap a backend
    boost::shared_ptr<ConsoleBackend> _consoleBackend;
    boost::shared_ptr<RotatableFileBackend> _rotatableFileBackend;
#ifndef _WIN32
    boost::shared_ptr<SyslogBackend> _syslogBackend;
#endif
    boost::shared_ptr<RamLogBackend> _globalLogCacheBackend;
    boost::shared_ptr<RamLogBackend> _startupWarningsBackend;
    LogFormat _format{LogFormat::kDefault};
    bool _defaultBackendsAttached{false};
};


void LogManager::rotate() {
    if (_impl->_rotatableFileBackend) {
        auto backend = _impl->_rotatableFileBackend->locked_backend();
        backend->rotate_file();
    }
}

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

void LogManager::setOutputFormat(LogFormat format) {
    if (_impl->_format != format) {
        switch (format) {
            case LogFormat::kText:
                _impl->setFormatterToAllBackends<TextFormatter>();
                break;

            case LogFormat::kJson:
                _impl->setFormatterToAllBackends<JsonFormatter>();
                break;

            default:
                break;
        };
        _impl->_format = format;
    }
}

void LogManager::detachDefaultBackends() {
    invariant(isDefaultBackendsAttached());

    auto logCore = boost::log::core::get();
    logCore->remove_sink(_impl->_startupWarningsBackend);
    logCore->remove_sink(_impl->_globalLogCacheBackend);
    logCore->remove_sink(_impl->_consoleBackend);
    _impl->_defaultBackendsAttached = false;
}

void LogManager::detachConsoleBackend() {
    boost::log::core::get()->remove_sink(_impl->_consoleBackend);
}

void LogManager::setupRotatableFileBackend(std::string path, bool append) {
    _impl->setupRotatableFileBackend(path, append);
}

void LogManager::setupSyslogBackend(int syslogFacility) {
    _impl->setupSyslogBackend(syslogFacility);
}

void LogManager::reattachSyslogBackend() {
#ifndef _WIN32
    boost::log::core::get()->add_sink(_impl->_syslogBackend);
#endif
}

void LogManager::reattachRotatableFileBackend() {
    boost::log::core::get()->add_sink(_impl->_rotatableFileBackend);
}

void LogManager::reattachConsoleBackend() {
    boost::log::core::get()->add_sink(_impl->_consoleBackend);
}

void LogManager::reattachDefaultBackends() {
    invariant(!isDefaultBackendsAttached());

    auto logCore = boost::log::core::get();
    logCore->add_sink(_impl->_consoleBackend);
    logCore->add_sink(_impl->_globalLogCacheBackend);
    logCore->add_sink(_impl->_startupWarningsBackend);
    _impl->_defaultBackendsAttached = true;
}

bool LogManager::isDefaultBackendsAttached() const {
    return _impl->_defaultBackendsAttached;
}

}  // namespace logv2
}  // namespace mongo
