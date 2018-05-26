/*    Copyright 2018 MongoDB Inc.
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

#pragma once

#include <functional>
#include <sstream>

#include "mongo/base/status.h"
#include "mongo/logger/appender.h"
#include "mongo/logger/encoder.h"

namespace mongo {
namespace embedded {

/**
 * Appender for writing to callbacks registered with the embedded C API.
 */
template <typename Event>
class EmbeddedLogAppender : public logger::Appender<Event> {
    EmbeddedLogAppender(EmbeddedLogAppender const&) = delete;
    EmbeddedLogAppender& operator=(EmbeddedLogAppender const&) = delete;

public:
    typedef logger::Encoder<Event> EventEncoder;

    explicit EmbeddedLogAppender(
        std::function<void(void*, const char*, const char*, const char*, int)> callback,
        void* callbackUserData,
        std::unique_ptr<EventEncoder> encoder)
        : _encoder(std::move(encoder)),
          _callback(std::move(callback)),
          _callbackUserData(callbackUserData) {}

    Status append(const Event& event) final {
        std::stringstream output;
        _encoder->encode(event, output);
        _callback(_callbackUserData,
                  output.str().c_str(),
                  event.getComponent().getShortName().c_str(),
                  event.getContextName().toString().c_str(),
                  event.getSeverity().toInt());
        return Status::OK();
    }

private:
    std::unique_ptr<EventEncoder> _encoder;
    std::function<void(void*, const char*, const char*, const char*, int)> _callback;
    void* const _callbackUserData;
};

}  // namespace embedded
}  // namespace mongo
