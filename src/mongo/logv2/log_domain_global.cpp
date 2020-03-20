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

#include "log_domain_global.h"

#include "mongo/config.h"
#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/composite_backend.h"
#include "mongo/logv2/console.h"
#include "mongo/logv2/file_rotate_sink.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log_source.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/shared_access_fstream.h"
#include "mongo/logv2/tagged_severity_filter.h"
#include "mongo/logv2/text_formatter.h"
#include "mongo/logv2/uassert_sink.h"

#include <boost/core/null_deleter.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks.hpp>

namespace mongo::logv2 {

void LogDomainGlobal::ConfigurationOptions::makeDisabled() {
    consoleEnabled = false;
}

struct LogDomainGlobal::Impl {
    typedef CompositeBackend<boost::log::sinks::text_ostream_backend,
                             RamLogSink,
                             RamLogSink,
                             UserAssertSink>
        ConsoleBackend;
#ifndef _WIN32
    typedef CompositeBackend<boost::log::sinks::syslog_backend,
                             RamLogSink,
                             RamLogSink,
                             UserAssertSink>
        SyslogBackend;
#endif
    typedef CompositeBackend<FileRotateSink, RamLogSink, RamLogSink, UserAssertSink>
        RotatableFileBackend;

    Impl(LogDomainGlobal& parent);
    Status configure(LogDomainGlobal::ConfigurationOptions const& options);
    Status rotate(bool rename, StringData renameSuffix);

    const ConfigurationOptions& config() const;

    LogSource& source();

