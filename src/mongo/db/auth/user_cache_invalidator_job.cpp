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

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/server_parameters.h"
#include "mongo/s/config.h"
#include "mongo/util/background.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

    // How often to check with the config servers whether authorization information has changed.
    int userCacheInvalidationIntervalSecs = 30; // 30 second default

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
            if (potentialNewValue < 1 || potentialNewValue > 86400) {
                return Status(ErrorCodes::BadValue,
                              "userCacheInvalidationIntervalSecs must be between 1 "
                              "and 86400 (24 hours)");
            }
            return Status::OK();
        }
    } exportedIntervalParam;

    StatusWith<OID> getCurrentCacheGeneration() {
        try {
            ConnectionString config = configServer.getConnectionString();
            ScopedDbConnection conn(config.toString(), 30);

            BSONObj result;
            conn->runCommand("admin", BSON("_getUserCacheGeneration" << 1), result);
            conn.done();

            Status status = Command::getStatusFromCommandResult(result);
            if (!status.isOK()) {
                return StatusWith<OID>(status);
            }

            return StatusWith<OID>(result["cacheGeneration"].OID());
        } catch (const DBException& e) {
            return StatusWith<OID>(e.toStatus());
        } catch (const std::exception& e) {
            return StatusWith<OID>(ErrorCodes::UnknownError, e.what());
        }
    }

} // namespace

    UserCacheInvalidator::UserCacheInvalidator(AuthorizationManager* authzManager) :
            _authzManager(authzManager) {
        _previousCacheGeneration = _authzManager->getCacheGeneration();
    }

    void UserCacheInvalidator::run() {
        Client::initThread("UserCacheInvalidatorThread");

        while (true) {
            sleepsecs(userCacheInvalidationIntervalSecs);
            if (inShutdown()) {
                break;
            }

            StatusWith<OID> currentGeneration = getCurrentCacheGeneration();
            if (!currentGeneration.isOK()) {
                if (currentGeneration.getStatus().code() == ErrorCodes::CommandNotFound) {
                    warning() << "_getUserCacheGeneration command not found on config server(s), "
                            "this most likely means you are running an outdated version of mongod "
                            "on the config servers" << std::endl;
                } else {
                    warning() << "An error occurred while fetching current user cache generation "
                            "to check if user cache needs invalidation: " <<
                            currentGeneration.getStatus() << std::endl;
                }
                // When in doubt, invalidate the cache
                _authzManager->invalidateUserCache();
            }

            if (currentGeneration.getValue() != _previousCacheGeneration) {
                log() << "User cache generation changed from " << _previousCacheGeneration <<
                        " to " << currentGeneration.getValue() << "; invalidating user cache" <<
                        std::endl;
                _authzManager->invalidateUserCache();
                _previousCacheGeneration = currentGeneration.getValue();
            }
        }
    }

    std::string UserCacheInvalidator::name() const {
        return "UserCacheInvalidatorThread";
    }

} // namespace mongo
