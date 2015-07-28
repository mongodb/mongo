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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/scripting/v8-3.25_profiler.h"

#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
void V8CpuProfiler::start(v8::Isolate* isolate, const StringData name) {
    isolate->GetCpuProfiler()->StartCpuProfiling(
        v8::String::NewFromUtf8(isolate, name.toString().c_str()));
}

void V8CpuProfiler::stop(v8::Isolate* isolate, const StringData name) {
    _cpuProfiles.insert(
        make_pair(name.toString(),
                  isolate->GetCpuProfiler()->StopCpuProfiling(v8::String::NewFromUtf8(
                      isolate, name.toString().c_str(), v8::String::kNormalString, name.size()))));
}

void V8CpuProfiler::traverseDepthFirst(const v8::CpuProfileNode* cpuProfileNode,
                                       BSONArrayBuilder& arrayBuilder) {
    if (cpuProfileNode == NULL)
        return;
    BSONObjBuilder frameObjBuilder;
    frameObjBuilder.append("Function", *v8::String::Utf8Value(cpuProfileNode->GetFunctionName()));
    frameObjBuilder.append("Source",
                           *v8::String::Utf8Value(cpuProfileNode->GetScriptResourceName()));
    frameObjBuilder.appendNumber("Line", cpuProfileNode->GetLineNumber());
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
