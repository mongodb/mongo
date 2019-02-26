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
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

class RenamesAToB : public DocumentSourceTestOptimizations {
public:
    RenamesAToB() : DocumentSourceTestOptimizations() {}
    GetModPathsReturn getModifiedPaths() const final {
        // Pretend this stage simply renames the "a" field to be "b", leaving the value of "a" the
        // same. This would be the equivalent of an {$addFields: {b: "$a"}}.
        return {GetModPathsReturn::Type::kFiniteSet, std::set<std::string>{}, {{"b", "a"}}};
    }
};

TEST(DocumentSourceRenamedPaths, DoesReturnSimpleRenameFromFiniteSetRename) {
    RenamesAToB renamesAToB;
    auto renames = renamesAToB.renamedPaths({"b"});
    ASSERT(static_cast<bool>(renames));
    auto map = *renames;
    ASSERT_EQ(map.size(), 1UL);
    ASSERT_EQ(map["b"], "a");
}

TEST(DocumentSourceRenamedPaths, ReturnsSimpleMapForUnaffectedFieldsFromFiniteSetRename) {
    RenamesAToB renamesAToB;
    {
        auto renames = renamesAToB.renamedPaths({"c"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["c"], "c");
    }

    {
        auto renames = renamesAToB.renamedPaths({"a"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["a"], "a");
    }

    {
        auto renames = renamesAToB.renamedPaths({"e", "f", "g"});
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

TEST(DocumentSourceRenamedPaths, DoesReturnSimpleRenameFromAllExceptRename) {
    RenameCToDPreserveEFG renameCToDPreserveEFG;
    auto renames = renameCToDPreserveEFG.renamedPaths({"d"});
    ASSERT(static_cast<bool>(renames));
    auto map = *renames;
    ASSERT_EQ(map.size(), 1UL);
    ASSERT_EQ(map["d"], "c");
}

TEST(DocumentSourceRenamedPaths, ReturnsSimpleMapForUnaffectedFieldsFromAllExceptRename) {
    RenameCToDPreserveEFG renameCToDPreserveEFG;
    {
        auto renames = renameCToDPreserveEFG.renamedPaths({"e"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "e");
    }

    {
        auto renames = renameCToDPreserveEFG.renamedPaths({"f", "g"});
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

TEST(DocumentSourceRenamedPaths, DoesReturnRenameToDottedFieldFromAllExceptRename) {
    RenameCDotDToEPreserveFDotG renameCDotDToEPreserveFDotG;
    {
        auto renames = renameCDotDToEPreserveFDotG.renamedPaths({"e"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["e"], "c.d");
    }
    {
        auto renames = renameCDotDToEPreserveFDotG.renamedPaths({"e.x", "e.y"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["e.x"], "c.d.x");
        ASSERT_EQ(map["e.y"], "c.d.y");
    }
}

TEST(DocumentSourceRenamedPaths, DoesNotTreatPrefixAsUnmodifiedWhenSuffixIsModifiedFromAllExcept) {
    RenameCDotDToEPreserveFDotG renameCDotDToEPreserveFDotG;
    {
        auto renames = renameCDotDToEPreserveFDotG.renamedPaths({"f"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        // This is the exception, the only path that is not modified.
        auto renames = renameCDotDToEPreserveFDotG.renamedPaths({"f.g"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["f.g"], "f.g");
    }
    {
        // We know "f.g" is preserved, so it follows that a subpath of that path is also preserved.
        auto renames = renameCDotDToEPreserveFDotG.renamedPaths({"f.g.x", "f.g.xyz.foobarbaz"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["f.g.x"], "f.g.x");
        ASSERT_EQ(map["f.g.xyz.foobarbaz"], "f.g.xyz.foobarbaz");
    }

    {
        // This shares a prefix with the unmodified path, but should not be reported as unmodified.
        auto renames = renameCDotDToEPreserveFDotG.renamedPaths({"f.x"});
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

TEST(DocumentSourceRenamedPaths, DoesReturnRenameToDottedFieldFromFiniteSetRename) {
    RenameAToXDotYModifyCDotD renameAToXDotYModifyCDotD;
    {
        auto renames = renameAToXDotYModifyCDotD.renamedPaths({"x.y"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 1UL);
        ASSERT_EQ(map["x.y"], "a");
    }
    {
        auto renames = renameAToXDotYModifyCDotD.renamedPaths({"x.y.z", "x.y.a.b.c"});
        ASSERT(static_cast<bool>(renames));
        auto map = *renames;
        ASSERT_EQ(map.size(), 2UL);
        ASSERT_EQ(map["x.y.z"], "a.z");
        ASSERT_EQ(map["x.y.a.b.c"], "a.a.b.c");
    }
}

TEST(DocumentSourceRenamedPaths, DoesNotTreatPrefixAsUnmodifiedWhenSuffixIsPartOfModifiedSet) {
    RenameAToXDotYModifyCDotD renameAToXDotYModifyCDotD;
    {
        auto renames = renameAToXDotYModifyCDotD.renamedPaths({"c"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renameAToXDotYModifyCDotD.renamedPaths({"c.d"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renameAToXDotYModifyCDotD.renamedPaths({"c.d.e"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = renameAToXDotYModifyCDotD.renamedPaths({"c.not_d", "c.decoy"});
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

TEST(DocumentSourceRenamedPaths, ReturnsNoneWhenAllPathsAreModified) {
    ModifiesAllPaths modifiesAllPaths;
    {
        auto renames = modifiesAllPaths.renamedPaths({"a"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = modifiesAllPaths.renamedPaths({"a", "b", "c.d"});
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

TEST(DocumentSourceRenamedPaths, ReturnsNoneWhenModificationsAreNotKnown) {
    ModificationsUnknown modificationsUnknown;
    {
        auto renames = modificationsUnknown.renamedPaths({"a"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
    {
        auto renames = modificationsUnknown.renamedPaths({"a", "b", "c.d"});
        ASSERT_FALSE(static_cast<bool>(renames));
    }
}

}  // namespace
}  // namespace mongo
