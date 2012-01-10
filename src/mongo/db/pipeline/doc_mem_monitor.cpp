/**
 * Copyright (c) 2011 10gen Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "db/pipeline/doc_mem_monitor.h"
#include "util/systeminfo.h"

namespace mongo {

    DocMemMonitor::DocMemMonitor(StringWriter *pW) {
        /*
          Use the default values.

          Currently, we warn in log at 5%, and assert at 10%.
        */
        size_t errorRam = SystemInfo::getPhysicalRam() / 10;
        size_t warnRam = errorRam / 2;

        init(pW, warnRam, errorRam);
    }

    DocMemMonitor::DocMemMonitor(StringWriter *pW,
                                 size_t warnLimit, size_t errorLimit) {
        init(pW, warnLimit, errorLimit);
    }

    void DocMemMonitor::addToTotal(size_t amount) {
        totalUsed += amount;

        if (!warned) {
            if (warnLimit && (totalUsed > warnLimit)) {
                stringstream ss;
                ss << "warning, 5% of physical RAM used for ";
                pWriter->writeString(ss);
                ss << endl;
                warning() << ss.str();
                warned = true;
            }
        }
        
        if (errorLimit) {
            uassert(15944, "terminating request:  request heap use exceeded 10% of physical RAM", (totalUsed <= errorLimit));
        }
    }

    void DocMemMonitor::init(StringWriter *pW,
                             size_t warnLimit, size_t errorLimit) {
        this->pWriter = pW;
        this->warnLimit = warnLimit;
        this->errorLimit = errorLimit;

        warned = false;
        totalUsed = 0;
    }
}
