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

#ifndef V8_REGEXP_AST_H_
#define V8_REGEXP_AST_H_

// Prevent msvc build failures as indicated in bug 1205328
#ifdef min
# undef min
#endif
#ifdef max
# undef max
#endif

#include "irregexp/RegExpEngine.h"

namespace js {
namespace irregexp {

class RegExpCompiler;
class RegExpNode;

class RegExpVisitor
{
  public:
    virtual ~RegExpVisitor() { }
#define MAKE_CASE(Name)                                         \
    virtual void* Visit##Name(RegExp##Name*, void* data) = 0;
    FOR_EACH_REG_EXP_TREE_TYPE(MAKE_CASE)
#undef MAKE_CASE
};

class RegExpTree
{
  public:
    static const int kInfinity = INT32_MAX;
    virtual ~RegExpTree() {}
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success) = 0;
    virtual bool IsTextElement() { return false; }
    virtual bool IsAnchoredAtStart() { return false; }
    virtual bool IsAnchoredAtEnd() { return false; }
    virtual int min_match() = 0;
    virtual int max_match() = 0;
    // Returns the interval of registers used for captures within this
    // expression.
    virtual Interval CaptureRegisters() { return Interval::Empty(); }
    virtual void AppendToText(RegExpText* text) {
        MOZ_CRASH("Bad call");
    }
#define MAKE_ASTYPE(Name)                                               \
    virtual RegExp##Name* As##Name();                                   \
    virtual bool Is##Name();
    FOR_EACH_REG_EXP_TREE_TYPE(MAKE_ASTYPE)
#undef MAKE_ASTYPE
};

typedef Vector<RegExpTree*, 1, LifoAllocPolicy<Infallible> > RegExpTreeVector;

class RegExpDisjunction : public RegExpTree
{
  public:
    explicit RegExpDisjunction(RegExpTreeVector* alternatives);
    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpDisjunction* AsDisjunction();
    virtual Interval CaptureRegisters();
    virtual bool IsDisjunction();
    virtual bool IsAnchoredAtStart();
    virtual bool IsAnchoredAtEnd();
    virtual int min_match() { return min_match_; }
    virtual int max_match() { return max_match_; }

    const RegExpTreeVector& alternatives() { return *alternatives_; }

  private:
    RegExpTreeVector* alternatives_;
    int min_match_;
    int max_match_;
};

class RegExpAlternative : public RegExpTree
{
  public:
    explicit RegExpAlternative(RegExpTreeVector* nodes);
    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpAlternative* AsAlternative();
    virtual Interval CaptureRegisters();
    virtual bool IsAlternative();
    virtual bool IsAnchoredAtStart();
    virtual bool IsAnchoredAtEnd();
    virtual int min_match() { return min_match_; }
    virtual int max_match() { return max_match_; }

    const RegExpTreeVector& nodes() { return *nodes_; }

  private:
    RegExpTreeVector* nodes_;
    int min_match_;
    int max_match_;
};

class RegExpAssertion : public RegExpTree {
 public:
  enum AssertionType {
    START_OF_LINE,
    START_OF_INPUT,
    END_OF_LINE,
    END_OF_INPUT,
    BOUNDARY,
    NON_BOUNDARY
  };
  explicit RegExpAssertion(AssertionType type) : assertion_type_(type) { }
  virtual void* Accept(RegExpVisitor* visitor, void* data);
  virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                             RegExpNode* on_success);
  virtual RegExpAssertion* AsAssertion();
  virtual bool IsAssertion();
  virtual bool IsAnchoredAtStart();
  virtual bool IsAnchoredAtEnd();
  virtual int min_match() { return 0; }
  virtual int max_match() { return 0; }
  AssertionType assertion_type() { return assertion_type_; }
 private:
  AssertionType assertion_type_;
};

