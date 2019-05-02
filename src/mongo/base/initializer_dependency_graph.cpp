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

#include "mongo/base/initializer_dependency_graph.h"

#include <algorithm>
#include <fmt/format.h>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>

#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

namespace mongo {

using namespace fmt::literals;

InitializerDependencyGraph::InitializerDependencyGraph() {}
InitializerDependencyGraph::~InitializerDependencyGraph() {}

Status InitializerDependencyGraph::addInitializer(std::string name,
                                                  InitializerFunction initFn,
                                                  DeinitializerFunction deinitFn,
                                                  std::vector<std::string> prerequisites,
                                                  std::vector<std::string> dependents) {
    if (!initFn)
        return Status(ErrorCodes::BadValue, "Illegal to supply a NULL function");

    InitializerDependencyNode& newNode = _nodes[name];
    if (newNode.initFn) {
        return Status(ErrorCodes::Error(50999), name);
    }

    newNode.initFn = std::move(initFn);
    newNode.deinitFn = std::move(deinitFn);

    for (size_t i = 0; i < prerequisites.size(); ++i) {
        newNode.prerequisites.insert(prerequisites[i]);
    }

    for (size_t i = 0; i < dependents.size(); ++i) {
        _nodes[dependents[i]].prerequisites.insert(name);
    }

    return Status::OK();
}

InitializerDependencyNode* InitializerDependencyGraph::getInitializerNode(const std::string& name) {
    NodeMap::iterator iter = _nodes.find(name);
    if (iter == _nodes.end())
        return nullptr;

    return &iter->second;
}

namespace {

template <typename Seq>
void strAppendJoin(std::string& out, StringData separator, const Seq& sequence) {
    StringData currSep;
    for (StringData str : sequence) {
        out.append(currSep.rawData(), currSep.size());
        out.append(str.rawData(), str.size());
        currSep = separator;
    }
}

// In the case of a cycle, copy the cycle into `names`.
// It's undocumented behavior, but it's cheap and a test wants it.
template <typename Iter>
void throwGraphContainsCycle(Iter first, Iter last, std::vector<std::string>& names) {
    std::string desc = "Cycle in dependency graph: ";
    std::transform(first, last, std::back_inserter(names), [](auto& e) { return e->name(); });
    names.push_back((*first)->name());  // Tests awkwardly want first element to be repeated.
    strAppendJoin(desc, " -> ", names);
    uasserted(ErrorCodes::GraphContainsCycle, desc);
}
}  // namespace

Status InitializerDependencyGraph::topSort(std::vector<std::string>* sortedNames) const try {
    // Topological sort via repeated depth-first traversal.
    // All nodes must have an initFn before running topSort, or we return BadValue.
    struct Element {
        const std::string& name() const {
            return node->first;
        }
        const Node* node;
        std::vector<Element*> children;
        std::vector<Element*>::iterator membership;  // Position of this in `elements`.
    };

    std::vector<Element> elementsStore;
    std::vector<Element*> elements;

    // Swap the pointers in the `elements` vector that point to `a` and `b`.
    // Update their 'membership' data members to reflect the change.
    auto swapPositions = [](Element& a, Element& b) {
        using std::swap;
        swap(*a.membership, *b.membership);
        swap(a.membership, b.membership);
    };

    elementsStore.reserve(_nodes.size());
    std::transform(
        _nodes.begin(), _nodes.end(), std::back_inserter(elementsStore), [](const Node& n) {
            uassert(ErrorCodes::BadValue,
                    "No implementation provided for initializer {}"_format(n.first),
                    n.second.initFn);
            return Element{&n};
        });

    // Wire up all the child relationships by pointer rather than by string names.
    {
        StringMap<Element*> byName;
        for (Element& e : elementsStore)
            byName[e.name()] = &e;
        for (Element& element : elementsStore) {
            const auto& prereqs = element.node->second.prerequisites;
            std::transform(prereqs.begin(),
                           prereqs.end(),
                           std::back_inserter(element.children),
                           [&](StringData childName) {
                               auto iter = byName.find(childName);
                               uassert(ErrorCodes::BadValue,
                                       "Initializer {} depends on missing initializer {}"_format(
                                           element.node->first, childName),
                                       iter != byName.end());
                               return iter->second;
                           });
        }
    }

    elements.reserve(_nodes.size());
    std::transform(elementsStore.begin(),
                   elementsStore.end(),
                   std::back_inserter(elements),
                   [](auto& e) { return &e; });

    // Shuffle the inputs to improve test coverage of undeclared dependencies.
    {
        std::random_device slowSeedGen;
        std::mt19937 generator(slowSeedGen());
        std::shuffle(elements.begin(), elements.end(), generator);
        for (Element* e : elements)
            std::shuffle(e->children.begin(), e->children.end(), generator);
    }

    // Initialize all the `membership` iterators. Must only happen after shuffle.
    for (auto iter = elements.begin(); iter != elements.end(); ++iter)
        (*iter)->membership = iter;

    // The `elements` sequence is divided into 3 regions:
    // elements:        [ sorted | unsorted | stack ]
    //          unsortedBegin => [          )  <= unsortedEnd
    // Each element of the stack region is a prerequisite of its neighbor to the right. Through
    // 'swapPositions' calls and boundary increments, elements will transition from unsorted to
    // stack to sorted. The unsorted region shinks to ultimately become an empty region on the
    // right. No other moves are permitted.
    auto unsortedBegin = elements.begin();
    auto unsortedEnd = elements.end();

    while (unsortedBegin != elements.end()) {
        if (unsortedEnd == elements.end()) {
            // The stack is empty but there's more work to do. Grow the stack region to enclose
            // the rightmost unsorted element. Equivalent to pushing it.
            --unsortedEnd;
        }
        auto top = unsortedEnd;
        auto& children = (*top)->children;
        if (!children.empty()) {
            Element* picked = children.back();
            children.pop_back();
            if (picked->membership < unsortedBegin)
                continue;
            if (picked->membership >= unsortedEnd) {  // O(1) cycle detection
                sortedNames->clear();
                throwGraphContainsCycle(unsortedEnd, elements.end(), *sortedNames);
            }
            swapPositions(**--unsortedEnd, *picked);  // unsorted push to stack
            continue;
        }
        swapPositions(**unsortedEnd++, **unsortedBegin++);  // pop from stack to sorted
    }
    sortedNames->clear();
    sortedNames->reserve(_nodes.size());
    std::transform(elements.begin(),
                   elements.end(),
                   std::back_inserter(*sortedNames),
                   [](const Element* e) { return e->name(); });
    return Status::OK();
} catch (const DBException& ex) {
    return ex.toStatus();
}

}  // namespace mongo
