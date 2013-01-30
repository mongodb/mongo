/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
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
