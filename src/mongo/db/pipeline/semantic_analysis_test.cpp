/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_test_optimizations.h"
#include "mongo/db/pipeline/semantic_analysis.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

using namespace semantic_analysis;

class RenamesAToB : public DocumentSourceTestOptimizations {
public:
    RenamesAToB() : DocumentSourceTestOptimizations() {}
    GetModPathsReturn getModifiedPaths() const final {
        // Pretend this stage simply renames the "a" field to be "b", leaving the value of "a" the
        // same. This would be the equivalent of an {$addFields: {b: "$a"}}.
        return {GetModPathsReturn::Type::kFiniteSet, std::set<std::string>{}, {{"b", "a"}}};
    }
};

TEST(SemanticAnalysisRenamedPaths, DoesReturnSimpleRenameFromFiniteSetRename) {
    RenamesAToB renamesAToB;
    {
        auto renames = renamedPaths({"a"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["a"], "b");
    }
    {
        auto renames = renamedPaths({"b"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["b"], "a");
    }
}

TEST(SemanticAnalysisRenamedPaths, ReturnsSimpleMapForUnaffectedFieldsFromFiniteSetRename) {
    RenamesAToB renamesAToB;
    {
        auto renames = renamedPaths({"c"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c"], "c");
    }
    {
        auto renames = renamedPaths({"c"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c"], "c");
    }
    {
        auto renames = renamedPaths({"a"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["a"], "b");
    }
    {
        auto renames = renamedPaths({"b"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["b"], "a");
    }
    {
        auto renames = renamedPaths({"e", "f", "g"}, renamesAToB, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 3UL);
        ASSERT_EQ(map["e"], "e");
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
    {
        auto renames = renamedPaths({"e", "f", "g"}, renamesAToB, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 3UL);
        ASSERT_EQ(map["e"], "e");
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
}

class RenameCToDPreserveEFG : public DocumentSourceTestOptimizations {
public:
    RenameCToDPreserveEFG() : DocumentSourceTestOptimizations() {}

    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllExcept,
                std::set<std::string>{"e", "f", "g"},
                {{"d", "c"}}};
    }
};

TEST(SemanticAnalysisRenamedPaths, DoesReturnSimpleRenameFromAllExceptRename) {
    RenameCToDPreserveEFG renameCToDPreserveEFG;
    {
        auto renames = renamedPaths({"c"}, renameCToDPreserveEFG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c"], "d");
    }
    {
        auto renames = renamedPaths({"d"}, renameCToDPreserveEFG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["d"], "c");
    }
}

TEST(SemanticAnalysisRenamedPaths, ReturnsSimpleMapForUnaffectedFieldsFromAllExceptRename) {
    RenameCToDPreserveEFG renameCToDPreserveEFG;
    {
        auto renames = renamedPaths({"e"}, renameCToDPreserveEFG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "e");
    }
    {
        auto renames = renamedPaths({"e"}, renameCToDPreserveEFG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "e");
    }
    {
        auto renames = renamedPaths({"f", "g"}, renameCToDPreserveEFG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
    {
        auto renames = renamedPaths({"f", "g"}, renameCToDPreserveEFG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f"], "f");
        ASSERT_EQ(map["g"], "g");
    }
}

class RenameCDotDToEPreserveFDotG : public DocumentSourceTestOptimizations {
public:
    RenameCDotDToEPreserveFDotG() : DocumentSourceTestOptimizations() {}

    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllExcept, std::set<std::string>{"f.g"}, {{"e", "c.d"}}};
    }
};

TEST(SemanticAnalysisRenamedPaths, DoesReturnRenameToDottedFieldFromAllExceptRename) {
    RenameCDotDToEPreserveFDotG renameCDotDToEPreserveFDotG;
    {
        auto renames = renamedPaths({"c.d"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c.d"], "e");
    }
    {
        auto renames = renamedPaths({"e"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "c.d");
    }
    {
        auto renames =
            renamedPaths({"c.d.x", "c.d.y"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["c.d.x"], "e.x");
        ASSERT_EQ(map["c.d.y"], "e.y");
    }
    {
        auto renames =
            renamedPaths({"e.x", "e.y"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["e.x"], "c.d.x");
        ASSERT_EQ(map["e.y"], "c.d.y");
    }
}

TEST(SemanticAnalysisRenamedPaths,
     DoesNotTreatPrefixAsUnmodifiedWhenSuffixIsModifiedFromAllExcept) {
    RenameCDotDToEPreserveFDotG renameCDotDToEPreserveFDotG;
    {
        auto renames = renamedPaths({"f"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"f"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        // This is the exception, the only path that is not modified.
        auto renames = renamedPaths({"f.g"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["f.g"], "f.g");
    }
    {
        // This is the exception, the only path that is not modified.
        auto renames = renamedPaths({"f.g"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["f.g"], "f.g");
    }
    {
        // We know "f.g" is preserved, so it follows that a subpath of that path is also preserved.
        auto renames = renamedPaths(
            {"f.g.x", "f.g.xyz.foobarbaz"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f.g.x"], "f.g.x");
        ASSERT_EQ(map["f.g.xyz.foobarbaz"], "f.g.xyz.foobarbaz");
    }
    {
        auto renames = renamedPaths(
            {"f.g.x", "f.g.xyz.foobarbaz"}, renameCDotDToEPreserveFDotG, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f.g.x"], "f.g.x");
        ASSERT_EQ(map["f.g.xyz.foobarbaz"], "f.g.xyz.foobarbaz");
    }
    {
        // This shares a prefix with the unmodified path, but should not be reported as unmodified.
        auto renames = renamedPaths({"f.x"}, renameCDotDToEPreserveFDotG, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

class RenameAToXDotYModifyCDotD : public DocumentSourceTestOptimizations {
public:
    RenameAToXDotYModifyCDotD() : DocumentSourceTestOptimizations() {}

    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kFiniteSet, std::set<std::string>{"c.d"}, {{"x.y", "a"}}};
    }
};

TEST(SemanticAnalysisRenamedPaths, DoesReturnRenameToDottedFieldFromFiniteSetRename) {
    RenameAToXDotYModifyCDotD renameAToXDotYModifyCDotD;
    {
        auto renames = renamedPaths({"a"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["a"], "x.y");
    }
    {
        auto renames = renamedPaths({"x.y"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["x.y"], "a");
    }
    {
        auto renames =
            renamedPaths({"a.z", "a.a.b.c"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["a.z"], "x.y.z");
        ASSERT_EQ(map["a.a.b.c"], "x.y.a.b.c");
    }
    {
        auto renames =
            renamedPaths({"x.y.z", "x.y.a.b.c"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["x.y.z"], "a.z");
        ASSERT_EQ(map["x.y.a.b.c"], "a.a.b.c");
    }
}

TEST(SemanticAnalysisRenamedPaths, DoesNotTreatPrefixAsUnmodifiedWhenSuffixIsPartOfModifiedSet) {
    RenameAToXDotYModifyCDotD renameAToXDotYModifyCDotD;
    {
        auto renames = renamedPaths({"c"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d.e"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"c.d.e"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames =
            renamedPaths({"c.not_d", "c.decoy"}, renameAToXDotYModifyCDotD, Direction::kForward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["c.not_d"], "c.not_d");
        ASSERT_EQ(map["c.decoy"], "c.decoy");
    }
    {
        auto renames =
            renamedPaths({"c.not_d", "c.decoy"}, renameAToXDotYModifyCDotD, Direction::kBackward);
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["c.not_d"], "c.not_d");
        ASSERT_EQ(map["c.decoy"], "c.decoy");
    }
}

class ModifiesAllPaths : public DocumentSourceTestOptimizations {
public:
    ModifiesAllPaths() : DocumentSourceTestOptimizations() {}
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kAllPaths, std::set<std::string>{}, {}};
    }
};

TEST(SemanticAnalysisRenamedPaths, ReturnsNoneWhenAllPathsAreModified) {
    ModifiesAllPaths modifiesAllPaths;
    {
        auto renames = renamedPaths({"a"}, modifiesAllPaths, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modifiesAllPaths, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a"}, modifiesAllPaths, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modifiesAllPaths, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

class ModificationsUnknown : public DocumentSourceTestOptimizations {
public:
    ModificationsUnknown() : DocumentSourceTestOptimizations() {}
    GetModPathsReturn getModifiedPaths() const final {
        return {GetModPathsReturn::Type::kNotSupported, std::set<std::string>{}, {}};
    }
};

TEST(SemanticAnalysisRenamedPaths, ReturnsNoneWhenModificationsAreNotKnown) {
    ModificationsUnknown modificationsUnknown;
    {
        auto renames = renamedPaths({"a"}, modificationsUnknown, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modificationsUnknown, Direction::kForward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a"}, modificationsUnknown, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renamedPaths({"a", "b", "c.d"}, modificationsUnknown, Direction::kBackward);
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

}  // namespace
}  // namespace mongo
