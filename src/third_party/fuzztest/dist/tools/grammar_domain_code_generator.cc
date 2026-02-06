// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Code generator that generates the CPP header for the AST definition of a
// language. The input grammar files should have extension "g4".
//
// Usage:
// $ grammar_domain_code_generator \
//   --input_grammar_files=json.g4,morefile.g4 \
//   --output_header_file=json_grammar.h

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "./grammar_codegen/code_generation.h"
#include "./fuzztest/internal/logging.h"

ABSL_FLAG(std::string, output_header_file_path, "",
          "Required. The path of the generated grammar header file.");

ABSL_FLAG(
    std::vector<std::string>, input_grammar_files, std::vector<std::string>(),
    "Required. The nonempty list of the input grammar specification files.");

ABSL_FLAG(
    std::string, top_level_rule, "",
    "Optional. The name of the top level grammar rule. The domain "
    "generates strings of the grammar rule under this name. It is also in used"
    "the domain name.");

ABSL_FLAG(bool, insert_whitespace, false,
          "Optional. If true, spaces will be inserted between blocks of a "
          "parser production rule. This is sometimes useful if the original "
          "ANTLR grammar skips whitespaces. Generating whitespaces will help "
          "the lexer disambiguate tokens like keywords vs identifiers.");

namespace {

std::string GetContents(const std::string& path) {
  std::stringstream ss;
  ss << std::ifstream(path).rdbuf();
  return ss.str();
}
}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);

  std::string output_file_path = absl::GetFlag(FLAGS_output_header_file_path);
  FUZZTEST_INTERNAL_CHECK_PRECONDITION(
      !output_file_path.empty(),
      "You must specify output file with --output_header_file_path");
  std::ofstream output_file(output_file_path);
  FUZZTEST_INTERNAL_CHECK_PRECONDITION(output_file.is_open(),
                                       "Cannot open output file!");

  std::vector<std::string> input_files =
      absl::GetFlag(FLAGS_input_grammar_files);

  FUZZTEST_INTERNAL_CHECK_PRECONDITION(
      !input_files.empty(),
      "You must provide the list of input files, separated by ','");

  std::optional<std::string> grammar_name = std::nullopt;
  if (!absl::GetFlag(FLAGS_top_level_rule).empty()) {
    grammar_name = absl::GetFlag(FLAGS_top_level_rule);
  }

  std::vector<std::string> input_grammar_specs;
  for (const std::string& input_file : input_files) {
    FUZZTEST_INTERNAL_CHECK_PRECONDITION(std::filesystem::exists(input_file),
                                         input_file.c_str(), " not exist!");
    input_grammar_specs.push_back(GetContents(input_file));
  }
  output_file << fuzztest::internal::grammar::GenerateGrammarHeader(
      input_grammar_specs, grammar_name,
      absl::GetFlag(FLAGS_insert_whitespace));
  output_file.close();
  return 0;
}
