/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/commands/server_status_metric.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"

#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

bool falseNodesPredicate(const BSONElement& el) {
    return el.type() == BSONType::boolean && !el.boolean();
}

template <typename F>
void forEachPermutationOf(const std::vector<std::string>& seq, const F& func) {
    std::vector<int> order(seq.size(), 0);
    std::iota(order.begin(), order.end(), 0);
    do {
        std::vector<std::string> v;
        for (auto i : order)
            v.push_back(seq[i]);
        func(std::move(v));
    } while (std::next_permutation(order.begin(), order.end()));
}

class MetricTreeTest : public unittest::Test {
protected:
    MetricTreeSet& trees() {
        return *_trees;
    }

    void resetTrees() {
        _trees = std::make_unique<MetricTreeSet>();
    }

    BSONObj serialize() {
        return serialize(trees()[ClusterRole{}]);
    }

    BSONObj serialize(const MetricTree& mt) {
        return serialize(mt, BSONObj{});
    }

    BSONObj serialize(const MetricTree& mt, BSONObj excludePaths) {
        BSONObjBuilder bob;
        mt.appendTo(bob, excludePaths);
        return bob.obj();
    }

    Counter64& addCounter(StringData path, MetricTree& tree) {
        auto m = std::make_unique<
            BasicServerStatusMetric<ServerStatusMetricPolicySelectionT<Counter64>>>();
        auto& ref = m->value();
        tree.add(path, std::move(m));
        return ref;
    }

    Counter64& addCounter(StringData path, ClusterRole role = {}) {
        return addCounter(path, trees()[role]);
    }

    std::vector<std::string> extractTreeNodes(
        BSONObj metrics, const std::function<bool(const BSONElement&)>& pred = {}) {
        std::vector<std::string> nodes;
        // Extract node names from metrics tree
        for (auto&& el : metrics) {
            StringData key = el.fieldNameStringData();
            switch (el.type()) {
                case BSONType::object:
                    for (auto&& v : extractTreeNodes(el.Obj(), pred))
                        nodes.push_back(fmt::format("{}.{}", key, v));
                    break;
                default:
                    if (!pred || pred(el))
                        nodes.push_back(std::string{key});
                    break;
            }
        }
        return nodes;
    }

    std::vector<StringData> dotSplit(StringData path) {
        std::vector<StringData> parts;
        while (true) {
            auto dot = path.find(".");
            if (dot == std::string::npos) {
                parts.push_back(path);
                return parts;
            }
            parts.push_back(path.substr(0, dot));
            path = path.substr(dot + 1);
        }
    }

    BSONObj erasePaths(BSONObj inDoc, const std::vector<std::string>& paths) {
        mutablebson::Document doc(inDoc);
        for (auto&& path : paths) {
            auto elem = doc.root();
            for (auto&& part : dotSplit(path))
                if (elem = elem[part]; !elem.ok())
                    break;
            if (elem.ok())
                invariant(elem.remove());
        }
        return doc.getObject();
    }

    /** Parses `json` and nests the result under the "metrics" root. */
    static BSONObj mJson(StringData json) {
        return BSON("metrics" << fromjson(json));
    }

    /** Adds the implicit "metrics" root to the dotted `path`. */
    static std::string mStr(StringData path) {
        return fmt::format("metrics.{}", path);
    }

    /** New vector from adding implicit "metrics" to each element. */
    std::vector<std::string> mStrVec(std::vector<std::string> pathsVec) {
        std::vector<std::string> out;
        for (auto&& p : pathsVec)
            out.push_back(mStr(p));
        return out;
    }

    void appendJsonToTree(MetricTree& tree, StringData json) {
        for (auto&& path : extractTreeNodes(mJson(json)["metrics"].Obj()))
            addCounter(path, tree);
    }

    BSONObj actualMerged(const std::vector<std::string>& specs, BSONObj excludedPaths = {}) {
        std::vector<std::unique_ptr<MetricTree>> componentTrees;
        for (StringData json : specs) {
            auto tree = std::make_unique<MetricTree>();
            appendJsonToTree(*tree, json);
            componentTrees.push_back(std::move(tree));
        }
        std::vector<const MetricTree*> ptrs;
        for (auto&& t : componentTrees)
            ptrs.push_back(t.get());
        BSONObjBuilder b;
        appendMergedTrees(ptrs, b, excludedPaths);
        return b.obj();
    }

    BSONObj expectedMerged(const std::vector<std::string>& specs, BSONObj excludedPaths = {}) {
        BSONObjBuilder b;
        MetricTree mt;
        for (StringData json : specs)
            appendJsonToTree(mt, json);
        mt.appendTo(b);
        return erasePaths(b.obj(), extractTreeNodes(excludedPaths, falseNodesPredicate));
    }

private:
    std::unique_ptr<MetricTreeSet> _trees = std::make_unique<MetricTreeSet>();
};

TEST_F(MetricTreeTest, DefaultMetricsSubtree) {
    for (StringData path : {"foo", "bar"})
        addCounter(path);
    ASSERT_BSONOBJ_EQ(serialize(), mJson("{bar:0,foo:0}"));
}

TEST_F(MetricTreeTest, LeadingDotMeansRoot) {
    for (StringData path : {".foo", ".bar"})
        addCounter(path);
    ASSERT_BSONOBJ_EQ(serialize(), fromjson("{bar:0,foo:0}"));
}

