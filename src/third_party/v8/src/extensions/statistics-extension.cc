// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "statistics-extension.h"

namespace v8 {
namespace internal {

const char* const StatisticsExtension::kSource =
    "native function getV8Statistics();";


v8::Handle<v8::FunctionTemplate> StatisticsExtension::GetNativeFunction(
    v8::Handle<v8::String> str) {
  ASSERT(strcmp(*v8::String::AsciiValue(str), "getV8Statistics") == 0);
  return v8::FunctionTemplate::New(StatisticsExtension::GetCounters);
}


v8::Handle<v8::Value> StatisticsExtension::GetCounters(
    const v8::Arguments& args) {
  Isolate* isolate = Isolate::Current();
  Heap* heap = isolate->heap();
  if (args.Length() > 0) {  // GC if first argument evaluates to true.
    if (args[0]->IsBoolean() && args[0]->ToBoolean()->Value()) {
      heap->CollectAllGarbage(Heap::kNoGCFlags, "counters extension");
    }
  }

  Counters* counters = isolate->counters();
  v8::Local<v8::Object> result = v8::Object::New();

  StatsCounter* counter = NULL;

#define ADD_COUNTER(name, caption)                                             \
  counter = counters->name();                                                  \
  if (counter->Enabled())                                                      \
    result->Set(v8::String::New(#name),                                        \
        v8::Number::New(*counter->GetInternalPointer()));

  STATS_COUNTER_LIST_1(ADD_COUNTER)
  STATS_COUNTER_LIST_2(ADD_COUNTER)
#undef ADD_COUNTER
#define ADD_COUNTER(name)                                                      \
  counter = counters->count_of_##name();                                       \
  if (counter->Enabled())                                                      \
    result->Set(v8::String::New("count_of_" #name),                            \
        v8::Number::New(*counter->GetInternalPointer()));                      \
  counter = counters->size_of_##name();                                        \
  if (counter->Enabled())                                                      \
    result->Set(v8::String::New("size_of_" #name),                             \
        v8::Number::New(*counter->GetInternalPointer()));

  INSTANCE_TYPE_LIST(ADD_COUNTER)
#undef ADD_COUNTER
#define ADD_COUNTER(name)                                                      \
  result->Set(v8::String::New("count_of_CODE_TYPE_" #name),                    \
      v8::Number::New(                                                         \
          *counters->count_of_CODE_TYPE_##name()->GetInternalPointer()));      \
  result->Set(v8::String::New("size_of_CODE_TYPE_" #name),                     \
        v8::Number::New(                                                       \
            *counters->size_of_CODE_TYPE_##name()->GetInternalPointer()));

  CODE_KIND_LIST(ADD_COUNTER)
#undef ADD_COUNTER
#define ADD_COUNTER(name)                                                      \
  result->Set(v8::String::New("count_of_FIXED_ARRAY_" #name),                  \
      v8::Number::New(                                                         \
          *counters->count_of_FIXED_ARRAY_##name()->GetInternalPointer()));    \
  result->Set(v8::String::New("size_of_FIXED_ARRAY_" #name),                   \
        v8::Number::New(                                                       \
            *counters->size_of_FIXED_ARRAY_##name()->GetInternalPointer()));

  FIXED_ARRAY_SUB_INSTANCE_TYPE_LIST(ADD_COUNTER)
#undef ADD_COUNTER

  return result;
}


void StatisticsExtension::Register() {
  static StatisticsExtension statistics_extension;
  static v8::DeclareExtension declaration(&statistics_extension);
}

} }  // namespace v8::internal
