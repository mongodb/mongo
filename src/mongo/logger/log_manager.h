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

#pragma once

#include <boost/optional.hpp>
#include <string>

#include "mongo/logger/component_message_log_domain.h"
#include "mongo/logger/rotatable_file_writer.h"
#include "mongo/stdx/unordered_map.h"

namespace mongo {
namespace logger {

/**
 * Container for managing log domains.
 *
 * Use this while setting up the logging system, before launching any threads.
 */
class LogManager {
    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

public:
    LogManager();
    ~LogManager();

    /**
     * Gets the global domain for this manager.  It has no name.
     * Will attach a default console log appender.
     */
    ComponentMessageLogDomain* getGlobalDomain() {
        return &_globalDomain;
    }

    /**
     * Get the log domain with the given name, creating if needed.
     */
    MessageLogDomain* getNamedDomain(const std::string& name);

    /**
     * Detaches the default console log appender
     *
     * @note This function is not thread safe.
     */
    void detachDefaultConsoleAppender();

    /**
     * Reattaches the default console log appender
     *
     * @note This function is not thread safe.
     */
    void reattachDefaultConsoleAppender();

    /**
     * Checks if the default console log appender is attached
     */
    bool isDefaultConsoleAppenderAttached() const;

private:
    typedef stdx::unordered_map<std::string, MessageLogDomain*> DomainsByNameMap;

    DomainsByNameMap _domains;
    ComponentMessageLogDomain _globalDomain;
    boost::optional<ComponentMessageLogDomain::AppenderHandle> _defaultAppender;
};

}  // namespace logger
}  // namespace mongo
