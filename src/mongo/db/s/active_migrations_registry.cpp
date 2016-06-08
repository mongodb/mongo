/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/db/s/active_migrations_registry.h"

#include "mongo/base/status_with.h"
#include "mongo/util/assert_util.h"

namespace mongo {

ActiveMigrationsRegistry::ActiveMigrationsRegistry() = default;

ActiveMigrationsRegistry::~ActiveMigrationsRegistry() {
    invariant(!_activeMoveChunkRequest);
}

StatusWith<ScopedRegisterMigration> ActiveMigrationsRegistry::registerMigration(
    const MoveChunkRequest& args) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_activeMoveChunkRequest) {
        return {
            ErrorCodes::ConflictingOperationInProgress,
            str::stream()
                << "Unable start new migration, because there is already an active migration for "
                << _activeMoveChunkRequest->getNss().ns()};
    }

    _activeMoveChunkRequest = std::move(args);
    return {ScopedRegisterMigration(this)};
}

boost::optional<NamespaceString> ActiveMigrationsRegistry::getActiveMigrationNss() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    if (_activeMoveChunkRequest) {
        return _activeMoveChunkRequest->getNss();
    }

    return boost::none;
}

void ActiveMigrationsRegistry::_clearMigration() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    invariant(_activeMoveChunkRequest);
    _activeMoveChunkRequest.reset();
}

ScopedRegisterMigration::ScopedRegisterMigration(ActiveMigrationsRegistry* registry)
    : _registry(registry) {}

ScopedRegisterMigration::~ScopedRegisterMigration() {
    if (_registry) {
        _registry->_clearMigration();
    }
}

ScopedRegisterMigration::ScopedRegisterMigration(ScopedRegisterMigration&& other) {
    *this = std::move(other);
}

ScopedRegisterMigration& ScopedRegisterMigration::operator=(ScopedRegisterMigration&& other) {
    _registry = other._registry;
    other._registry = nullptr;
    return *this;
}

}  // namespace mongo
