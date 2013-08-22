/*    Copyright 2013 10gen Inc.
 *
 *    Licensed under the Apache License, Version 2.0 (the "License");
 *    you may not use this file except in compliance with the License.
 *    You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *    Unless required by applicable law or agreed to in writing, software
 *    distributed under the License is distributed on an "AS IS" BASIS,
 *    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *    See the License for the specific language governing permissions and
 *    limitations under the License.
 */

#include "mongo/platform/basic.h"

#include "mongo/logger/logstream_builder.h"

#include "mongo/base/init.h"
#include "mongo/base/owned_pointer_vector.h"
#include "mongo/base/status.h"
#include "mongo/logger/message_event_utf8_encoder.h"
#include "mongo/logger/tee.h"
#include "mongo/util/assert_util.h"  // TODO: remove apple dep for this in threadlocal.h
#include "mongo/util/concurrency/threadlocal.h"
#include "mongo/util/time_support.h"

namespace mongo {

namespace {

    /// Type of per-thread cache for storing pre-constructed ostringstreams.  While its type is
    /// vector, it should only ever contain 0 or 1 item.  It is a vector rather than just a
    /// thread_specific_ptr<> because of the high cost of thread_specific_ptr<>::reset().
    typedef OwnedPointerVector<std::ostringstream> OwnedOstreamVector;

    /// This flag indicates whether the system providing a per-thread cache of ostringstreams
    /// for use by LogstreamBuilder instances is initialized and ready for use.  Until this
    /// flag is true, LogstreamBuilder instances must not use the cache.
    bool isThreadOstreamCacheInitialized = false;

    MONGO_INITIALIZER(LogstreamBuilder)(InitializerContext*) {
        isThreadOstreamCacheInitialized = true;
        return Status::OK();
    }

}  // namespace

    TSP_DECLARE(OwnedOstreamVector, threadOstreamCache);
    TSP_DEFINE(OwnedOstreamVector, threadOstreamCache);

namespace logger {

    LogstreamBuilder::LogstreamBuilder(MessageLogDomain* domain,
                                       const std::string& contextName,
                                       LogSeverity severity)
        : _domain(domain),
          _contextName(contextName),
          _severity(severity),
          _os(NULL),
          _tee(NULL) {
    }

    LogstreamBuilder::LogstreamBuilder(logger::MessageLogDomain* domain,
                                       const std::string& contextName,
                                       LabeledLevel labeledLevel)
            : _domain(domain),
              _contextName(contextName),
              _severity(labeledLevel),
              _os(NULL),
              _tee(NULL) {

        setBaseMessage(labeledLevel.getLabel());
    }

    LogstreamBuilder::LogstreamBuilder(const LogstreamBuilder& other)
        : _domain(other._domain),
          _contextName(other._contextName),
          _severity(other._severity),
          _baseMessage(other._baseMessage),
          _os(NULL),
          _tee(NULL) {

        if (other._os || other._tee)
            abort();
    }


    LogstreamBuilder::~LogstreamBuilder() {
        if (_os) {
            if ( !_baseMessage.empty() ) _baseMessage.push_back(' ');
            _baseMessage += _os->str();
            MessageEventEphemeral message(curTimeMillis64(), _severity, _contextName, _baseMessage);
            _domain->append(message);
            if (_tee) {
                _os->str("");
                logger::MessageEventDetailsEncoder teeEncoder;
                teeEncoder.encode(message, *_os);
                _tee->write(_os->str());
            }
            _os->str("");
            if (isThreadOstreamCacheInitialized && threadOstreamCache.getMake()->vector().empty()) {
                threadOstreamCache.get()->mutableVector().push_back(_os);
            }
            else {
                delete _os;
            }
        }
    }

    void LogstreamBuilder::operator<<(Tee* tee) {
        makeStream();  // Adding a Tee counts for purposes of deciding to make a log message.
        // TODO: dassert(!_tee);
        _tee = tee;
    }

    void LogstreamBuilder::makeStream() {
        if (!_os) {
            if (isThreadOstreamCacheInitialized &&
                !threadOstreamCache.getMake()->vector().empty()) {

                std::vector<std::ostringstream*>& oses = threadOstreamCache.get()->mutableVector();
                _os = oses.back();
                oses.pop_back();
            }
            else {
                _os = new std::ostringstream;
            }
        }
    }

}  // namespace logger
}  // namespace mongo
