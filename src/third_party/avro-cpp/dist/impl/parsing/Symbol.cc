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

#include "Symbol.hh"

namespace avro {
namespace parsing {

using std::ostringstream;
using std::string;
using std::vector;

const char *Symbol::stringValues[] = {
    "TerminalLow",
    "Null",
    "Bool",
    "Int",
    "Long",
    "Float",
    "Double",
    "String",
    "Bytes",
    "ArrayStart",
    "ArrayEnd",
    "MapStart",
    "MapEnd",
    "Fixed",
    "Enum",
    "Union",
    "TerminalHigh",
    "SizeCheck",
    "NameList",
    "Root",
    "Repeater",
    "Alternative",
    "Placeholder",
    "Indirect",
    "Symbolic",
    "EnumAdjust",
    "UnionAdjust",
    "SkipStart",
    "Resolve",
    "ImplicitActionLow",
    "RecordStart",
    "RecordEnd",
    "Field",
    "Record",
    "SizeList",
    "WriterUnion",
    "DefaultStart",
    "DefaultEnd",
    "ImplicitActionHigh",
    "Error"};

Symbol Symbol::enumAdjustSymbol(const NodePtr &writer, const NodePtr &reader) {
    vector<string> rs;
    size_t rc = reader->names();
    for (size_t i = 0; i < rc; ++i) {
        rs.push_back(reader->nameAt(i));
    }

    size_t wc = writer->names();
    vector<int> adj; // enums are encoded as ints
    adj.reserve(wc);

    vector<string> err;

    for (size_t i = 0; i < wc; ++i) {
        const string &s = writer->nameAt(i);
        vector<string>::const_iterator it = find(rs.begin(), rs.end(), s);
        if (it == rs.end()) {
            auto pos = err.size() + 1;
            adj.push_back(static_cast<int>(-pos));
            err.push_back(s);
        } else {
            adj.push_back(static_cast<int>(it - rs.begin()));
        }
    }
    return Symbol(Kind::EnumAdjust, make_pair(adj, err));
}

Symbol Symbol::error(const NodePtr &writer, const NodePtr &reader) {
    ostringstream oss;
    oss << "Cannot resolve: " << std::endl;
    writer->printJson(oss, 0);
    oss << std::endl
        << "with" << std::endl;
    reader->printJson(oss, 0);
    return Symbol(Kind::Error, oss.str());
}

} // namespace parsing
} // namespace avro
