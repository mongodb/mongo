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
#pragma once

#include <clang-tidy/ClangTidy.h>
#include <clang-tidy/ClangTidyCheck.h>

namespace mongo::tidy {

/**
 * MongoMacroDefinitionLeaksCheck is a custom clang-tidy check for detecting
 * the imbalance between the definitions and undefinitions of the macro
 * "MONGO_LOGV2_DEFAULT_COMPONENT" in the same file.
 *
 * It extends ClangTidyCheck and overrides the registerPPCallbacks function. The registerPPCallbacks
 * function adds a custom Preprocessor callback class (MongoMacroPPCallbacks) to handle
 * preprocessor events and detect an imbalance in the definitions and undefinitions of the
 * "MONGO_LOGV2_DEFAULT_COMPONENT" macro within each file.
 *
 * If a .h or .hpp file is found to have a non-zero difference between definitions and undefinitions
 * of the "MONGO_LOGV2_DEFAULT_COMPONENT" macro, it's considered a leak and the check raises a
 * diagnostic message pointing out the location of the last macro definition.
 */
class MongoMacroDefinitionLeaksCheck : public clang::tidy::ClangTidyCheck {
public:
    MongoMacroDefinitionLeaksCheck(clang::StringRef Name, clang::tidy::ClangTidyContext* Context);
    void registerPPCallbacks(const clang::SourceManager& SM,
                             clang::Preprocessor* PP,
                             clang::Preprocessor* ModuleExpanderPP) override;
};

}  // namespace mongo::tidy
