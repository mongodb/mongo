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


#include "MongoCxx20BannedIncludesCheck.h"

#include <clang/Lex/PPCallbacks.h>
#include <clang/Lex/Preprocessor.h>

namespace mongo::tidy {

namespace {

class MongoCxx20BannedIncludesPPCallbacks : public clang::PPCallbacks {
public:
    explicit MongoCxx20BannedIncludesPPCallbacks(MongoCxx20BannedIncludesCheck& Check,
                                                 clang::LangOptions LangOpts)
        : Check(Check), LangOpts(LangOpts) {}

    void InclusionDirective(clang::SourceLocation HashLoc,
                            const clang::Token& IncludeTok,
                            llvm::StringRef FileName,
                            bool IsAngled,
                            clang::CharSourceRange FilenameRange,
                            const clang::FileEntry* File,
                            llvm::StringRef SearchPath,
                            llvm::StringRef RelativePath,
                            const clang::Module* Imported,
                            clang::SrcMgr::CharacteristicKind FileType) override {

        if (FileName.equals("coroutine") || FileName.equals("format")) {
            Check.diag(FilenameRange.getBegin(),
                       "Use of prohibited %0%1%2 header. There are no override waivers issued for "
                       "this header. For questions please reach out to #cxx-discuss.")
                << (IsAngled ? "<" : "\"") << FileName << (IsAngled ? ">" : "\"");
        }

        if (FileName.equals("syncstream") || FileName.equals("ranges") ||
            FileName.equals("barrier") || FileName.equals("latch") ||
            FileName.equals("semaphore")) {
            Check.diag(FilenameRange.getBegin(),
                       "Use of prohibited %0%1%2 header. There are override waivers issued for "
                       "this header. You need to follow the process in PM-3140 to override this "
                       "warning. For questions please reach out to #cxx-discuss.")
                << (IsAngled ? "<" : "\"") << FileName << (IsAngled ? ">" : "\"");
        }
    }

private:
    MongoCxx20BannedIncludesCheck& Check;
    clang::LangOptions LangOpts;
};

}  // namespace

MongoCxx20BannedIncludesCheck::MongoCxx20BannedIncludesCheck(llvm::StringRef Name,
                                                             clang::tidy::ClangTidyContext* Context)
    : ClangTidyCheck(Name, Context) {}

void MongoCxx20BannedIncludesCheck::registerPPCallbacks(const clang::SourceManager& SM,
                                                        clang::Preprocessor* PP,
                                                        clang::Preprocessor* ModuleExpanderPP) {
    PP->addPPCallbacks(
        ::std::make_unique<MongoCxx20BannedIncludesPPCallbacks>(*this, getLangOpts()));
}

}  // namespace mongo::tidy
