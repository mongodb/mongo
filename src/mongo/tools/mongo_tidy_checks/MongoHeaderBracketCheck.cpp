/**
 *    Copyright (C) 2023-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "MongoHeaderBracketCheck.h"

#include <filesystem>

#include <clang-tidy/utils/OptionsUtils.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringSet.h>

namespace mongo::tidy {

namespace {

bool containsAny(llvm::StringRef fullString, clang::ArrayRef<clang::StringRef> const& patterns) {
    return std::any_of(patterns.begin(), patterns.end(), [&](llvm::StringRef pattern) {
        // Ensure the pattern ends with a directory delimiter to avoid matching partial names.
        auto patternStr = pattern.str();
        return fullString.contains(llvm::StringRef(std::filesystem::path(patternStr) / "")) ||
            // also accept relative paths
            fullString.contains(llvm::StringRef(std::filesystem::current_path() /
                                                std::filesystem::path(patternStr) / ""));
    });
}

class MongoIncludeBracketsPPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoIncludeBracketsPPCallbacks(MongoHeaderBracketCheck& Check,
                                             clang::LangOptions LangOpts,
                                             const clang::SourceManager& SM)
        : Check(Check), LangOpts(LangOpts), SM(SM) {}


    void InclusionDirective(clang::SourceLocation HashLoc,
                            const clang::Token& IncludeTok,
                            llvm::StringRef FileName,
                            bool IsAngled,
                            clang::CharSourceRange FilenameRange,
                            clang::OptionalFileEntryRef File,
                            llvm::StringRef SearchPath,
                            llvm::StringRef RelativePath,
                            const clang::Module* SuggestedModule,
                            bool ModuleImported,
                            clang::SrcMgr::CharacteristicKind FileType) override {

        // This represents the full path from the project root directory
        // to the header file that is being included.
        std::filesystem::path header_path =
            (std::filesystem::path(SearchPath.str()) / std::filesystem::path(RelativePath.str()))
                .lexically_normal();

        // This represents the full path from the project root to the
        // source file that is including the include that is currently being processed.
        std::filesystem::path origin_source_path(HashLoc.printToString(SM));
        if (origin_source_path.is_absolute()) {
            origin_source_path =
                std::filesystem::relative(origin_source_path, std::filesystem::current_path());
        }

        // if this is a mongo source file including a real file
        if (!llvm::StringRef(origin_source_path).starts_with("<built-in>:") &&
            containsAny(llvm::StringRef(origin_source_path.lexically_normal()),
                        Check.mongoSourceDirs)) {

            // Check that the a third party header which is included from a mongo source file
            // is used angle brackets.
            if (!IsAngled && !containsAny(llvm::StringRef(header_path), Check.mongoSourceDirs)) {

                std::string Replacement = (llvm::Twine("<") + FileName + ">").str();
                Check.diag(FilenameRange.getBegin(),
                           "non-mongo include '%0' should use angle brackets")
                    << FileName
                    << clang::FixItHint::CreateReplacement(FilenameRange.getAsRange(), Replacement);
            }

            // Check that the a third party header which is in our vendored tree is not including
            // the third_party in the include path.
            if (!containsAny(llvm::StringRef(header_path), Check.mongoSourceDirs) &&
                llvm::StringRef(header_path).contains("src/third_party/") &&
                FileName.contains("third_party/")) {
                Check.diag(FilenameRange.getBegin(),
                           "third_party include '%0' should not start with 'third_party/'. The "
                           "included file should be useable in either context of system or "
                           "in-tree third_party libraries.")
                    << FileName;
            }

            // Check that the a mongo header which is included from a mongo source file
            // is used double quotes.
            else if (IsAngled && containsAny(llvm::StringRef(header_path), Check.mongoSourceDirs)) {
                // TODO(SERVER-95253): Explicitly handle third party headers inside of the
                // enterprise module directory.
                if (!FileName.contains("bsoncxx/") && !FileName.contains("mongocxx/")) {
                    std::string Replacement = (llvm::Twine("\"") + FileName + "\"").str();
                    Check.diag(FilenameRange.getBegin(),
                               "mongo include '%0' should use double quotes")
                        << FileName
                        << clang::FixItHint::CreateReplacement(FilenameRange.getAsRange(),
                                                               Replacement);
                }
            }
        }
    }

private:
    MongoHeaderBracketCheck& Check;
    clang::LangOptions LangOpts;
    const clang::SourceManager& SM;
};

}  // namespace

MongoHeaderBracketCheck::MongoHeaderBracketCheck(llvm::StringRef Name,
                                                 clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context),
      mongoSourceDirs(clang::tidy::utils::options::parseStringList(
          Options.get("mongoSourceDirs", "src/mongo"))) {}

void MongoHeaderBracketCheck::storeOptions(clang::tidy::ClangTidyOptions::OptionMap& Opts) {
    Options.store(
        Opts, "mongoSourceDirs", clang::tidy::utils::options::serializeStringList(mongoSourceDirs));
}
void MongoHeaderBracketCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                                  clang::Preprocessor* PP,
                                                  clang::Preprocessor* ModuleExpanderPP) {
    PP->addPPCallbacks(
        ::std::make_unique<MongoIncludeBracketsPPCallbacks>(*this, getLangOpts(), SM));
}

}  // namespace mongo::tidy
