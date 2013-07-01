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

#include "mongo/logger/logger.h"

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/inline_decls.h"  // For MONGO_unlikely, which should really be in
                                      // mongo/platform/compiler.h

namespace mongo {
namespace logger {

    static LogManager* theGlobalLogManager;  // NULL at program start, before even static
                                             // initialization.

    static RotatableFileManager theGlobalRotatableFileManager;

    LogManager* globalLogManager() {
        if (MONGO_unlikely(!theGlobalLogManager)) {
            theGlobalLogManager = new LogManager;
        }
        return theGlobalLogManager;
    }

    RotatableFileManager* globalRotatableFileManager() {
        return &theGlobalRotatableFileManager;
    }

    /**
     * Just in case no static initializer called globalLogManager, make sure that the global log
     * manager is instantiated while we're still in a single-threaded context.
     */
    MONGO_INITIALIZER_GENERAL(GlobalLogManager, MONGO_NO_PREREQUISITES, ("default"))(
            InitializerContext*) {

        globalLogManager();
        return Status::OK();
    }

}  // namespace logger
}  // namespace mongo
