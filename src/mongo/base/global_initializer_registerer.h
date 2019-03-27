/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <string>
#include <vector>

#include "mongo/base/initializer_function.h"
#include "mongo/base/status.h"

/**
* Convenience parameter representing the default set of dependents for initializer functions. Should
* just be used internally, for MONGO_INITIALIZER macros, use MONGO_DEFAULT_PREREQUISITES from init.h
* instead.
*/
#define MONGO_DEFAULT_PREREQUISITES_STR "default"

namespace mongo {

/**
 * Type representing the act of registering a process-global intialization function.
 *
 * Create a module-global instance of this type to register a new initializer, to be run by a
 * call to a variant of mongo::runGlobalInitializers().  See mongo/base/initializer.h,
 * mongo/base/init.h and mongo/base/initializer_dependency_graph.h for details.
 */
class GlobalInitializerRegisterer {
    GlobalInitializerRegisterer(const GlobalInitializerRegisterer&) = delete;
    GlobalInitializerRegisterer& operator=(const GlobalInitializerRegisterer&) = delete;

public:
    /**
    * Defines an initializer function that depends on default prerequisites and has no explicit
    * dependents.
    *
    * Does not support deinitialization and will never be re-initialized.
    *
    * Usage:
    *    GlobalInitializerRegisterer myRegsisterer(
    *            "myInitializer",
    *            [](InitializerContext* context) {
    *                // initialization code
    *                return Status::OK();
    *            }
    *    );
    */
    GlobalInitializerRegisterer(std::string name, InitializerFunction initFn);

    /**
    * Defines an initializer function that depends on PREREQUISITES and has no explicit dependents.
    *
    * At run time, the full set of prerequisites for NAME will be computed as the union of the
    * explicit PREREQUISITES.
    *
    * Does not support deinitialization and will never be re-initialized.
    *
    * Usage:
    *    GlobalInitializerRegisterer myRegsisterer(
    *            "myInitializer",
    *            {"myPrereq1", "myPrereq2", ...},
    *            [](InitializerContext* context) {
    *                // initialization code
    *                return Status::OK();
    *            }
    *    );
    */
    GlobalInitializerRegisterer(std::string name,
                                std::vector<std::string> prerequisites,
                                InitializerFunction initFn);

    /**
    * Defines an initializer function that depends on PREREQUISITES and has DEPENDENTS as explicit
    * dependents.
    *
    * At run time, the full set of prerequisites for NAME will be computed as the union of the
    * explicit PREREQUISITES and the set of all other mongo initializers that name NAME in their
    * list of dependents.
    *
    * Does not support deinitialization and will never be re-initialized.
    *
    * Usage:
    *    GlobalInitializerRegisterer myRegsisterer(
    *            "myInitializer",
    *            {"myPrereq1", "myPrereq2", ...},
    *            {"myDependent1", "myDependent2", ...},
    *            [](InitializerContext* context) {
    *                // initialization code
    *                return Status::OK();
    *            }
    *    );
    */
    GlobalInitializerRegisterer(std::string name,
                                std::vector<std::string> prerequisites,
                                std::vector<std::string> dependents,
                                InitializerFunction initFn);

    /**
    * Defines an initializer function that depends on default prerequisites and has no explicit
    * dependents.
    * It also provides a deinitialization that will execute in reverse order from initialization and
    * support re-initialization.
    *
    * Usage:
    *    GlobalInitializerRegisterer myRegsisterer(
    *            "myInitializer",
    *            [](InitializerContext* context) {
    *                // initialization code
    *                return Status::OK();
    *            },
    *            [](DeinitializerContext* context) {
    *                // deinitialization code
    *                return Status::OK();
    *            }
    *    );
    */
    GlobalInitializerRegisterer(std::string name,
                                InitializerFunction initFn,
                                DeinitializerFunction deinitFn);

    /**
    * Defines an initializer function that depends on PREREQUISITES and has no explicit dependents.
    *
    * At run time, the full set of prerequisites for NAME will be computed as the union of the
    * explicit PREREQUISITES.
    *
    * It also provides a deinitialization that will execute in reverse order from initialization and
    * support re-initialization.
    *
    * Usage:
    *    GlobalInitializerRegisterer myRegsisterer(
    *            "myInitializer",
    *            {"myPrereq1", "myPrereq2", ...},
    *            [](InitializerContext* context) {
    *                // initialization code
    *                return Status::OK();
    *            },
    *            [](DeinitializerContext* context) {
    *                // deinitialization code
    *                return Status::OK();
    *            }
    *    );
    */
    GlobalInitializerRegisterer(std::string name,
                                std::vector<std::string> prerequisites,
                                InitializerFunction initFn,
                                DeinitializerFunction deinitFn);

    /**
    * Defines an initializer function that depends on PREREQUISITES and has DEPENDENTS as explicit
    * dependents.
    *
    * At run time, the full set of prerequisites for NAME will be computed as the union of the
    * explicit PREREQUISITES and the set of all other mongo initializers that name NAME in their
    * list of dependents.
    *
    * It also provides a deinitialization that will execute in reverse order from initialization and
    * support re-initialization.
    *
    * Usage:
    *    GlobalInitializerRegisterer myRegsisterer(
    *            "myInitializer",
    *            {"myPrereq1", "myPrereq2", ...},
    *            {"myDependent1", "myDependent2", ...},
    *            [](InitializerContext* context) {
    *                // initialization code
    *                return Status::OK();
    *            },
    *            [](DeinitializerContext* context) {
    *                // deinitialization code
    *                return Status::OK();
    *            }
    *    );
    */
    GlobalInitializerRegisterer(std::string name,
                                std::vector<std::string> prerequisites,
                                std::vector<std::string> dependents,
                                InitializerFunction initFn,
                                DeinitializerFunction deinitFn);
};

inline GlobalInitializerRegisterer::GlobalInitializerRegisterer(std::string name,
                                                                InitializerFunction initFn)
    : GlobalInitializerRegisterer(std::move(name),
                                  {MONGO_DEFAULT_PREREQUISITES_STR},
                                  {},
                                  std::move(initFn),
                                  DeinitializerFunction()) {}

inline GlobalInitializerRegisterer::GlobalInitializerRegisterer(
    std::string name, std::vector<std::string> prerequisites, InitializerFunction initFn)
    : GlobalInitializerRegisterer(std::move(name),
                                  std::move(prerequisites),
                                  {},
                                  std::move(initFn),
                                  DeinitializerFunction()) {}

inline GlobalInitializerRegisterer::GlobalInitializerRegisterer(
    std::string name,
    std::vector<std::string> prerequisites,
    std::vector<std::string> dependents,
    InitializerFunction initFn)
    : GlobalInitializerRegisterer(std::move(name),
                                  std::move(prerequisites),
                                  std::move(dependents),
                                  std::move(initFn),
                                  DeinitializerFunction()) {}

inline GlobalInitializerRegisterer::GlobalInitializerRegisterer(std::string name,
                                                                InitializerFunction initFn,
                                                                DeinitializerFunction deinitFn)
    : GlobalInitializerRegisterer(std::move(name),
                                  {MONGO_DEFAULT_PREREQUISITES_STR},
                                  {},
                                  std::move(initFn),
                                  std::move(deinitFn)) {}

inline GlobalInitializerRegisterer::GlobalInitializerRegisterer(
    std::string name,
    std::vector<std::string> prerequisites,
    InitializerFunction initFn,
    DeinitializerFunction deinitFn)
    : GlobalInitializerRegisterer(
          std::move(name), std::move(prerequisites), {}, std::move(initFn), std::move(deinitFn)) {}

}  // namespace mongo
