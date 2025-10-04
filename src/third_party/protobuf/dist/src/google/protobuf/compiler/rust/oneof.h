// Protocol Buffers - Google's data interchange format
// Copyright 2023 Google LLC.  All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GOOGLE_PROTOBUF_COMPILER_RUST_ONEOF_H__
#define GOOGLE_PROTOBUF_COMPILER_RUST_ONEOF_H__

#include "google/protobuf/compiler/rust/context.h"
#include "google/protobuf/descriptor.h"

namespace google {
namespace protobuf {
namespace compiler {
namespace rust {

void GenerateOneofDefinition(Context<OneofDescriptor> oneof);
void GenerateOneofAccessors(Context<OneofDescriptor> oneof);
void GenerateOneofExternC(Context<OneofDescriptor> oneof);
void GenerateOneofThunkCc(Context<OneofDescriptor> oneof);

}  // namespace rust
}  // namespace compiler
}  // namespace protobuf
}  // namespace google

#endif  // GOOGLE_PROTOBUF_COMPILER_RUST_ONEOF_H__
