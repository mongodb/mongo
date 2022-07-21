/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/base/dependency_graph.h"

#include <algorithm>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <iostream>
#include <iterator>
#include <random>
#include <sstream>

#include "mongo/util/assert_util.h"
#include "mongo/util/string_map.h"

namespace mongo {

void DependencyGraph::addNode(std::string name,
                              std::vector<std::string> prerequisites,
                              std::vector<std::string> dependents,
                              std::unique_ptr<Payload> payload) {
    if (!payload) {
        struct DummyPayload : Payload {};
        payload = std::make_unique<DummyPayload>();
    }
    auto& newNode = _nodes[name];
    uassert(ErrorCodes::Error(50999), name, !newNode.payload);  // collision
    for (auto& otherNode : prerequisites)
        newNode.prerequisites.insert(otherNode);
    for (auto& otherNode : dependents)
        _nodes[otherNode].prerequisites.insert(name);
    newNode.payload = std::move(payload);
}

namespace {

using namespace fmt::literals;

template <typename Seq>
void strAppendJoin(std::string& out, StringData separator, const Seq& sequence) {
    StringData currSep;
    for (StringData str : sequence) {
        out.append(currSep.rawData(), currSep.size());
        out.append(str.rawData(), str.size());
        currSep = separator;
    }
}

// In the case of a cycle, copy the cycle node names into `*cycle`.
template <typename Iter>
void throwGraphContainsCycle(Iter first, Iter last, std::vector<std::string>* cycle) {
    std::vector<std::string> names;
    std::transform(first, last, std::back_inserter(names), [](auto& e) { return e->name(); });
    if (cycle)
        *cycle = names;
    names.push_back((*first)->name());
    uasserted(ErrorCodes::GraphContainsCycle,
              format(FMT_STRING("Cycle in dependency graph: {}"), fmt::join(names, " -> ")));
}

}  // namespace

std::vector<std::string> DependencyGraph::topSort(std::vector<std::string>* cycle) const {
    // Topological sort via repeated depth-first traversal.
    // All nodes must have an initFn before running topSort, or we return BadValue.
    struct Element {
        const std::string& name() const {
            return nodeIter->first;
        }
        stdx::unordered_map<std::string, Node>::const_iterator nodeIter;
        std::vector<Element*> children;
        std::vector<Element*>::iterator membership;  // Position of this in `elements`.
    };

    std::vector<Element> elementsStore;
    std::vector<Element*> elements;

    // Swap the pointers in the `elements` vector that point to `a` and `b`.
    // Update their 'membership' data members to reflect the change.
    auto swapPositions = [](Element& a, Element& b) {
        if (&a == &b) {
            return;
        }
        using std::swap;
        swap(*a.membership, *b.membership);
        swap(a.membership, b.membership);
    };

    elementsStore.reserve(_nodes.size());
    for (auto iter = _nodes.begin(); iter != _nodes.end(); ++iter) {
        uassert(ErrorCodes::BadValue,
                "node {} was mentioned but never added"_format(iter->first),
                iter->second.payload);
        elementsStore.push_back(Element{iter});
    }

    // Wire up all the child relationships by pointer rather than by string names.
    {
        StringMap<Element*> byName;
        for (Element& e : elementsStore)
            byName[e.name()] = &e;
        for (Element& element : elementsStore) {
            const auto& prereqs = element.nodeIter->second.prerequisites;
            std::transform(prereqs.begin(),
                           prereqs.end(),
                           std::back_inserter(element.children),
                           [&](StringData childName) {
                               auto iter = byName.find(childName);
                               uassert(ErrorCodes::BadValue,
                                       "node {} depends on missing node {}"_format(
                                           element.nodeIter->first, childName),
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
                throwGraphContainsCycle(unsortedEnd, elements.end(), cycle);
            }
            swapPositions(**--unsortedEnd, *picked);  // unsorted push to stack
            continue;
        }
        swapPositions(**unsortedEnd++, **unsortedBegin++);  // pop from stack to sorted
    }
    std::vector<std::string> sortedNames;
    sortedNames.reserve(_nodes.size());
    std::transform(elements.begin(),
                   elements.end(),
                   std::back_inserter(sortedNames),
                   [](const Element* e) { return e->name(); });
    return sortedNames;
}

DependencyGraph::Payload* DependencyGraph::find(const std::string& name) {
    auto iter = _nodes.find(name);
    return (iter == _nodes.end()) ? nullptr : iter->second.payload.get();
}

}  // namespace mongo