class CharacterSet
{
  public:
    explicit CharacterSet(char16_t standard_set_type)
      : ranges_(nullptr),
        standard_set_type_(standard_set_type)
    {}
    explicit CharacterSet(CharacterRangeVector* ranges)
      : ranges_(ranges),
        standard_set_type_(0)
    {}

    CharacterRangeVector& ranges(LifoAlloc* alloc);
    char16_t standard_set_type() { return standard_set_type_; }
    void set_standard_set_type(char16_t special_set_type) {
        standard_set_type_ = special_set_type;
    }
    bool is_standard() { return standard_set_type_ != 0; }
    void Canonicalize();

  private:
    CharacterRangeVector* ranges_;

    // If non-zero, the value represents a standard set (e.g., all whitespace
    // characters) without having to expand the ranges.
    char16_t standard_set_type_;
};

class RegExpCharacterClass : public RegExpTree
{
  public:
    RegExpCharacterClass(CharacterRangeVector* ranges, bool is_negated)
      : set_(ranges),
        is_negated_(is_negated)
    {}

    explicit RegExpCharacterClass(char16_t type)
      : set_(type),
        is_negated_(false)
    {}

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpCharacterClass* AsCharacterClass();
    virtual bool IsCharacterClass();
    virtual bool IsTextElement() { return true; }
    virtual int min_match() { return 1; }
    virtual int max_match() { return 1; }
    virtual void AppendToText(RegExpText* text);

    CharacterSet character_set() { return set_; }

    // TODO(lrn): Remove need for complex version if is_standard that
    // recognizes a mangled standard set and just do { return set_.is_special(); }
    bool is_standard(LifoAlloc* alloc);

    // Returns a value representing the standard character set if is_standard()
    // returns true.
    // Currently used values are:
    // s : unicode whitespace
    // S : unicode non-whitespace
    // w : ASCII word character (digit, letter, underscore)
    // W : non-ASCII word character
    // d : ASCII digit
    // D : non-ASCII digit
    // . : non-unicode non-newline
    // * : All characters
    char16_t standard_type() { return set_.standard_set_type(); }

    CharacterRangeVector& ranges(LifoAlloc* alloc) { return set_.ranges(alloc); }
    bool is_negated() { return is_negated_; }

  private:
    CharacterSet set_;
    bool is_negated_;
};

typedef Vector<char16_t, 10, LifoAllocPolicy<Infallible> > CharacterVector;

class RegExpAtom : public RegExpTree
{
  public:
    explicit RegExpAtom(CharacterVector* data)
      : data_(data)
    {}

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpAtom* AsAtom();
    virtual bool IsAtom();
    virtual bool IsTextElement() { return true; }
    virtual int min_match() { return data_->length(); }
    virtual int max_match() { return data_->length(); }
    virtual void AppendToText(RegExpText* text);

    const CharacterVector& data() { return *data_; }
    int length() { return data_->length(); }

  private:
    CharacterVector* data_;
};

class RegExpText : public RegExpTree
{
  public:
    explicit RegExpText(LifoAlloc* alloc)
      : elements_(*alloc), length_(0)
    {}

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpText* AsText();
    virtual bool IsText();
    virtual bool IsTextElement() { return true; }
    virtual int min_match() { return length_; }
    virtual int max_match() { return length_; }
    virtual void AppendToText(RegExpText* text);

    void AddElement(TextElement elm)  {
        elements_.append(elm);
        length_ += elm.length();
    }
    const TextElementVector& elements() { return elements_; }

  private:
    TextElementVector elements_;
    int length_;
};

