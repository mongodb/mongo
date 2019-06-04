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

#include "mongo/logv2/log_domain.h"
#include "mongo/logv2/log_domain_impl.h"
#include "mongo/logv2/log_manager.h"
#include "mongo/unittest/unittest.h"

#include <boost/log/attributes/attribute_value_set.hpp>
#include <boost/log/core/record_view.hpp>
#include <boost/log/utility/formatting_ostream.hpp>

#include <boost/log/sinks.hpp>

#include <boost/make_shared.hpp>

namespace mongo {
namespace logv2 {

class LogTestV2 : public unittest::Test {

public:
    LogTestV2() {
        LogManager::global().detachDefaultBackends();
        /*auto backend = boost::make_shared<LogTestBackend>(_logLines);
        _sink = boost::make_shared<boost::log::sinks::synchronous_sink<LogTestBackend>>(
            std::move(backend));*/
    }

    virtual ~LogTestV2() {
        LogManager::global().reattachDefaultBackends();
        // LogManager::global().getGlobalDomain().impl().core()->remove_sink(_sink);
        LogManager::global().getGlobalDomain().impl().core()->remove_all_sinks();
    }

    void attach(boost::shared_ptr<boost::log::sinks::sink> sink) {
        LogManager::global().getGlobalDomain().impl().core()->add_sink(std::move(sink));
    }

    /* void attach(std::function<bool(boost::log::attribute_value_set const&)> filter,
                 std::function<void(boost::log::record_view const&,
     boost::log::formatting_ostream&)>
                     formatter) {
         _sink->set_filter(std::move(filter));
         _sink->set_formatter(std::move(formatter));
         LogManager::global().getGlobalDomain().impl().core()->add_sink(_sink);
     }

     std::string const& last() {
         return _logLines.back();
     }

     std::size_t count() {
         return _logLines.size();
     }*/

private:
    /*class LogTestBackend
        : public boost::log::sinks::
              basic_formatted_sink_backend<char, boost::log::sinks::synchronized_feeding> {
    public:
        LogTestBackend(std::vector<std::string>& logLines) : _logLines(logLines) {}

        void consume(boost::log::record_view const& rec, string_type const& formatted_string) {
            _logLines.push_back(formatted_string);
        }

    private:
        std::vector<std::string>& _logLines;
    };

    std::vector<std::string> _logLines;
    boost::shared_ptr<boost::log::sinks::synchronous_sink<LogTestBackend>> _sink;*/
};

}  // namespace logv2
}  // namespace mongo
