/*    Copyright 2013 10gen Inc.
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

namespace {
    // During unittests, where we don't use quickExit(), static finalization may destroy the
    // cache before its last use, so mark it as not initialized in that case.
    // This must be after the TSP_DEFINE so that it is destroyed first.
    struct ThreadOstreamCacheFinalizer {
        ~ThreadOstreamCacheFinalizer() {
            isThreadOstreamCacheInitialized = false;
        }
    } threadOstreamCacheFinalizer;
} // namespace

namespace logger {

    LogstreamBuilder::LogstreamBuilder(MessageLogDomain* domain,
                                       const std::string& contextName,
                                       LogSeverity severity)
        : _domain(domain),
          _contextName(contextName),
          _severity(severity),
          _component(LogComponent::kDefault),
          _os(NULL),
          _tee(NULL) {
    }

    LogstreamBuilder::LogstreamBuilder(MessageLogDomain* domain,
                                       const std::string& contextName,
                                       LogSeverity severity,
                                       LogComponent component)
        : _domain(domain),
          _contextName(contextName),
          _severity(severity),
          _component(component),
          _os(NULL),
          _tee(NULL) {
    }

    LogstreamBuilder::LogstreamBuilder(logger::MessageLogDomain* domain,
                                       const std::string& contextName,
                                       LabeledLevel labeledLevel)
            : _domain(domain),
              _contextName(contextName),
              _severity(labeledLevel),
              _component(LogComponent::kDefault),
              _os(NULL),
              _tee(NULL) {

        setBaseMessage(labeledLevel.getLabel());
    }

    LogstreamBuilder::LogstreamBuilder(const LogstreamBuilder& other)
        : _domain(other._domain),
          _contextName(other._contextName),
          _severity(other._severity),
          _component(other._component),
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
            MessageEventEphemeral message(curTimeMillis64(), _severity, _component, _contextName,
                                          _baseMessage);
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