class RegExpQuantifier : public RegExpTree
{
  public:
    enum QuantifierType { GREEDY, NON_GREEDY, POSSESSIVE };
    RegExpQuantifier(int min, int max, QuantifierType type, RegExpTree* body)
      : body_(body),
        min_(min),
        max_(max),
        min_match_(min * body->min_match()),
        quantifier_type_(type)
    {
        if (max > 0 && body->max_match() > kInfinity / max) {
            max_match_ = kInfinity;
        } else {
            max_match_ = max * body->max_match();
        }
    }

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    static RegExpNode* ToNode(int min,
                              int max,
                              bool is_greedy,
                              RegExpTree* body,
                              RegExpCompiler* compiler,
                              RegExpNode* on_success,
                              bool not_at_start = false);
    virtual RegExpQuantifier* AsQuantifier();
    virtual Interval CaptureRegisters();
    virtual bool IsQuantifier();
    virtual int min_match() { return min_match_; }
    virtual int max_match() { return max_match_; }
    int min() { return min_; }
    int max() { return max_; }
    bool is_possessive() { return quantifier_type_ == POSSESSIVE; }
    bool is_non_greedy() { return quantifier_type_ == NON_GREEDY; }
    bool is_greedy() { return quantifier_type_ == GREEDY; }
    RegExpTree* body() { return body_; }

  private:
    RegExpTree* body_;
    int min_;
    int max_;
    int min_match_;
    int max_match_;
    QuantifierType quantifier_type_;
};

class RegExpCapture : public RegExpTree
{
  public:
    explicit RegExpCapture(RegExpTree* body, int index)
      : body_(body), index_(index)
    {}

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    static RegExpNode* ToNode(RegExpTree* body,
                              int index,
                              RegExpCompiler* compiler,
                              RegExpNode* on_success);
    virtual RegExpCapture* AsCapture();
    virtual bool IsAnchoredAtStart();
    virtual bool IsAnchoredAtEnd();
    virtual Interval CaptureRegisters();
    virtual bool IsCapture();
    virtual int min_match() { return body_->min_match(); }
    virtual int max_match() { return body_->max_match(); }
    RegExpTree* body() { return body_; }
    int index() { return index_; }
    static int StartRegister(int index) { return index * 2; }
    static int EndRegister(int index) { return index * 2 + 1; }

  private:
    RegExpTree* body_;
    int index_;
};

class RegExpLookahead : public RegExpTree
{
  public:
    RegExpLookahead(RegExpTree* body,
                    bool is_positive,
                    int capture_count,
                    int capture_from)
      : body_(body),
        is_positive_(is_positive),
        capture_count_(capture_count),
        capture_from_(capture_from)
    {}

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpLookahead* AsLookahead();
    virtual Interval CaptureRegisters();
    virtual bool IsLookahead();
    virtual bool IsAnchoredAtStart();
    virtual int min_match() { return 0; }
    virtual int max_match() { return 0; }
    RegExpTree* body() { return body_; }
    bool is_positive() { return is_positive_; }
    int capture_count() { return capture_count_; }
    int capture_from() { return capture_from_; }

  private:
    RegExpTree* body_;
    bool is_positive_;
    int capture_count_;
    int capture_from_;
};

typedef Vector<RegExpCapture*, 1, LifoAllocPolicy<Infallible> > RegExpCaptureVector;

class RegExpBackReference : public RegExpTree
{
  public:
    explicit RegExpBackReference(RegExpCapture* capture)
      : capture_(capture)
    {}

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpBackReference* AsBackReference();
    virtual bool IsBackReference();
    virtual int min_match() { return 0; }
    virtual int max_match() { return capture_->max_match(); }
    int index() { return capture_->index(); }
    RegExpCapture* capture() { return capture_; }
  private:
    RegExpCapture* capture_;
};

class RegExpEmpty : public RegExpTree
{
  public:
    RegExpEmpty()
    {}

    virtual void* Accept(RegExpVisitor* visitor, void* data);
    virtual RegExpNode* ToNode(RegExpCompiler* compiler,
                               RegExpNode* on_success);
    virtual RegExpEmpty* AsEmpty();
    virtual bool IsEmpty();
    virtual int min_match() { return 0; }
    virtual int max_match() { return 0; }
    static RegExpEmpty* GetInstance() {
        static RegExpEmpty instance;
        return &instance;
    }
};

} } // namespace js::irregexp

#endif  // V8_REGEXP_AST_H_