    LogDomainGlobal& _parent;
    LogComponentSettings _settings;
    ConfigurationOptions _config;
    boost::shared_ptr<boost::log::sinks::unlocked_sink<ConsoleBackend>> _consoleSink;
    boost::shared_ptr<boost::log::sinks::unlocked_sink<RotatableFileBackend>> _rotatableFileSink;
#ifndef _WIN32
    boost::shared_ptr<boost::log::sinks::unlocked_sink<SyslogBackend>> _syslogSink;
#endif
    AtomicWord<int32_t> activeSourceThreadLocals{0};
    LogSource shutdownLogSource{&_parent, true};
    bool isInShutdown{false};
};

LogDomainGlobal::Impl::Impl(LogDomainGlobal& parent) : _parent(parent) {
    auto console = boost::make_shared<ConsoleBackend>(
        boost::make_shared<boost::log::sinks::text_ostream_backend>(),
        boost::make_shared<RamLogSink>(RamLog::get("global")),
        boost::make_shared<RamLogSink>(RamLog::get("startupWarnings")),
        boost::make_shared<UserAssertSink>());

    console->lockedBackend<0>()->add_stream(
        boost::shared_ptr<std::ostream>(&Console::out(), boost::null_deleter()));
    console->lockedBackend<0>()->auto_flush();
    console->setFilter<2>(
        TaggedSeverityFilter(_parent, {LogTag::kStartupWarnings}, LogSeverity::Log()));

    _consoleSink =
        boost::make_shared<boost::log::sinks::unlocked_sink<ConsoleBackend>>(std::move(console));
    _consoleSink->set_filter(ComponentSettingsFilter(_parent, _settings));

    // Set default configuration
    invariant(configure({}).isOK());

    // Make a call to source() to make sure the internal thread_local is created as early as
    // possible and thus destroyed as late as possible.
    source();
}

Status LogDomainGlobal::Impl::configure(LogDomainGlobal::ConfigurationOptions const& options) {
#ifndef _WIN32
    if (options.syslogEnabled) {
        // Create a backend
        auto backend = boost::make_shared<SyslogBackend>(
            boost::make_shared<boost::log::sinks::syslog_backend>(
                boost::log::keywords::facility =
                    boost::log::sinks::syslog::make_facility(options.syslogFacility),
                boost::log::keywords::use_impl = boost::log::sinks::syslog::native),
            boost::make_shared<RamLogSink>(RamLog::get("global")),
            boost::make_shared<RamLogSink>(RamLog::get("startupWarnings")),
            boost::make_shared<UserAssertSink>());

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

        backend->lockedBackend<0>()->set_severity_mapper(mapping);
        backend->setFilter<2>(
            TaggedSeverityFilter(_parent, {LogTag::kStartupWarnings}, LogSeverity::Log()));

        _syslogSink =
            boost::make_shared<boost::log::sinks::unlocked_sink<SyslogBackend>>(std::move(backend));
        _syslogSink->set_filter(ComponentSettingsFilter(_parent, _settings));

        boost::log::core::get()->add_sink(_syslogSink);
    } else if (_syslogSink) {
        boost::log::core::get()->remove_sink(_syslogSink);
        _syslogSink.reset();
    }
#endif

    if (options.consoleEnabled && _consoleSink.use_count() == 1) {
        boost::log::core::get()->add_sink(_consoleSink);
    }

    if (!options.consoleEnabled && _consoleSink.use_count() > 1) {
        boost::log::core::get()->remove_sink(_consoleSink);
    }

    if (options.fileEnabled) {
        auto backend = boost::make_shared<RotatableFileBackend>(
            boost::make_shared<FileRotateSink>(options.timestampFormat),
            boost::make_shared<RamLogSink>(RamLog::get("global")),
            boost::make_shared<RamLogSink>(RamLog::get("startupWarnings")),
            boost::make_shared<UserAssertSink>());
        Status ret = backend->lockedBackend<0>()->addFile(
            options.filePath,
            options.fileOpenMode == ConfigurationOptions::OpenMode::kAppend ? true : false);
        if (!ret.isOK())
            return ret;
        backend->lockedBackend<0>()->auto_flush(true);
        backend->setFilter<2>(
            TaggedSeverityFilter(_parent, {LogTag::kStartupWarnings}, LogSeverity::Log()));

        _rotatableFileSink =
            boost::make_shared<boost::log::sinks::unlocked_sink<RotatableFileBackend>>(backend);
        _rotatableFileSink->set_filter(ComponentSettingsFilter(_parent, _settings));

        boost::log::core::get()->add_sink(_rotatableFileSink);
    } else if (_rotatableFileSink) {
        boost::log::core::get()->remove_sink(_rotatableFileSink);
        _rotatableFileSink.reset();
    }

    auto setFormatters = [this](auto&& mkFmt) {
        _consoleSink->set_formatter(mkFmt());
        if (_rotatableFileSink)
            _rotatableFileSink->set_formatter(mkFmt());
#ifndef _WIN32
        if (_syslogSink)
            _syslogSink->set_formatter(mkFmt());
#endif
    };

    switch (options.format) {
        case LogFormat::kPlain:
            setFormatters([&] { return PlainFormatter(options.maxAttributeSizeKB); });
            break;
        case LogFormat::kDefault:
        case LogFormat::kJson:
            setFormatters(
                [&] { return JSONFormatter(options.maxAttributeSizeKB, options.timestampFormat); });
            break;
    }

    _config = options;

    return Status::OK();
}

const LogDomainGlobal::ConfigurationOptions& LogDomainGlobal::Impl::config() const {
    return _config;
}

Status LogDomainGlobal::Impl::rotate(bool rename, StringData renameSuffix) {
    if (_rotatableFileSink) {
        auto backend = _rotatableFileSink->locked_backend()->lockedBackend<0>();
        return backend->rotate(rename, renameSuffix);
    }
    return Status::OK();
}

LogSource& LogDomainGlobal::Impl::source() {
    // Use a thread_local logger so we don't need to have locking. thread_locals are destroyed
    // before statics so keep track of number of thread_locals we have active and if this code
    // is hit when it is zero then we are in shutdown and can use a global LogSource that does
    // not provide synchronization instead.
    class SourceCache {
    public:
        SourceCache(Impl* domain) : _domain(domain), _source(&domain->_parent) {
            _domain->activeSourceThreadLocals.addAndFetch(1);
        }
        ~SourceCache() {
            if (_domain->activeSourceThreadLocals.subtractAndFetch(1) == 0) {
                _domain->isInShutdown = true;
            }
        }

        LogSource& source() {
            return _source;
        }

    private:
        Impl* _domain;
        LogSource _source;
    };
    thread_local SourceCache cache(this);
    if (isInShutdown)
        return shutdownLogSource;
    return cache.source();
}

LogDomainGlobal::LogDomainGlobal() {
    _impl = std::make_unique<Impl>(*this);
}

LogDomainGlobal::~LogDomainGlobal() {}

LogSource& LogDomainGlobal::source() {
    return _impl->source();
}


Status LogDomainGlobal::configure(LogDomainGlobal::ConfigurationOptions const& options) {
    return _impl->configure(options);
}

const LogDomainGlobal::ConfigurationOptions& LogDomainGlobal::config() const {
    return _impl->config();
}

Status LogDomainGlobal::rotate(bool rename, StringData renameSuffix) {
    return _impl->rotate(rename, renameSuffix);
}

LogComponentSettings& LogDomainGlobal::settings() {
    return _impl->_settings;
}

}  // namespace mongo::logv2
