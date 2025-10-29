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


#include "MongoHeaderIncludePathCheck.h"

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

static std::optional<std::string> canonicalFromSrc(const std::filesystem::path& full) {
    std::filesystem::path rel = full.lexically_normal();

    // If absolute, try to make it relative to CWD (best effort; OK if it fails)
    if (rel.is_absolute()) {
        std::error_code ec;
        auto tmp = std::filesystem::relative(rel, std::filesystem::current_path(), ec);
        if (!ec)
            rel = std::move(tmp);
    }

    bool seenSrc = false;
    std::filesystem::path out;
    for (const auto& comp : rel) {
        // Note: on Windows, comp is a path; compare as string ignoring slashes.
        if (!seenSrc) {
            if (comp == "src")
                seenSrc = true;
        } else {
            out /= comp;
        }
    }
    if (!seenSrc || out.empty())
        return std::nullopt;
    return out.generic_string();  // forward slashes
}

// Skip canonical-path enforcement for generated protobuf headers (.pb.h, .grpc.pb.h, .pb.hpp)
static bool isGeneratedProtoHeader(llvm::StringRef spelledName, llvm::StringRef resolvedFullPath) {
    std::string spelled = spelledName.str();
    std::replace(spelled.begin(), spelled.end(), '\\', '/');
    std::string full = resolvedFullPath.str();
    std::replace(full.begin(), full.end(), '\\', '/');

    auto endsWith = [](llvm::StringRef s, llvm::StringRef suf) {
        return s.ends_with_insensitive(suf);
    };
    if (endsWith(spelled, ".pb.h") || endsWith(spelled, ".grpc.pb.h") ||
        endsWith(spelled, ".pb.hpp")) {
        return true;
    }

    auto has = [](const std::string& hay, llvm::StringRef needle) {
        return hay.find(needle.str()) != std::string::npos;
    };
    if ((has(full, ".pb.h") || has(full, ".grpc.pb.h") || has(full, ".pb.hpp"))) {
        return true;
    }
    return false;
}

inline bool isMongoPath(const std::filesystem::path& p,
                        clang::ArrayRef<llvm::StringRef> mongoSourceDirs) {
    return containsAny(llvm::StringRef(p.lexically_normal().string()), mongoSourceDirs);
}

inline bool isExcludedFromCanonicalization(llvm::StringRef spelledName,
                                           llvm::StringRef resolvedFullPath) {
    // Skip generated protos and a few allow-listed paths.
    if (isGeneratedProtoHeader(spelledName, resolvedFullPath))
        return true;
    if (spelledName.contains("bsoncxx/") || spelledName.contains("mongocxx/"))
        return true;
    if (containsAny(resolvedFullPath, llvm::ArrayRef<llvm::StringRef>{"mongo_tidy_checks/"}))
        return true;
    return false;
}

class MongoIncludeIncludePathPPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoIncludeIncludePathPPCallbacks(MongoHeaderIncludePathCheck& Check,
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
        // Resolve header path on disk (best effort).
        const std::filesystem::path headerPath =
            (std::filesystem::path(SearchPath.str()) / std::filesystem::path(RelativePath.str()))
                .lexically_normal();
        if (!std::filesystem::exists(headerPath))
            return;  // nothing to do

        // Resolve the including source file (avoid "path:line:col" by using getFilename()).
        const llvm::StringRef originFile = SM.getFilename(HashLoc);
        if (originFile.empty() || originFile.starts_with("<built-in>:"))
            return;

        // Guard clauses for scope of this check.
        std::filesystem::path originPath(originFile.str());
        if (!isMongoPath(originPath, Check.mongoSourceDirs))
            return;  // only enforce for mongo sources
        if (!isMongoPath(headerPath, Check.mongoSourceDirs))
            return;  // only canonicalize mongo headers
        if (isExcludedFromCanonicalization(FileName, llvm::StringRef(headerPath.string())))
            return;

        // Compute canonical 'src/'-relative form and compare against spelled name.
        if (auto canon = canonicalFromSrc(headerPath)) {
            std::string spelled = FileName.str();
            std::replace(spelled.begin(), spelled.end(), '\\', '/');  // normalize for compare

            if (spelled != *canon) {
                const std::string replacement =
                    (llvm::Twine("\"") + llvm::StringRef(*canon) + "\"").str();
                Check.diag(FilenameRange.getBegin(),
                           "mongo include '%0' should be referenced from 'src/' as '%1'")
                    << FileName << *canon
                    << clang::FixItHint::CreateReplacement(FilenameRange.getAsRange(), replacement);
            }
        }
    }

private:
    MongoHeaderIncludePathCheck& Check;
    clang::LangOptions LangOpts;
    const clang::SourceManager& SM;
};

}  // namespace

MongoHeaderIncludePathCheck::MongoHeaderIncludePathCheck(llvm::StringRef Name,
                                                         clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context),
      mongoSourceDirs(clang::tidy::utils::options::parseStringList(
          Options.get("mongoSourceDirs", "src/mongo"))) {}

void MongoHeaderIncludePathCheck::storeOptions(clang::tidy::ClangTidyOptions::OptionMap& Opts) {
    Options.store(
        Opts, "mongoSourceDirs", clang::tidy::utils::options::serializeStringList(mongoSourceDirs));
}
void MongoHeaderIncludePathCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                                      clang::Preprocessor* PP,
                                                      clang::Preprocessor* ModuleExpanderPP) {
    PP->addPPCallbacks(
        ::std::make_unique<MongoIncludeIncludePathPPCallbacks>(*this, getLangOpts(), SM));
}

}  // namespace mongo::tidy
