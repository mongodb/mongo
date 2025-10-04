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

#ifndef avro_SchemaResolution_hh__
#define avro_SchemaResolution_hh__

#include "Config.hh"

namespace avro {

enum SchemaResolution {

    /// The schemas definitely do not match

    RESOLVE_NO_MATCH,

    /// The schemas match at a cursory level
    ///
    /// For records and enums, this means the name is the same, but it does not
    /// necessarily mean that every symbol or field is an exact match.

    RESOLVE_MATCH,

    /// For primitives, the matching may occur if the type is promotable.  This means that the
    /// writer matches reader if the writer's type is promoted the specified type.

    //@{

    RESOLVE_PROMOTABLE_TO_LONG,
    RESOLVE_PROMOTABLE_TO_FLOAT,
    RESOLVE_PROMOTABLE_TO_DOUBLE,

    //@}

};

} // namespace avro

#endif
