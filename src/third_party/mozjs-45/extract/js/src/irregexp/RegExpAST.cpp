/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99: */

// Copyright 2012 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "irregexp/RegExpAST.h"

using namespace js;
using namespace js::irregexp;

#define MAKE_ACCEPT(Name)                                            \
  void* RegExp##Name::Accept(RegExpVisitor* visitor, void* data) {   \
    return visitor->Visit##Name(this, data);                         \
  }
FOR_EACH_REG_EXP_TREE_TYPE(MAKE_ACCEPT)
#undef MAKE_ACCEPT

#define MAKE_TYPE_CASE(Name)                                         \
  RegExp##Name* RegExpTree::As##Name() {                             \
    return nullptr;                                                  \
  }                                                                  \
  bool RegExpTree::Is##Name() { return false; }
FOR_EACH_REG_EXP_TREE_TYPE(MAKE_TYPE_CASE)
#undef MAKE_TYPE_CASE

#define MAKE_TYPE_CASE(Name)                                        \
  RegExp##Name* RegExp##Name::As##Name() {                          \
    return this;                                                    \
  }                                                                 \
  bool RegExp##Name::Is##Name() { return true; }
FOR_EACH_REG_EXP_TREE_TYPE(MAKE_TYPE_CASE)
#undef MAKE_TYPE_CASE

static Interval
ListCaptureRegisters(const RegExpTreeVector& children)
{
    Interval result = Interval::Empty();
    for (size_t i = 0; i < children.length(); i++)
        result = result.Union(children[i]->CaptureRegisters());
    return result;
}

// ----------------------------------------------------------------------------
// RegExpDisjunction

RegExpDisjunction::RegExpDisjunction(RegExpTreeVector* alternatives)
  : alternatives_(alternatives)
{
    MOZ_ASSERT(alternatives->length() > 1);
    RegExpTree* first_alternative = (*alternatives)[0];
    min_match_ = first_alternative->min_match();
    max_match_ = first_alternative->max_match();
    for (size_t i = 1; i < alternatives->length(); i++) {
        RegExpTree* alternative = (*alternatives)[i];
        min_match_ = Min(min_match_, alternative->min_match());
        max_match_ = Max(max_match_, alternative->max_match());
    }
}

Interval
RegExpDisjunction::CaptureRegisters()
{
    return ListCaptureRegisters(alternatives());
}

bool
RegExpDisjunction::IsAnchoredAtStart()
{
    const RegExpTreeVector& alternatives = this->alternatives();
    for (size_t i = 0; i < alternatives.length(); i++) {
        if (!alternatives[i]->IsAnchoredAtStart())
            return false;
    }
    return true;
}

bool
RegExpDisjunction::IsAnchoredAtEnd()
{
    const RegExpTreeVector& alternatives = this->alternatives();
    for (size_t i = 0; i < alternatives.length(); i++) {
        if (!alternatives[i]->IsAnchoredAtEnd())
            return false;
    }
    return true;
}

// ----------------------------------------------------------------------------
// RegExpAlternative

static int IncreaseBy(int previous, int increase)
{
    if (RegExpTree::kInfinity - previous < increase)
        return RegExpTree::kInfinity;
    return previous + increase;
}

RegExpAlternative::RegExpAlternative(RegExpTreeVector* nodes)
  : nodes_(nodes),
    min_match_(0),
    max_match_(0)
{
    MOZ_ASSERT(nodes->length() > 1);
    for (size_t i = 0; i < nodes->length(); i++) {
        RegExpTree* node = (*nodes)[i];
        int node_min_match = node->min_match();
        min_match_ = IncreaseBy(min_match_, node_min_match);
        int node_max_match = node->max_match();
        max_match_ = IncreaseBy(max_match_, node_max_match);
    }
}

Interval
RegExpAlternative::CaptureRegisters()
{
    return ListCaptureRegisters(nodes());
}

bool
RegExpAlternative::IsAnchoredAtStart()
{
    const RegExpTreeVector& nodes = this->nodes();
    for (size_t i = 0; i < nodes.length(); i++) {
        RegExpTree* node = nodes[i];
        if (node->IsAnchoredAtStart()) { return true; }
        if (node->max_match() > 0) { return false; }
    }
    return false;
}

bool
RegExpAlternative::IsAnchoredAtEnd()
{
    const RegExpTreeVector& nodes = this->nodes();
    for (int i = nodes.length() - 1; i >= 0; i--) {
        RegExpTree* node = nodes[i];
        if (node->IsAnchoredAtEnd()) { return true; }
        if (node->max_match() > 0) { return false; }
    }
    return false;
}

// ----------------------------------------------------------------------------
// RegExpAssertion

bool
RegExpAssertion::IsAnchoredAtStart()
{
    return assertion_type() == RegExpAssertion::START_OF_INPUT;
}

bool
RegExpAssertion::IsAnchoredAtEnd()
{
    return assertion_type() == RegExpAssertion::END_OF_INPUT;
}

// ----------------------------------------------------------------------------
// RegExpCharacterClass

void
RegExpCharacterClass::AppendToText(RegExpText* text)
{
    text->AddElement(TextElement::CharClass(this));
}

CharacterRangeVector&
CharacterSet::ranges(LifoAlloc* alloc)
{
    if (ranges_ == nullptr) {
        ranges_ = alloc->newInfallible<CharacterRangeVector>(*alloc);
        CharacterRange::AddClassEscape(alloc, standard_set_type_, ranges_);
    }
    return *ranges_;
}

// ----------------------------------------------------------------------------
// RegExpAtom

void
RegExpAtom::AppendToText(RegExpText* text)
{
    text->AddElement(TextElement::Atom(this));
}

// ----------------------------------------------------------------------------
// RegExpText

void
RegExpText::AppendToText(RegExpText* text)
{
    for (size_t i = 0; i < elements().length(); i++)
        text->AddElement(elements()[i]);
}

// ----------------------------------------------------------------------------
// RegExpQuantifier

Interval
RegExpQuantifier::CaptureRegisters()
{
    return body()->CaptureRegisters();
}

// ----------------------------------------------------------------------------
// RegExpCapture

bool
RegExpCapture::IsAnchoredAtStart()
{
    return body()->IsAnchoredAtStart();
}

bool
RegExpCapture::IsAnchoredAtEnd()
{
    return body()->IsAnchoredAtEnd();
}

Interval
RegExpCapture::CaptureRegisters()
{
    Interval self(StartRegister(index()), EndRegister(index()));
    return self.Union(body()->CaptureRegisters());
}

// ----------------------------------------------------------------------------
// RegExpLookahead

Interval
RegExpLookahead::CaptureRegisters()
{
    return body()->CaptureRegisters();
}

bool
RegExpLookahead::IsAnchoredAtStart()
{
    return is_positive() && body()->IsAnchoredAtStart();
}
