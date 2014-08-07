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

#pragma once

#include <boost/thread/mutex.hpp>
#include <iosfwd>

#include "mongo/logger/logstream_builder.h"

namespace mongo {

    /**
     * This is a version of the Console class that uses stderr for output instead of stdout.  See
     * the description of the Console class for other details about how this class should operate
     */
    class ErrorConsole {
    public:
        ErrorConsole();

        std::ostream& out();

    private:
        boost::unique_lock<boost::mutex> _consoleLock;
    };

    using logger::LogstreamBuilder;

    /*
     * Informational messages.  Messages sent here will go to stdout normally, stderr if data is
     * being sent to stdout, and be silenced if the user specifies --quiet.
     */
    LogstreamBuilder toolInfoOutput();
    /*
     * Informational messages.  Messages sent here will go to stdout normally, stderr if data is
     * being sent to stdout, and be silenced if the user specifies --quiet.  Incudes extra log
     * decoration.
     */
    LogstreamBuilder toolInfoLog();
    /*
     * Error messages.  Messages sent here should always go to stderr and not be silenced by
     * --quiet.
     */
    LogstreamBuilder toolError();

}  // namespace mongo
