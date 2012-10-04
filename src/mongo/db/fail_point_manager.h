/*
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
 */

#pragma once

#include "mongo/util/fail_point.h"

namespace mongo {
    class FailPointManager {
    public:
        /**
         * @return the global fail point registry.
         */
        static FailPointRegistry* getRegistry();

        /**
         * Installs the injectFault command.
         *
         * Note: not thread-safe
         */
        static void init();

    private:
        static FailPointRegistry _fpRegistry;
    };

    class FailPointInstaller {
    public:
        FailPointInstaller(FailPointRegistry* fpRegistry,
                string name,
                FailPoint* failPoint) {
            fpRegistry->addFailPoint(name, failPoint);
        }
    };

#define MONGO_FP_DECLARE(fp) FailPoint fp; \
    FailPointInstaller install_##fp(FailPointManager::getRegistry(), #fp, &fp);
}
