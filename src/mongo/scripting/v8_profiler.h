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

#pragma once

#include <v8.h>
#include <v8-profiler.h>
#include <map>

#include "mongo/base/string_data.h"
#include "mongo/db/jsobj.h"

namespace mongo {

    /** Collect CPU Profiling data from v8. */
    class V8CpuProfiler {
    public:
        /** Start the CPU profiler */
        void start(const StringData name);

        /** Stop the CPU profiler */
        void stop(const StringData name);

        /** Get the current cpu profile */
        const BSONArray fetch(const StringData name);
    private:
        void traverseDepthFirst(const v8::CpuProfileNode* cpuProfileNode,
                                BSONArrayBuilder& arrayBuilder);

        typedef std::map<std::string, const v8::CpuProfile*> CpuProfileMap;
        CpuProfileMap _cpuProfiles;
    };

}
