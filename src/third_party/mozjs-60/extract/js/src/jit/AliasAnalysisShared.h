/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jit_AliasAnalysisShared_h
#define jit_AliasAnalysisShared_h

#include "jit/MIR.h"
#include "jit/MIRGraph.h"

namespace js {
namespace jit {

class MIRGraph;

class AliasAnalysisShared
{
  protected:
    MIRGenerator* mir;
    MIRGraph& graph_;

  public:
    AliasAnalysisShared(MIRGenerator* mir, MIRGraph& graph)
      : mir(mir),
        graph_(graph)
    {}

    virtual MOZ_MUST_USE bool analyze() {
        return true;
    }

    static MDefinition::AliasType genericMightAlias(const MDefinition* load,
                                                    const MDefinition* store);


  protected:
    void spewDependencyList();

    TempAllocator& alloc() const {
        return graph_.alloc();
    }
};

// Iterates over the flags in an AliasSet.
class AliasSetIterator
{
  private:
    uint32_t flags;
    unsigned pos;

  public:
    explicit AliasSetIterator(AliasSet set)
      : flags(set.flags()), pos(0)
    {
        while (flags && (flags & 1) == 0) {
            flags >>= 1;
            pos++;
        }
    }
    AliasSetIterator& operator ++(int) {
        do {
            flags >>= 1;
            pos++;
        } while (flags && (flags & 1) == 0);
        return *this;
    }
    explicit operator bool() const {
        return !!flags;
    }
    unsigned operator*() const {
        MOZ_ASSERT(pos < AliasSet::NumCategories);
        return pos;
    }
};

} // namespace jit
} // namespace js

#endif /* jit_AliasAnalysisShared_h */
