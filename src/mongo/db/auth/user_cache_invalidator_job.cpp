/*    Copyright 2012 10gen Inc.
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

#include "mongo/pch.h"

#include "mongo/db/auth/user_cache_invalidator_job.h"

#include <string>

#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/background.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

    int userCacheInvalidationIntervalSecs = 60 * 10; // 10 minute default

    class ExportedInvalidationIntervalParameter : public ExportedServerParameter<int> {
    public:
        ExportedInvalidationIntervalParameter() :
            ExportedServerParameter<int>(ServerParameterSet::getGlobal(),
                                         "userCacheInvalidationIntervalSecs",
                                         &userCacheInvalidationIntervalSecs,
                                         true,
                                         true) {}

        virtual Status validate( const int& potentialNewValue )
        {
            if (potentialNewValue < 30 || potentialNewValue > 86400) {
                return Status(ErrorCodes::BadValue,
                              "userCacheInvalidationIntervalSecs must be between 30 " 
                              "and 86400 (24 hours)");
            }
            return Status::OK();
        }
    } exportedIntervalParam;

} // namespace

    void UserCacheInvalidator::run() {
        Client::initThread("UserCacheInvalidatorThread");
        while (true) {
            sleepsecs(userCacheInvalidationIntervalSecs);
            if (inShutdown()) {
                break;
            }
            LOG(1) << "Invalidating user cache" << endl;
            _authzManager->invalidateUserCache();
        }
    }

    std::string UserCacheInvalidator::name() const {
        return "UserCacheInvalidatorThread";
    }

} // namespace mongo
