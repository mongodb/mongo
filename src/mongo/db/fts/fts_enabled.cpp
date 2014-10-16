// fts_enabled.cpp

/**
*    Copyright (C) 2012 10gen Inc.
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
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommands

#include "mongo/db/server_parameters.h"

#include "mongo/util/log.h"

namespace mongo {
    namespace fts {
        namespace {

            bool dummyEnabledFlag = true; // Unused, needed for server parameter.

            /**
             * Declaration for the "textSearchEnabled" server parameter, which is now deprecated.
             * Note that:
             * - setting to true performs a no-op and logs a deprecation message.
             * - setting to false will fail.
             */
            class ExportedTextSearchEnabledParameter : public ExportedServerParameter<bool> {
            public:
                ExportedTextSearchEnabledParameter() :
                    ExportedServerParameter<bool>( ServerParameterSet::getGlobal(),
                                                   "textSearchEnabled",
                                                   &dummyEnabledFlag,
                                                   true,
                                                   true ) {}

                virtual Status validate( const bool& potentialNewValue ) {
                    if ( !potentialNewValue ) {
                        return Status( ErrorCodes::BadValue,
                                       "textSearchEnabled cannot be set to false");
                    }

                    log() << "Attempted to set textSearchEnabled server parameter.";
                    log() << "Text search is enabled by default and cannot be disabled.";
                    log() << "The following are now deprecated and will be removed in a future "
                          << "release:";
                    log() << "- the \"textSearchEnabled\" server parameter (setting it has no "
                          << "effect)";
                    log() << "- the \"text\" command (has been replaced by the $text query "
                             "operator)";

                    return Status::OK();
                }

            } exportedTextSearchEnabledParam;

        }
    }
}
