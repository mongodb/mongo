/**
*    Copyright (C) 2018 MongoDB Inc.
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

#pragma once

#include <functional>
#include <memory>

namespace mongo {
class ServiceContext;

// This is a initialization registration system similar to MONGO_INITIALIZER. But it cannot be an
// MONGO_INITIALIZER because we need to create the service context before the MONGO_INITIALIZERS
// execute. This class should probably be refactored to use the shim system from this ticket:
// https://jira.mongodb.org/browse/SERVER-32645
class ServiceContextRegistrar {
public:
    explicit ServiceContextRegistrar(std::function<std::unique_ptr<ServiceContext>()> fn);
};

bool hasServiceContextFactory();
std::unique_ptr<ServiceContext> createServiceContext();
}  // namespace mongo
