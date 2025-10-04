/*
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

#ifndef avro_Resolver_hh__
#define avro_Resolver_hh__

#include <boost/noncopyable.hpp>
#include <cstdint>
#include <memory>

#include "Config.hh"
#include "Reader.hh"

/// \file Resolver.hh
///

namespace avro {

class ValidSchema;
class Layout;

class AVRO_DECL Resolver : private boost::noncopyable {
public:
    virtual void parse(Reader &reader, uint8_t *address) const = 0;
    virtual ~Resolver() = default;
};

std::unique_ptr<Resolver> constructResolver(
    const ValidSchema &writerSchema,
    const ValidSchema &readerSchema,
    const Layout &readerLayout);

} // namespace avro

#endif