// For each node, output order should be alphabetical metrics, then
// alphabetical subtrees. The order in which nodes are added to the tree
// should be irrelevant.
TEST_F(MetricTreeTest, StableOrder) {
    // Start with a BSON tree, get the nodes that would have produced it,
    // and then try every ordering of those nodes and confirm that they always
    // produce the same tree.
    BSONObj expected = mJson("{b:0,c:0,a:{a:0},d:{a:0,b:0}}");
    forEachPermutationOf(extractTreeNodes(expected["metrics"].Obj()), [&](auto&& in) {
        resetTrees();
        for (const auto& path : in)
            addCounter(path);
        ASSERT_BSONOBJ_EQ(serialize(trees()[ClusterRole{}]), expected);
    });
}

TEST_F(MetricTreeTest, ValidateCounterMetric) {
    auto& counter = addCounter("tree.counter");
    for (auto&& incr : {1, 2}) {
        counter.increment(incr);
        ASSERT_BSONOBJ_EQ(serialize(),
                          mJson(fmt::format("{{tree:{{counter:{}}}}}", counter.get())));
    }
}

TEST_F(MetricTreeTest, ValidateTextMetric) {
    auto& text = *MetricBuilder<std::string>{"tree.text"}.setTreeSet(&trees());
    for (auto&& str : {"hello", "bye"}) {
        text = std::string{str};
        ASSERT_BSONOBJ_EQ(serialize(), mJson(fmt::format("{{tree:{{text:\"{}\"}}}}", str)));
    }
}

TEST_F(MetricTreeTest, ExcludePaths) {
    // Make a full tree, and try a few different exclusion trees on it. The
    // serialized result with excludePaths should be exactly equivalent to
    // making a result without exclusions and performing explicit subtree
    // removals on the result.
    const BSONObj full = mJson("{b:0,c:0,a:{a:0},d:{a:0,b:0}}");
    for (auto&& path : extractTreeNodes(full["metrics"].Obj()))
        addCounter(path);
    for (auto&& exclusionTreeJson : std::vector<std::string>{
             "{b:true}",
             "{c:false,a:false}",
             "{a:false}",
             "{d:{a:false}}",
         }) {
        auto excludePaths = mJson(exclusionTreeJson);
        ASSERT_BSONOBJ_EQ(serialize(trees()[ClusterRole{}], excludePaths),
                          erasePaths(full, extractTreeNodes(excludePaths, falseNodesPredicate)));
    }
}

TEST_F(MetricTreeTest, MergedTreeView) {
    for (auto&& testCase : std::vector<std::vector<std::string>>{
             // Empties
             {},
             {"{}"},
             {"{}", "{}"},
             {"{}", "{}", "{}"},

             // Simple nodes
             {"{a:0}"},
             {"{a:0}", "{}"},
             {"{a:0}", "{b:0}"},

             // Subtrees
             {"{a:{aa:0}}"},
             {"{a:{aa:0}}", "{a:{ab:0}}"},
             {"{a:{aa:0}}", "{a:{ab:0}}", "{a:{ac:0}}"},

             // Mix metric and subtree
             {"{b:0}", "{a:{aa:0}}"},

             // Big multimerge of a multilevel tree
             {
                 "{b:0,c:0,a:{a:0},d:{a:0,b:0}}",
                 "{e:0,f:{a:0,b:0}}",
                 "{f:{c:0}}",
                 "{g:0,h:{a:0,b:{a:0}}}",
                 "{h:{b:{b:0}}}",
                 "{}",
             },
         }) {
        resetTrees();
        // A tree constructed from all nodes of the component trees
        // should produce the same result as `appendMergedTrees`.
        ASSERT_BSONOBJ_EQ(actualMerged(testCase), expectedMerged(testCase));
    }
}

TEST_F(MetricTreeTest, MergedTreeViewWithExcludePaths) {
    for (auto&& testCase : std::vector<std::vector<std::string>>{
             {},
             {"{}"},
             {"{}", "{}"},
             {"{b:0,c:0,a:{a:0},d:{a:0,b:0}}"},
             {"{b:0,a:{a:0},d:{a:0,b:0}}", "{c:0}"},
             {"{b:0,c:0,d:{a:0,b:0}}", "{a:{a:0}}"},
         }) {
        for (auto&& exclusionTreeJson : {
                 "{b:true}",
                 "{c:false,a:false}",
                 "{a:false}",
                 "{d:{a:false}}",
             }) {
            BSONObj exclusionTree = mJson(exclusionTreeJson);
            ASSERT_BSONOBJ_EQ(actualMerged(testCase, exclusionTree),
                              expectedMerged(testCase, exclusionTree));
        }
    }
}

TEST_F(MetricTreeTest, MergedTreeViewWithCollision) {
    using BadValueEx = ExceptionFor<ErrorCodes::BadValue>;
    ASSERT_THROWS(actualMerged({"{a:0}", "{a:0}"}), BadValueEx);
    ASSERT_THROWS(actualMerged({"{a:{aa:0}}", "{a:{aa:0}}"}), BadValueEx);
}

TEST_F(MetricTreeTest, MetricTreeSet) {
    auto& nTree = trees()[ClusterRole::None];
    auto& sTree = trees()[ClusterRole::ShardServer];
    auto& rTree = trees()[ClusterRole::RouterServer];
    ASSERT_NE(&nTree, &sTree);
    ASSERT_NE(&sTree, &rTree);

    addCounter(".n", ClusterRole::None);
    addCounter(".s", ClusterRole::ShardServer);
    addCounter(".r", ClusterRole::RouterServer);
}

TEST_F(MetricTreeTest, MetricBuilderSetTreeSet) {
    *MetricBuilder<Counter64>{"test.m1"}.setTreeSet(&trees());
    ASSERT_BSONOBJ_EQ(serialize(trees()[ClusterRole::None]), mJson("{test:{m1:0}}"));
}

}  // namespace
}  // namespace mongo
