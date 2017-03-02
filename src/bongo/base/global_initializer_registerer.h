/*    Copyright 2012 10gen Inc.
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

#include <string>
#include <vector>

#include "bongo/base/disallow_copying.h"
#include "bongo/base/initializer_function.h"
#include "bongo/base/status.h"

namespace bongo {

/**
 * Type representing the act of registering a process-global intialization function.
 *
 * Create a module-global instance of this type to register a new initializer, to be run by a
 * call to a variant of bongo::runGlobalInitializers().  See bongo/base/initializer.h,
 * bongo/base/init.h and bongo/base/initializer_dependency_graph.h for details.
 */
class GlobalInitializerRegisterer {
    BONGO_DISALLOW_COPYING(GlobalInitializerRegisterer);

public:
    GlobalInitializerRegisterer(const std::string& name,
                                const InitializerFunction& fn,
                                const std::vector<std::string>& prerequisites,
                                const std::vector<std::string>& dependents);
};

}  // namespace bongo
