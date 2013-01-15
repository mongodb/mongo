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

#include "mongo/scripting/v8_profiler.h"

#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
    void V8CpuProfiler::start(const StringData name) {
        v8::CpuProfiler::StartProfiling(v8::String::New(name.toString().c_str()));
    }

    void V8CpuProfiler::stop(const StringData name) {
        _cpuProfiles.insert(make_pair(name.toString(),
                v8::CpuProfiler::StopProfiling(v8::String::New(name.toString().c_str(),
                                                               name.size()))));
    }

    void V8CpuProfiler::traverseDepthFirst(const v8::CpuProfileNode* cpuProfileNode,
                                           BSONArrayBuilder& arrayBuilder) {
        if (cpuProfileNode == NULL)
            return;
        BSONObjBuilder frameObjBuilder;
        frameObjBuilder.append("Function",
                               *v8::String::Utf8Value(cpuProfileNode->GetFunctionName()));
        frameObjBuilder.append("Source",
                               *v8::String::Utf8Value(cpuProfileNode->GetScriptResourceName()));
        frameObjBuilder.appendNumber("Line", cpuProfileNode->GetLineNumber());
        frameObjBuilder.appendNumber("SelfTime", cpuProfileNode->GetSelfTime());
        frameObjBuilder.appendNumber("TotalTime", cpuProfileNode->GetTotalTime());
        if (cpuProfileNode->GetChildrenCount()) {
            BSONArrayBuilder subArrayBuilder(frameObjBuilder.subarrayStart("Children"));
            for (int i = 0; i < cpuProfileNode->GetChildrenCount(); ++i) {
                traverseDepthFirst(cpuProfileNode->GetChild(i), subArrayBuilder);
            }
            subArrayBuilder.done();
        }
        arrayBuilder << frameObjBuilder.obj();
    }

    const BSONArray V8CpuProfiler::fetch(const StringData name) {
        BSONArrayBuilder arrayBuilder;
        CpuProfileMap::const_iterator iProf = _cpuProfiles.find(name.toString());
        if (iProf == _cpuProfiles.end())
            return arrayBuilder.arr();
        const v8::CpuProfile* cpuProfile = iProf->second;
        if (cpuProfile == NULL)
            return arrayBuilder.arr();
        traverseDepthFirst(cpuProfile->GetTopDownRoot(), arrayBuilder);
        return arrayBuilder.arr();
    }
}
