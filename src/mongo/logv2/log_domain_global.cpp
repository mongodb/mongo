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

#include "mongo/logv2/component_settings_filter.h"
#include "mongo/logv2/composite_backend.h"
#include "mongo/logv2/console.h"
#include "mongo/logv2/json_formatter.h"
#include "mongo/logv2/log_source.h"
#include "mongo/logv2/ramlog_sink.h"
#include "mongo/logv2/tagged_severity_filter.h"
#include "mongo/logv2/text_formatter.h"

#include <boost/core/null_deleter.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks.hpp>

namespace mongo {
namespace logv2 {
namespace {

class RotateCollector : public boost::log::sinks::file::collector {
public:
    explicit RotateCollector(LogDomainGlobal::ConfigurationOptions const& options)
        : _mode{options._fileRotationMode} {}

    void store_file(boost::filesystem::path const& file) override {
        if (_mode == LogDomainGlobal::ConfigurationOptions::RotationMode::kRename) {
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
    LogDomainGlobal::ConfigurationOptions::RotationMode _mode;
};

}  // namespace

void LogDomainGlobal::ConfigurationOptions::makeDisabled() {
    _consoleEnabled = false;
}

struct LogDomainGlobal::Impl {
    typedef CompositeBackend<boost::log::sinks::text_ostream_backend, RamLogSink, RamLogSink>
        ConsoleBackend;
#ifndef _WIN32
    typedef CompositeBackend<boost::log::sinks::syslog_backend, RamLogSink, RamLogSink>
        SyslogBackend;
#endif
    typedef CompositeBackend<boost::log::sinks::text_file_backend, RamLogSink, RamLogSink>
        RotatableFileBackend;

    Impl(LogDomainGlobal& parent);
    Status configure(LogDomainGlobal::ConfigurationOptions const& options);
    Status rotate();

    LogDomainGlobal& _parent;
    LogComponentSettings _settings;
    boost::shared_ptr<boost::log::sinks::unlocked_sink<ConsoleBackend>> _consoleSink;
    boost::shared_ptr<boost::log::sinks::unlocked_sink<RotatableFileBackend>> _rotatableFileSink;
#ifndef _WIN32
    boost::shared_ptr<boost::log::sinks::unlocked_sink<SyslogBackend>> _syslogSink;
#endif
};

LogDomainGlobal::Impl::Impl(LogDomainGlobal& parent) : _parent(parent) {
    auto console = boost::make_shared<ConsoleBackend>(
        boost::make_shared<boost::log::sinks::text_ostream_backend>(),
        boost::make_shared<RamLogSink>(RamLog::get("global")),
        boost::make_shared<RamLogSink>(RamLog::get("startupWarnings")));

    console->lockedBackend<0>()->add_stream(
        boost::shared_ptr<std::ostream>(&Console::out(), boost::null_deleter()));
    console->lockedBackend<0>()->auto_flush();
    console->setFilter<2>(
        TaggedSeverityFilter(_parent, {LogTag::kStartupWarnings}, LogSeverity::Warning()));

    _consoleSink =
        boost::make_shared<boost::log::sinks::unlocked_sink<ConsoleBackend>>(std::move(console));
    _consoleSink->set_filter(ComponentSettingsFilter(_parent, _settings));

    // Set default configuration
    invariant(configure({}).isOK());
}

Status LogDomainGlobal::Impl::configure(LogDomainGlobal::ConfigurationOptions const& options) {
#ifndef _WIN32
    if (options._syslogEnabled) {
        // Create a backend
        auto backend = boost::make_shared<SyslogBackend>(
            boost::make_shared<boost::log::sinks::syslog_backend>(
                boost::log::keywords::facility =
                    boost::log::sinks::syslog::make_facility(options._syslogFacility),
                boost::log::keywords::use_impl = boost::log::sinks::syslog::native),
            boost::make_shared<RamLogSink>(RamLog::get("global")),
            boost::make_shared<RamLogSink>(RamLog::get("startupWarnings")));

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

        _syslogSink =
            boost::make_shared<boost::log::sinks::unlocked_sink<SyslogBackend>>(std::move(backend));
        _syslogSink->set_filter(ComponentSettingsFilter(_parent, _settings));

        boost::log::core::get()->add_sink(_syslogSink);
    } else if (_syslogSink) {
        boost::log::core::get()->remove_sink(_syslogSink);
        _syslogSink.reset();
    }
#endif

    if (options._consoleEnabled && _consoleSink.use_count() == 1) {
        boost::log::core::get()->add_sink(_consoleSink);
    }

    if (!options._consoleEnabled && _consoleSink.use_count() > 1) {
        boost::log::core::get()->remove_sink(_consoleSink);
    }

    if (options._fileEnabled) {
        auto backend = boost::make_shared<RotatableFileBackend>(
            boost::make_shared<boost::log::sinks::text_file_backend>(
                boost::log::keywords::file_name = options._filePath),
            boost::make_shared<RamLogSink>(RamLog::get("global")),
            boost::make_shared<RamLogSink>(RamLog::get("startupWarnings")));

        backend->lockedBackend<0>()->auto_flush(true);

        backend->lockedBackend<0>()->set_file_collector(
            boost::make_shared<RotateCollector>(options));

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

    switch (options._format) {
        case LogFormat::kDefault:
        case LogFormat::kText:
            setFormatters([] { return TextFormatter(); });
            break;
        case LogFormat::kJson:
            setFormatters([] { return JSONFormatter(); });
            break;
    }

    return Status::OK();
}

Status LogDomainGlobal::Impl::rotate() {
    if (_rotatableFileSink) {
        auto backend = _rotatableFileSink->locked_backend()->lockedBackend<0>();
        backend->rotate_file();
    }
    return Status::OK();
}

LogDomainGlobal::LogDomainGlobal() {
    _impl = std::make_unique<Impl>(*this);
}

LogDomainGlobal::~LogDomainGlobal() {}

LogSource& LogDomainGlobal::source() {
    // Use a thread_local logger so we don't need to have locking
    thread_local LogSource lg(this);
    return lg;
}


Status LogDomainGlobal::configure(LogDomainGlobal::ConfigurationOptions const& options) {
    return _impl->configure(options);
}

Status LogDomainGlobal::rotate() {
    return _impl->rotate();
}

LogComponentSettings& LogDomainGlobal::settings() {
    return _impl->_settings;
}

}  // namespace logv2
}  // namespace mongo
