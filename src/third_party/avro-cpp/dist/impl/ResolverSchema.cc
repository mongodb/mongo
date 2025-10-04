
/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ResolverSchema.hh"
#include "Resolver.hh"
#include "ValidSchema.hh"

namespace avro {

ResolverSchema::ResolverSchema(
    const ValidSchema &writerSchema,
    const ValidSchema &readerSchema,
    const Layout &readerLayout) : resolver_(constructResolver(writerSchema, readerSchema, readerLayout)) {}

void ResolverSchema::parse(Reader &reader, uint8_t *address) {
    resolver_->parse(reader, address);
}

} // namespace avro
