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

#ifndef avro_Compiler_hh__
#define avro_Compiler_hh__

#include "Config.hh"
#include <cstdint>
#include <istream>

namespace avro {

class AVRO_DECL InputStream;

/// This class is used to implement an avro spec parser using a flex/bison
/// compiler.  In order for the lexer to be reentrant, this class provides a
/// lexer object for each parse.  The bison parser also uses this class to
/// build up an avro parse tree as the avro spec is parsed.

class AVRO_DECL ValidSchema;

/// Given a stream containing a JSON schema, compiles the schema to a
/// ValidSchema object.  Throws if the schema cannot be compiled to a valid
/// schema

AVRO_DECL void compileJsonSchema(std::istream &is, ValidSchema &schema);

/// Non-throwing version of compileJsonSchema.
///
/// \return True if no error, false if error (with the error string set)
///

AVRO_DECL bool compileJsonSchema(std::istream &is, ValidSchema &schema,
                                 std::string &error);

AVRO_DECL ValidSchema compileJsonSchemaFromStream(InputStream &is);

AVRO_DECL ValidSchema compileJsonSchemaFromMemory(const uint8_t *input, size_t len);

AVRO_DECL ValidSchema compileJsonSchemaFromString(const char *input);

AVRO_DECL ValidSchema compileJsonSchemaFromString(const std::string &input);

AVRO_DECL ValidSchema compileJsonSchemaFromFile(const char *filename);

} // namespace avro

#endif
