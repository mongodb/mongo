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

#ifndef V8_JSREGEXP_H_
#define V8_JSREGEXP_H_

#include "jscntxt.h"

#include "ds/SplayTree.h"
#include "jit/Label.h"
#include "vm/RegExpObject.h"

namespace js {

class MatchPairs;
class RegExpShared;

namespace jit {
    class Label;
    class JitCode;
}

namespace irregexp {

class RegExpTree;
class RegExpMacroAssembler;

struct RegExpCompileData
{
    RegExpCompileData()
      : tree(nullptr),
        simple(true),
        contains_anchor(false),
        capture_count(0)
    {}

    RegExpTree* tree;
    bool simple;
    bool contains_anchor;
    int capture_count;
};

struct RegExpCode
{
    jit::JitCode* jitCode;
    uint8_t* byteCode;

    RegExpCode()
      : jitCode(nullptr), byteCode(nullptr)
    {}

    bool empty() {
        return !jitCode && !byteCode;
    }

    void destroy() {
        js_free(byteCode);
    }
};

RegExpCode
CompilePattern(JSContext* cx, RegExpShared* shared, RegExpCompileData* data,
               HandleLinearString sample,  bool is_global, bool ignore_case,
               bool is_ascii, bool match_only, bool force_bytecode, bool sticky);

// Note: this may return RegExpRunStatus_Error if an interrupt was requested
// while the code was executing.
template <typename CharT>
RegExpRunStatus
ExecuteCode(JSContext* cx, jit::JitCode* codeBlock, const CharT* chars, size_t start,
            size_t length, MatchPairs* matches);

template <typename CharT>
RegExpRunStatus
InterpretCode(JSContext* cx, const uint8_t* byteCode, const CharT* chars, size_t start,
              size_t length, MatchPairs* matches);

#define FOR_EACH_NODE_TYPE(VISIT)                                    \
  VISIT(End)                                                         \
  VISIT(Action)                                                      \
  VISIT(Choice)                                                      \
  VISIT(BackReference)                                               \
  VISIT(Assertion)                                                   \
  VISIT(Text)

#define FOR_EACH_REG_EXP_TREE_TYPE(VISIT)                            \
  VISIT(Disjunction)                                                 \
  VISIT(Alternative)                                                 \
  VISIT(Assertion)                                                   \
  VISIT(CharacterClass)                                              \
  VISIT(Atom)                                                        \
  VISIT(Quantifier)                                                  \
  VISIT(Capture)                                                     \
  VISIT(Lookahead)                                                   \
  VISIT(BackReference)                                               \
  VISIT(Empty)                                                       \
  VISIT(Text)

#define FORWARD_DECLARE(Name) class RegExp##Name;
FOR_EACH_REG_EXP_TREE_TYPE(FORWARD_DECLARE)
#undef FORWARD_DECLARE

class CharacterRange;
typedef Vector<CharacterRange, 1, LifoAllocPolicy<Infallible> > CharacterRangeVector;

// Represents code units in the range from from_ to to_, both ends are
// inclusive.
class CharacterRange
{
  public:
    CharacterRange()
      : from_(0), to_(0)
    {}

    CharacterRange(char16_t from, char16_t to)
      : from_(from), to_(to)
    {}

    static void AddClassEscape(LifoAlloc* alloc, char16_t type, CharacterRangeVector* ranges);

    static inline CharacterRange Singleton(char16_t value) {
        return CharacterRange(value, value);
    }
    static inline CharacterRange Range(char16_t from, char16_t to) {
        MOZ_ASSERT(from <= to);
        return CharacterRange(from, to);
    }
    static inline CharacterRange Everything() {
        return CharacterRange(0, 0xFFFF);
    }
    bool Contains(char16_t i) { return from_ <= i && i <= to_; }
    char16_t from() const { return from_; }
    void set_from(char16_t value) { from_ = value; }
    char16_t to() const { return to_; }
    void set_to(char16_t value) { to_ = value; }
    bool is_valid() { return from_ <= to_; }
    bool IsEverything(char16_t max) { return from_ == 0 && to_ >= max; }
    bool IsSingleton() { return (from_ == to_); }
    void AddCaseEquivalents(bool is_ascii, CharacterRangeVector* ranges);

    static void Split(const LifoAlloc* alloc,
                      CharacterRangeVector base,
                      const Vector<int>& overlay,
                      CharacterRangeVector* included,
                      CharacterRangeVector* excluded);

    // Whether a range list is in canonical form: Ranges ordered by from value,
    // and ranges non-overlapping and non-adjacent.
    static bool IsCanonical(const CharacterRangeVector& ranges);

    // Convert range list to canonical form. The characters covered by the ranges
    // will still be the same, but no character is in more than one range, and
    // adjacent ranges are merged. The resulting list may be shorter than the
    // original, but cannot be longer.
    static void Canonicalize(CharacterRangeVector& ranges);

    // Negate the contents of a character range in canonical form.
    static void Negate(const LifoAlloc* alloc,
                       CharacterRangeVector src,
                       CharacterRangeVector* dst);

    static const int kStartMarker = (1 << 24);
    static const int kPayloadMask = (1 << 24) - 1;

  private:
    char16_t from_;
    char16_t to_;
};

// A set of unsigned integers that behaves especially well on small
// integers (< 32).
class OutSet
{
  public:
    OutSet()
      : first_(0), remaining_(nullptr), successors_(nullptr)
    {}

    OutSet* Extend(LifoAlloc* alloc, unsigned value);
    bool Get(unsigned value);
    static const unsigned kFirstLimit = 32;

  private:
    typedef Vector<OutSet*, 1, LifoAllocPolicy<Infallible> > OutSetVector;
    typedef Vector<unsigned, 1, LifoAllocPolicy<Infallible> > RemainingVector;

    // Destructively set a value in this set.  In most cases you want
    // to use Extend instead to ensure that only one instance exists
    // that contains the same values.
    void Set(LifoAlloc* alloc, unsigned value);

    // The successors are a list of sets that contain the same values
    // as this set and the one more value that is not present in this
    // set.
    OutSetVector* successors() { return successors_; }

    OutSet(uint32_t first, RemainingVector* remaining)
      : first_(first), remaining_(remaining), successors_(nullptr)
    {}

    RemainingVector& remaining() { return *remaining_; }

    uint32_t first_;
    RemainingVector* remaining_;
    OutSetVector* successors_;
    friend class Trace;
};

// A mapping from integers, specified as ranges, to a set of integers.
// Used for mapping character ranges to choices.
class DispatchTable
{
  public:
    explicit DispatchTable(LifoAlloc* alloc)
    {}

    class Entry {
      public:
        Entry()
          : from_(0), to_(0), out_set_(nullptr)
        {}

        Entry(char16_t from, char16_t to, OutSet* out_set)
          : from_(from), to_(to), out_set_(out_set)
        {}

        char16_t from() { return from_; }
        char16_t to() { return to_; }
        void set_to(char16_t value) { to_ = value; }
        void AddValue(LifoAlloc* alloc, int value) {
            out_set_ = out_set_->Extend(alloc, value);
        }
        OutSet* out_set() { return out_set_; }
      private:
        char16_t from_;
        char16_t to_;
        OutSet* out_set_;
    };

    void AddRange(LifoAlloc* alloc, CharacterRange range, int value);
    OutSet* Get(char16_t value);
    void Dump();

  private:
    // There can't be a static empty set since it allocates its
    // successors in a LifoAlloc and caches them.
    OutSet* empty() { return &empty_; }
    OutSet empty_;
};

class TextElement
{
  public:
    enum TextType {
        ATOM,
        CHAR_CLASS
    };

    static TextElement Atom(RegExpAtom* atom);
    static TextElement CharClass(RegExpCharacterClass* char_class);

    int cp_offset() const { return cp_offset_; }
    void set_cp_offset(int cp_offset) { cp_offset_ = cp_offset; }
    int length() const;

    TextType text_type() const { return text_type_; }

    RegExpTree* tree() const { return tree_; }

    RegExpAtom* atom() const {
        MOZ_ASSERT(text_type() == ATOM);
        return reinterpret_cast<RegExpAtom*>(tree());
    }

    RegExpCharacterClass* char_class() const {
        MOZ_ASSERT(text_type() == CHAR_CLASS);
        return reinterpret_cast<RegExpCharacterClass*>(tree());
    }

  private:
    TextElement(TextType text_type, RegExpTree* tree)
      : cp_offset_(-1), text_type_(text_type), tree_(tree)
    {}

    int cp_offset_;
    TextType text_type_;
    RegExpTree* tree_;
};

typedef Vector<TextElement, 1, LifoAllocPolicy<Infallible> > TextElementVector;

class NodeVisitor;
class RegExpCompiler;
class Trace;
class BoyerMooreLookahead;

struct NodeInfo
{
    NodeInfo()
      : being_analyzed(false),
        been_analyzed(false),
        follows_word_interest(false),
        follows_newline_interest(false),
        follows_start_interest(false),
        at_end(false),
        visited(false),
        replacement_calculated(false)
    {}

    // Returns true if the interests and assumptions of this node
    // matches the given one.
    bool Matches(NodeInfo* that) {
        return (at_end == that->at_end) &&
            (follows_word_interest == that->follows_word_interest) &&
            (follows_newline_interest == that->follows_newline_interest) &&
            (follows_start_interest == that->follows_start_interest);
    }

    // Updates the interests of this node given the interests of the
    // node preceding it.
    void AddFromPreceding(NodeInfo* that) {
        at_end |= that->at_end;
        follows_word_interest |= that->follows_word_interest;
        follows_newline_interest |= that->follows_newline_interest;
        follows_start_interest |= that->follows_start_interest;
    }

    bool HasLookbehind() {
        return follows_word_interest ||
            follows_newline_interest ||
            follows_start_interest;
    }

    // Sets the interests of this node to include the interests of the
    // following node.
    void AddFromFollowing(NodeInfo* that) {
        follows_word_interest |= that->follows_word_interest;
        follows_newline_interest |= that->follows_newline_interest;
        follows_start_interest |= that->follows_start_interest;
    }

    void ResetCompilationState() {
        being_analyzed = false;
        been_analyzed = false;
    }

    bool being_analyzed: 1;
    bool been_analyzed: 1;

    // These bits are set of this node has to know what the preceding
    // character was.
    bool follows_word_interest: 1;
    bool follows_newline_interest: 1;
    bool follows_start_interest: 1;

    bool at_end: 1;
    bool visited: 1;
    bool replacement_calculated: 1;
};

// Details of a quick mask-compare check that can look ahead in the
// input stream.
class QuickCheckDetails
{
  public:
    QuickCheckDetails()
      : characters_(0),
        mask_(0),
        value_(0),
        cannot_match_(false)
    {}

    explicit QuickCheckDetails(int characters)
      : characters_(characters),
        mask_(0),
        value_(0),
        cannot_match_(false)
    {}

    bool Rationalize(bool ascii);

    // Merge in the information from another branch of an alternation.
    void Merge(QuickCheckDetails* other, int from_index);

    // Advance the current position by some amount.
    void Advance(int by, bool ascii);

    void Clear();

    bool cannot_match() { return cannot_match_; }
    void set_cannot_match() { cannot_match_ = true; }

    int characters() { return characters_; }
    void set_characters(int characters) { characters_ = characters; }

    struct Position {
        Position() : mask(0), value(0), determines_perfectly(false) { }
        char16_t mask;
        char16_t value;
        bool determines_perfectly;
    };

    Position* positions(int index) {
        MOZ_ASSERT(index >= 0);
        MOZ_ASSERT(index < characters_);
        return positions_ + index;
    }

    uint32_t mask() { return mask_; }
    uint32_t value() { return value_; }

  private:
    // How many characters do we have quick check information from.  This is
    // the same for all branches of a choice node.
    int characters_;
    Position positions_[4];

    // These values are the condensate of the above array after Rationalize().
    uint32_t mask_;
    uint32_t value_;

    // If set to true, there is no way this quick check can match at all.
    // E.g., if it requires to be at the start of the input, and isn't.
    bool cannot_match_;
};

class RegExpNode
{
  public:
    explicit RegExpNode(LifoAlloc* alloc);
    virtual ~RegExpNode() {}
    virtual void Accept(NodeVisitor* visitor) = 0;

    // Generates a goto to this node or actually generates the code at this point.
    virtual void Emit(RegExpCompiler* compiler, Trace* trace) = 0;

    // How many characters must this node consume at a minimum in order to
    // succeed.  If we have found at least 'still_to_find' characters that
    // must be consumed there is no need to ask any following nodes whether
    // they are sure to eat any more characters.  The not_at_start argument is
    // used to indicate that we know we are not at the start of the input.  In
    // this case anchored branches will always fail and can be ignored when
    // determining how many characters are consumed on success.
    virtual int EatsAtLeast(int still_to_find, int budget, bool not_at_start) = 0;

    // Emits some quick code that checks whether the preloaded characters match.
    // Falls through on certain failure, jumps to the label on possible success.
    // If the node cannot make a quick check it does nothing and returns false.
    bool EmitQuickCheck(RegExpCompiler* compiler,
                        Trace* trace,
                        bool preload_has_checked_bounds,
                        jit::Label* on_possible_success,
                        QuickCheckDetails* details_return,
                        bool fall_through_on_failure);

    // For a given number of characters this returns a mask and a value.  The
    // next n characters are anded with the mask and compared with the value.
    // A comparison failure indicates the node cannot match the next n characters.
    // A comparison success indicates the node may match.
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int characters_filled_in,
                                      bool not_at_start) = 0;

    static const int kNodeIsTooComplexForGreedyLoops = -1;

    virtual int GreedyLoopTextLength() { return kNodeIsTooComplexForGreedyLoops; }

    // Only returns the successor for a text node of length 1 that matches any
    // character and that has no guards on it.
    virtual RegExpNode* GetSuccessorOfOmnivorousTextNode(RegExpCompiler* compiler) {
        return nullptr;
    }

    static const int kRecursionBudget = 200;

    // Collects information on the possible code units (mod 128) that can match if
    // we look forward.  This is used for a Boyer-Moore-like string searching
    // implementation.  TODO(erikcorry):  This should share more code with
    // EatsAtLeast, GetQuickCheckDetails.  The budget argument is used to limit
    // the number of nodes we are willing to look at in order to create this data.
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start) {
        MOZ_CRASH("Bad call");
    }

    // If we know that the input is ASCII then there are some nodes that can
    // never match.  This method returns a node that can be substituted for
    // itself, or nullptr if the node can never match.
    virtual RegExpNode* FilterASCII(int depth, bool ignore_case) { return this; }

    // Helper for FilterASCII.
    RegExpNode* replacement() {
        MOZ_ASSERT(info()->replacement_calculated);
        return replacement_;
    }
    RegExpNode* set_replacement(RegExpNode* replacement) {
        info()->replacement_calculated = true;
        replacement_ =  replacement;
        return replacement;  // For convenience.
    }

    // We want to avoid recalculating the lookahead info, so we store it on the
    // node.  Only info that is for this node is stored.  We can tell that the
    // info is for this node when offset == 0, so the information is calculated
    // relative to this node.
    void SaveBMInfo(BoyerMooreLookahead* bm, bool not_at_start, int offset) {
        if (offset == 0) set_bm_info(not_at_start, bm);
    }

    jit::Label* label() { return &label_; }

    // If non-generic code is generated for a node (i.e. the node is not at the
    // start of the trace) then it cannot be reused.  This variable sets a limit
    // on how often we allow that to happen before we insist on starting a new
    // trace and generating generic code for a node that can be reused by flushing
    // the deferred actions in the current trace and generating a goto.
    static const int kMaxCopiesCodeGenerated = 10;

    NodeInfo* info() { return &info_; }

    BoyerMooreLookahead* bm_info(bool not_at_start) {
        return bm_info_[not_at_start ? 1 : 0];
    }

    LifoAlloc* alloc() const { return alloc_; }

  protected:
    enum LimitResult { DONE, CONTINUE };
    RegExpNode* replacement_;

    LimitResult LimitVersions(RegExpCompiler* compiler, Trace* trace);

    void set_bm_info(bool not_at_start, BoyerMooreLookahead* bm) {
        bm_info_[not_at_start ? 1 : 0] = bm;
    }

  private:
    static const int kFirstCharBudget = 10;
    jit::Label label_;
    NodeInfo info_;

    // This variable keeps track of how many times code has been generated for
    // this node (in different traces).  We don't keep track of where the
    // generated code is located unless the code is generated at the start of
    // a trace, in which case it is generic and can be reused by flushing the
    // deferred operations in the current trace and generating a goto.
    int trace_count_;
    BoyerMooreLookahead* bm_info_[2];

    LifoAlloc* alloc_;
};

// A simple closed interval.
class Interval
{
  public:
    Interval() : from_(kNone), to_(kNone) { }

    Interval(int from, int to) : from_(from), to_(to) { }

    Interval Union(Interval that) {
        if (that.from_ == kNone)
            return *this;
        else if (from_ == kNone)
            return that;
        else
            return Interval(Min(from_, that.from_), Max(to_, that.to_));
    }

    bool Contains(int value) {
        return (from_ <= value) && (value <= to_);
    }

    bool is_empty() { return from_ == kNone; }

    int from() const { return from_; }
    int to() const { return to_; }

    static Interval Empty() { return Interval(); }
    static const int kNone = -1;

  private:
    int from_;
    int to_;
};

class SeqRegExpNode : public RegExpNode
{
  public:
    explicit SeqRegExpNode(RegExpNode* on_success)
      : RegExpNode(on_success->alloc()), on_success_(on_success)
    {}

    RegExpNode* on_success() { return on_success_; }
    void set_on_success(RegExpNode* node) { on_success_ = node; }
    virtual RegExpNode* FilterASCII(int depth, bool ignore_case);
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);

  protected:
    RegExpNode* FilterSuccessor(int depth, bool ignore_case);

  private:
    RegExpNode* on_success_;
};

class ActionNode : public SeqRegExpNode
{
  public:
    enum ActionType {
        SET_REGISTER,
        INCREMENT_REGISTER,
        STORE_POSITION,
        BEGIN_SUBMATCH,
        POSITIVE_SUBMATCH_SUCCESS,
        EMPTY_MATCH_CHECK,
        CLEAR_CAPTURES
    };

    ActionNode(ActionType action_type, RegExpNode* on_success)
      : SeqRegExpNode(on_success),
        action_type_(action_type)
    {}

    static ActionNode* SetRegister(int reg, int val, RegExpNode* on_success);
    static ActionNode* IncrementRegister(int reg, RegExpNode* on_success);
    static ActionNode* StorePosition(int reg,
                                     bool is_capture,
                                     RegExpNode* on_success);
    static ActionNode* ClearCaptures(Interval range, RegExpNode* on_success);
    static ActionNode* BeginSubmatch(int stack_pointer_reg,
                                     int position_reg,
                                     RegExpNode* on_success);
    static ActionNode* PositiveSubmatchSuccess(int stack_pointer_reg,
                                               int restore_reg,
                                               int clear_capture_count,
                                               int clear_capture_from,
                                               RegExpNode* on_success);
    static ActionNode* EmptyMatchCheck(int start_register,
                                       int repetition_register,
                                       int repetition_limit,
                                       RegExpNode* on_success);
    virtual void Accept(NodeVisitor* visitor);
    virtual void Emit(RegExpCompiler* compiler, Trace* trace);
    virtual int EatsAtLeast(int still_to_find, int budget, bool not_at_start);
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int filled_in,
                                      bool not_at_start) {
        return on_success()->GetQuickCheckDetails(
                                                  details, compiler, filled_in, not_at_start);
    }
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);
    ActionType action_type() { return action_type_; }
    // TODO(erikcorry): We should allow some action nodes in greedy loops.
    virtual int GreedyLoopTextLength() { return kNodeIsTooComplexForGreedyLoops; }

  private:
    union {
        struct {
            int reg;
            int value;
        } u_store_register;
        struct {
            int reg;
        } u_increment_register;
        struct {
            int reg;
            bool is_capture;
        } u_position_register;
        struct {
            int stack_pointer_register;
            int current_position_register;
            int clear_register_count;
            int clear_register_from;
        } u_submatch;
        struct {
            int start_register;
            int repetition_register;
            int repetition_limit;
        } u_empty_match_check;
        struct {
            int range_from;
            int range_to;
        } u_clear_captures;
    } data_;
    ActionType action_type_;
    friend class DotPrinter;
};

class TextNode : public SeqRegExpNode
{
  public:
    TextNode(TextElementVector* elements,
             RegExpNode* on_success)
      : SeqRegExpNode(on_success),
        elements_(elements)
    {}

    TextNode(RegExpCharacterClass* that,
             RegExpNode* on_success)
      : SeqRegExpNode(on_success),
        elements_(alloc()->newInfallible<TextElementVector>(*alloc()))
    {
        elements_->append(TextElement::CharClass(that));
    }

    virtual void Accept(NodeVisitor* visitor);
    virtual void Emit(RegExpCompiler* compiler, Trace* trace);
    virtual int EatsAtLeast(int still_to_find, int budget, bool not_at_start);
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int characters_filled_in,
                                      bool not_at_start);
    TextElementVector& elements() { return *elements_; }
    void MakeCaseIndependent(bool is_ascii);
    virtual int GreedyLoopTextLength();
    virtual RegExpNode* GetSuccessorOfOmnivorousTextNode(
                                                         RegExpCompiler* compiler);
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);
    void CalculateOffsets();
    virtual RegExpNode* FilterASCII(int depth, bool ignore_case);

  private:
    enum TextEmitPassType {
        NON_ASCII_MATCH,             // Check for characters that can't match.
        SIMPLE_CHARACTER_MATCH,      // Case-dependent single character check.
        NON_LETTER_CHARACTER_MATCH,  // Check characters that have no case equivs.
        CASE_CHARACTER_MATCH,        // Case-independent single character check.
        CHARACTER_CLASS_MATCH        // Character class.
    };
    static bool SkipPass(int pass, bool ignore_case);
    static const int kFirstRealPass = SIMPLE_CHARACTER_MATCH;
    static const int kLastPass = CHARACTER_CLASS_MATCH;
    void TextEmitPass(RegExpCompiler* compiler,
                      TextEmitPassType pass,
                      bool preloaded,
                      Trace* trace,
                      bool first_element_checked,
                      int* checked_up_to);
    int Length();
    TextElementVector* elements_;
};

class AssertionNode : public SeqRegExpNode
{
  public:
    enum AssertionType {
        AT_END,
        AT_START,
        AT_BOUNDARY,
        AT_NON_BOUNDARY,
        AFTER_NEWLINE
    };
    AssertionNode(AssertionType t, RegExpNode* on_success)
      : SeqRegExpNode(on_success), assertion_type_(t)
    {}

    static AssertionNode* AtEnd(RegExpNode* on_success) {
        return on_success->alloc()->newInfallible<AssertionNode>(AT_END, on_success);
    }
    static AssertionNode* AtStart(RegExpNode* on_success) {
        return on_success->alloc()->newInfallible<AssertionNode>(AT_START, on_success);
    }
    static AssertionNode* AtBoundary(RegExpNode* on_success) {
        return on_success->alloc()->newInfallible<AssertionNode>(AT_BOUNDARY, on_success);
    }
    static AssertionNode* AtNonBoundary(RegExpNode* on_success) {
        return on_success->alloc()->newInfallible<AssertionNode>(AT_NON_BOUNDARY, on_success);
    }
    static AssertionNode* AfterNewline(RegExpNode* on_success) {
        return on_success->alloc()->newInfallible<AssertionNode>(AFTER_NEWLINE, on_success);
    }
    virtual void Accept(NodeVisitor* visitor);
    virtual void Emit(RegExpCompiler* compiler, Trace* trace);
    virtual int EatsAtLeast(int still_to_find, int budget, bool not_at_start);
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int filled_in,
                                      bool not_at_start);
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);
    AssertionType assertion_type() { return assertion_type_; }

  private:
    void EmitBoundaryCheck(RegExpCompiler* compiler, Trace* trace);
    enum IfPrevious { kIsNonWord, kIsWord };
    void BacktrackIfPrevious(RegExpCompiler* compiler,
                             Trace* trace,
                             IfPrevious backtrack_if_previous);
    AssertionType assertion_type_;
};

class BackReferenceNode : public SeqRegExpNode
{
  public:
    BackReferenceNode(int start_reg,
                      int end_reg,
                      RegExpNode* on_success)
      : SeqRegExpNode(on_success),
        start_reg_(start_reg),
        end_reg_(end_reg)
    {}

    virtual void Accept(NodeVisitor* visitor);
    int start_register() { return start_reg_; }
    int end_register() { return end_reg_; }
    virtual void Emit(RegExpCompiler* compiler, Trace* trace);
    virtual int EatsAtLeast(int still_to_find,
                            int recursion_depth,
                            bool not_at_start);
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int characters_filled_in,
                                      bool not_at_start) {
        return;
    }
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);

  private:
    int start_reg_;
    int end_reg_;
};

class EndNode : public RegExpNode
{
  public:
    enum Action { ACCEPT, BACKTRACK, NEGATIVE_SUBMATCH_SUCCESS };

    explicit EndNode(LifoAlloc* alloc, Action action)
      : RegExpNode(alloc), action_(action)
    {}

    virtual void Accept(NodeVisitor* visitor);
    virtual void Emit(RegExpCompiler* compiler, Trace* trace);
    virtual int EatsAtLeast(int still_to_find,
                            int recursion_depth,
                            bool not_at_start) { return 0; }
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int characters_filled_in,
                                      bool not_at_start)
    {
        // Returning 0 from EatsAtLeast should ensure we never get here.
        MOZ_CRASH("Bad call");
    }
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start) {
        // Returning 0 from EatsAtLeast should ensure we never get here.
        MOZ_CRASH("Bad call");
    }

  private:
    Action action_;
};

class NegativeSubmatchSuccess : public EndNode
{
  public:
    NegativeSubmatchSuccess(LifoAlloc* alloc,
                            int stack_pointer_reg,
                            int position_reg,
                            int clear_capture_count,
                            int clear_capture_start)
      : EndNode(alloc, NEGATIVE_SUBMATCH_SUCCESS),
        stack_pointer_register_(stack_pointer_reg),
        current_position_register_(position_reg),
        clear_capture_count_(clear_capture_count),
        clear_capture_start_(clear_capture_start)
    {}

    virtual void Emit(RegExpCompiler* compiler, Trace* trace);

  private:
    int stack_pointer_register_;
    int current_position_register_;
    int clear_capture_count_;
    int clear_capture_start_;
};

class Guard
{
  public:
    enum Relation { LT, GEQ };
    Guard(int reg, Relation op, int value)
        : reg_(reg),
          op_(op),
          value_(value)
    {}

    int reg() { return reg_; }
    Relation op() { return op_; }
    int value() { return value_; }

  private:
    int reg_;
    Relation op_;
    int value_;
};

typedef Vector<Guard*, 1, LifoAllocPolicy<Infallible> > GuardVector;

class GuardedAlternative
{
  public:
    explicit GuardedAlternative(RegExpNode* node)
      : node_(node), guards_(nullptr)
    {}

    void AddGuard(LifoAlloc* alloc, Guard* guard);
    RegExpNode* node() const { return node_; }
    void set_node(RegExpNode* node) { node_ = node; }
    const GuardVector* guards() const { return guards_; }

  private:
    RegExpNode* node_;
    GuardVector* guards_;
};

typedef Vector<GuardedAlternative, 0, LifoAllocPolicy<Infallible> > GuardedAlternativeVector;

class AlternativeGeneration;

class ChoiceNode : public RegExpNode
{
  public:
    explicit ChoiceNode(LifoAlloc* alloc, int expected_size)
      : RegExpNode(alloc),
        alternatives_(*alloc),
        table_(nullptr),
        not_at_start_(false),
        being_calculated_(false)
    {
        alternatives_.reserve(expected_size);
    }

    virtual void Accept(NodeVisitor* visitor);
    void AddAlternative(GuardedAlternative node) {
        alternatives_.append(node);
    }

    GuardedAlternativeVector& alternatives() { return alternatives_; }
    DispatchTable* GetTable(bool ignore_case);
    virtual void Emit(RegExpCompiler* compiler, Trace* trace);
    virtual int EatsAtLeast(int still_to_find, int budget, bool not_at_start);
    int EatsAtLeastHelper(int still_to_find,
                          int budget,
                          RegExpNode* ignore_this_node,
                          bool not_at_start);
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int characters_filled_in,
                                      bool not_at_start);
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);

    bool being_calculated() { return being_calculated_; }
    bool not_at_start() { return not_at_start_; }
    void set_not_at_start() { not_at_start_ = true; }
    void set_being_calculated(bool b) { being_calculated_ = b; }
    virtual bool try_to_emit_quick_check_for_alternative(int i) { return true; }
    virtual RegExpNode* FilterASCII(int depth, bool ignore_case);

  protected:
    int GreedyLoopTextLengthForAlternative(GuardedAlternative* alternative);
    GuardedAlternativeVector alternatives_;

  private:
    friend class Analysis;
    void GenerateGuard(RegExpMacroAssembler* macro_assembler,
                       Guard* guard,
                       Trace* trace);
    int CalculatePreloadCharacters(RegExpCompiler* compiler, int eats_at_least);
    void EmitOutOfLineContinuation(RegExpCompiler* compiler,
                                   Trace* trace,
                                   GuardedAlternative alternative,
                                   AlternativeGeneration* alt_gen,
                                   int preload_characters,
                                   bool next_expects_preload);
    DispatchTable* table_;

    // If true, this node is never checked at the start of the input.
    // Allows a new trace to start with at_start() set to false.
    bool not_at_start_;
    bool being_calculated_;
};

class NegativeLookaheadChoiceNode : public ChoiceNode
{
  public:
    explicit NegativeLookaheadChoiceNode(LifoAlloc* alloc,
                                         GuardedAlternative this_must_fail,
                                         GuardedAlternative then_do_this)
      : ChoiceNode(alloc, 2)
    {
        AddAlternative(this_must_fail);
        AddAlternative(then_do_this);
    }
    virtual int EatsAtLeast(int still_to_find, int budget, bool not_at_start);
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int characters_filled_in,
                                      bool not_at_start);
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);

    // For a negative lookahead we don't emit the quick check for the
    // alternative that is expected to fail.  This is because quick check code
    // starts by loading enough characters for the alternative that takes fewest
    // characters, but on a negative lookahead the negative branch did not take
    // part in that calculation (EatsAtLeast) so the assumptions don't hold.
    virtual bool try_to_emit_quick_check_for_alternative(int i) { return i != 0; }
    virtual RegExpNode* FilterASCII(int depth, bool ignore_case);
};

class LoopChoiceNode : public ChoiceNode
{
  public:
    explicit LoopChoiceNode(LifoAlloc* alloc, bool body_can_be_zero_length)
      : ChoiceNode(alloc, 2),
        loop_node_(nullptr),
        continue_node_(nullptr),
        body_can_be_zero_length_(body_can_be_zero_length)
    {}

    void AddLoopAlternative(GuardedAlternative alt);
    void AddContinueAlternative(GuardedAlternative alt);
    virtual void Emit(RegExpCompiler* compiler, Trace* trace);
    virtual int EatsAtLeast(int still_to_find,  int budget, bool not_at_start);
    virtual void GetQuickCheckDetails(QuickCheckDetails* details,
                                      RegExpCompiler* compiler,
                                      int characters_filled_in,
                                      bool not_at_start);
    virtual bool FillInBMInfo(int offset,
                              int budget,
                              BoyerMooreLookahead* bm,
                              bool not_at_start);
    RegExpNode* loop_node() { return loop_node_; }
    RegExpNode* continue_node() { return continue_node_; }
    bool body_can_be_zero_length() { return body_can_be_zero_length_; }
    virtual void Accept(NodeVisitor* visitor);
    virtual RegExpNode* FilterASCII(int depth, bool ignore_case);

  private:
    // AddAlternative is made private for loop nodes because alternatives
    // should not be added freely, we need to keep track of which node
    // goes back to the node itself.
    void AddAlternative(GuardedAlternative node) {
        ChoiceNode::AddAlternative(node);
    }

    RegExpNode* loop_node_;
    RegExpNode* continue_node_;
    bool body_can_be_zero_length_;
};

// Improve the speed that we scan for an initial point where a non-anchored
// regexp can match by using a Boyer-Moore-like table. This is done by
// identifying non-greedy non-capturing loops in the nodes that eat any
// character one at a time.  For example in the middle of the regexp
// /foo[\s\S]*?bar/ we find such a loop.  There is also such a loop implicitly
// inserted at the start of any non-anchored regexp.
//
// When we have found such a loop we look ahead in the nodes to find the set of
// characters that can come at given distances. For example for the regexp
// /.?foo/ we know that there are at least 3 characters ahead of us, and the
// sets of characters that can occur are [any, [f, o], [o]]. We find a range in
// the lookahead info where the set of characters is reasonably constrained. In
// our example this is from index 1 to 2 (0 is not constrained). We can now
// look 3 characters ahead and if we don't find one of [f, o] (the union of
// [f, o] and [o]) then we can skip forwards by the range size (in this case 2).
//
// For Unicode input strings we do the same, but modulo 128.
//
// We also look at the first string fed to the regexp and use that to get a hint
// of the character frequencies in the inputs. This affects the assessment of
// whether the set of characters is 'reasonably constrained'.
//
// We also have another lookahead mechanism (called quick check in the code),
// which uses a wide load of multiple characters followed by a mask and compare
// to determine whether a match is possible at this point.
enum ContainedInLattice {
  kNotYet = 0,
  kLatticeIn = 1,
  kLatticeOut = 2,
  kLatticeUnknown = 3  // Can also mean both in and out.
};

inline ContainedInLattice
Combine(ContainedInLattice a, ContainedInLattice b) {
    return static_cast<ContainedInLattice>(a | b);
}

ContainedInLattice
AddRange(ContainedInLattice a,
         const int* ranges,
         int ranges_size,
         Interval new_range);

class BoyerMoorePositionInfo
{
  public:
    explicit BoyerMoorePositionInfo(LifoAlloc* alloc)
      : map_(*alloc),
        map_count_(0),
        w_(kNotYet),
        s_(kNotYet),
        d_(kNotYet),
        surrogate_(kNotYet)
    {
        map_.reserve(kMapSize);
        for (int i = 0; i < kMapSize; i++)
            map_.append(false);
    }

    bool& at(int i) { return map_[i]; }

    static const int kMapSize = 128;
    static const int kMask = kMapSize - 1;

    int map_count() const { return map_count_; }

    void Set(int character);
    void SetInterval(const Interval& interval);
    void SetAll();
    bool is_non_word() { return w_ == kLatticeOut; }
    bool is_word() { return w_ == kLatticeIn; }

  private:
    Vector<bool, 0, LifoAllocPolicy<Infallible> > map_;
    int map_count_;  // Number of set bits in the map.
    ContainedInLattice w_;  // The \w character class.
    ContainedInLattice s_;  // The \s character class.
    ContainedInLattice d_;  // The \d character class.
    ContainedInLattice surrogate_;  // Surrogate UTF-16 code units.
};

typedef Vector<BoyerMoorePositionInfo*, 1, LifoAllocPolicy<Infallible> > BoyerMoorePositionInfoVector;

class BoyerMooreLookahead
{
  public:
    BoyerMooreLookahead(LifoAlloc* alloc, size_t length, RegExpCompiler* compiler);

    int length() { return length_; }
    int max_char() { return max_char_; }
    RegExpCompiler* compiler() { return compiler_; }

    int Count(int map_number) {
        return bitmaps_[map_number]->map_count();
    }

    BoyerMoorePositionInfo* at(int i) { return bitmaps_[i]; }

    void Set(int map_number, int character) {
        if (character > max_char_) return;
        BoyerMoorePositionInfo* info = bitmaps_[map_number];
        info->Set(character);
    }

    void SetInterval(int map_number, const Interval& interval) {
        if (interval.from() > max_char_) return;
        BoyerMoorePositionInfo* info = bitmaps_[map_number];
        if (interval.to() > max_char_) {
            info->SetInterval(Interval(interval.from(), max_char_));
        } else {
            info->SetInterval(interval);
        }
    }

    void SetAll(int map_number) {
        bitmaps_[map_number]->SetAll();
    }

    void SetRest(int from_map) {
        for (int i = from_map; i < length_; i++) SetAll(i);
    }
    bool EmitSkipInstructions(RegExpMacroAssembler* masm);

    bool CheckOverRecursed();

  private:
    // This is the value obtained by EatsAtLeast.  If we do not have at least this
    // many characters left in the sample string then the match is bound to fail.
    // Therefore it is OK to read a character this far ahead of the current match
    // point.
    int length_;
    RegExpCompiler* compiler_;

    // 0x7f for ASCII, 0xffff for UTF-16.
    int max_char_;
    BoyerMoorePositionInfoVector bitmaps_;

    int GetSkipTable(int min_lookahead,
                     int max_lookahead,
                     uint8_t* boolean_skip_table);
    bool FindWorthwhileInterval(int* from, int* to);
    int FindBestInterval(int max_number_of_chars, int old_biggest_points, int* from, int* to);
};

// There are many ways to generate code for a node.  This class encapsulates
// the current way we should be generating.  In other words it encapsulates
// the current state of the code generator.  The effect of this is that we
// generate code for paths that the matcher can take through the regular
// expression.  A given node in the regexp can be code-generated several times
// as it can be part of several traces.  For example for the regexp:
// /foo(bar|ip)baz/ the code to match baz will be generated twice, once as part
// of the foo-bar-baz trace and once as part of the foo-ip-baz trace.  The code
// to match foo is generated only once (the traces have a common prefix).  The
// code to store the capture is deferred and generated (twice) after the places
// where baz has been matched.
class Trace
{
  public:
    // A value for a property that is either known to be true, know to be false,
    // or not known.
    enum TriBool {
        UNKNOWN = -1, FALSE_VALUE = 0, TRUE_VALUE = 1
    };

    class DeferredAction {
      public:
        DeferredAction(ActionNode::ActionType action_type, int reg)
          : action_type_(action_type), reg_(reg), next_(nullptr)
        {}

        DeferredAction* next() { return next_; }
        bool Mentions(int reg);
        int reg() { return reg_; }
        ActionNode::ActionType action_type() { return action_type_; }
      private:
        ActionNode::ActionType action_type_;
        int reg_;
        DeferredAction* next_;
        friend class Trace;
    };

    class DeferredCapture : public DeferredAction {
      public:
        DeferredCapture(int reg, bool is_capture, Trace* trace)
          : DeferredAction(ActionNode::STORE_POSITION, reg),
            cp_offset_(trace->cp_offset()),
            is_capture_(is_capture)
        {}

        int cp_offset() { return cp_offset_; }
        bool is_capture() { return is_capture_; }
      private:
        int cp_offset_;
        bool is_capture_;
        void set_cp_offset(int cp_offset) { cp_offset_ = cp_offset; }
    };

    class DeferredSetRegister : public DeferredAction {
      public:
        DeferredSetRegister(int reg, int value)
          : DeferredAction(ActionNode::SET_REGISTER, reg),
            value_(value)
        {}
        int value() { return value_; }
      private:
        int value_;
    };

    class DeferredClearCaptures : public DeferredAction {
      public:
        explicit DeferredClearCaptures(Interval range)
          : DeferredAction(ActionNode::CLEAR_CAPTURES, -1),
            range_(range)
        {}

        Interval range() { return range_; }
      private:
        Interval range_;
    };

    class DeferredIncrementRegister : public DeferredAction {
      public:
        explicit DeferredIncrementRegister(int reg)
          : DeferredAction(ActionNode::INCREMENT_REGISTER, reg)
        {}
    };

    Trace()
      : cp_offset_(0),
        actions_(nullptr),
        backtrack_(nullptr),
        stop_node_(nullptr),
        loop_label_(nullptr),
        characters_preloaded_(0),
        bound_checked_up_to_(0),
        flush_budget_(100),
        at_start_(UNKNOWN)
    {}

    // End the trace.  This involves flushing the deferred actions in the trace
    // and pushing a backtrack location onto the backtrack stack.  Once this is
    // done we can start a new trace or go to one that has already been
    // generated.
    void Flush(RegExpCompiler* compiler, RegExpNode* successor);

    int cp_offset() { return cp_offset_; }
    DeferredAction* actions() { return actions_; }

    // A trivial trace is one that has no deferred actions or other state that
    // affects the assumptions used when generating code.  There is no recorded
    // backtrack location in a trivial trace, so with a trivial trace we will
    // generate code that, on a failure to match, gets the backtrack location
    // from the backtrack stack rather than using a direct jump instruction.  We
    // always start code generation with a trivial trace and non-trivial traces
    // are created as we emit code for nodes or add to the list of deferred
    // actions in the trace.  The location of the code generated for a node using
    // a trivial trace is recorded in a label in the node so that gotos can be
    // generated to that code.
    bool is_trivial() {
        return backtrack_ == nullptr &&
            actions_ == nullptr &&
            cp_offset_ == 0 &&
            characters_preloaded_ == 0 &&
            bound_checked_up_to_ == 0 &&
            quick_check_performed_.characters() == 0 &&
            at_start_ == UNKNOWN;
    }

    TriBool at_start() { return at_start_; }
    void set_at_start(bool at_start) {
        at_start_ = at_start ? TRUE_VALUE : FALSE_VALUE;
    }
    jit::Label* backtrack() { return backtrack_; }
    jit::Label* loop_label() { return loop_label_; }
    RegExpNode* stop_node() { return stop_node_; }
    int characters_preloaded() { return characters_preloaded_; }
    int bound_checked_up_to() { return bound_checked_up_to_; }
    int flush_budget() { return flush_budget_; }
    QuickCheckDetails* quick_check_performed() { return &quick_check_performed_; }
    bool mentions_reg(int reg);

    // Returns true if a deferred position store exists to the specified
    // register and stores the offset in the out-parameter.  Otherwise
    // returns false.
    bool GetStoredPosition(int reg, int* cp_offset);

    // These set methods and AdvanceCurrentPositionInTrace should be used only on
    // new traces - the intention is that traces are immutable after creation.
    void add_action(DeferredAction* new_action) {
        MOZ_ASSERT(new_action->next_ == nullptr);
        new_action->next_ = actions_;
        actions_ = new_action;
    }

    void set_backtrack(jit::Label* backtrack) { backtrack_ = backtrack; }
    void set_stop_node(RegExpNode* node) { stop_node_ = node; }
    void set_loop_label(jit::Label* label) { loop_label_ = label; }
    void set_characters_preloaded(int count) { characters_preloaded_ = count; }
    void set_bound_checked_up_to(int to) { bound_checked_up_to_ = to; }
    void set_flush_budget(int to) { flush_budget_ = to; }
    void set_quick_check_performed(QuickCheckDetails* d) {
        quick_check_performed_ = *d;
    }
    void InvalidateCurrentCharacter();
    void AdvanceCurrentPositionInTrace(int by, RegExpCompiler* compiler);

  private:
    int FindAffectedRegisters(LifoAlloc* alloc, OutSet* affected_registers);
    void PerformDeferredActions(LifoAlloc* alloc,
                                RegExpMacroAssembler* macro,
                                int max_register,
                                OutSet& affected_registers,
                                OutSet* registers_to_pop,
                                OutSet* registers_to_clear);
    void RestoreAffectedRegisters(RegExpMacroAssembler* macro,
                                  int max_register,
                                  OutSet& registers_to_pop,
                                  OutSet& registers_to_clear);
    int cp_offset_;
    DeferredAction* actions_;
    jit::Label* backtrack_;
    RegExpNode* stop_node_;
    jit::Label* loop_label_;
    int characters_preloaded_;
    int bound_checked_up_to_;
    QuickCheckDetails quick_check_performed_;
    int flush_budget_;
    TriBool at_start_;
};

class NodeVisitor
{
  public:
    virtual ~NodeVisitor() { }
#define DECLARE_VISIT(Type)                                          \
    virtual void Visit##Type(Type##Node* that) = 0;
    FOR_EACH_NODE_TYPE(DECLARE_VISIT)
#undef DECLARE_VISIT
    virtual void VisitLoopChoice(LoopChoiceNode* that) { VisitChoice(that); }
};

// Assertion propagation moves information about assertions such as
// \b to the affected nodes.  For instance, in /.\b./ information must
// be propagated to the first '.' that whatever follows needs to know
// if it matched a word or a non-word, and to the second '.' that it
// has to check if it succeeds a word or non-word.  In this case the
// result will be something like:
//
//   +-------+        +------------+
//   |   .   |        |      .     |
//   +-------+  --->  +------------+
//   | word? |        | check word |
//   +-------+        +------------+
class Analysis : public NodeVisitor
{
  public:
    Analysis(JSContext* cx, bool ignore_case, bool is_ascii)
      : cx(cx),
        ignore_case_(ignore_case),
        is_ascii_(is_ascii),
        error_message_(nullptr)
    {}

    void EnsureAnalyzed(RegExpNode* node);

#define DECLARE_VISIT(Type)                     \
    virtual void Visit##Type(Type##Node* that);
    FOR_EACH_NODE_TYPE(DECLARE_VISIT)
#undef DECLARE_VISIT
    virtual void VisitLoopChoice(LoopChoiceNode* that);

    bool has_failed() { return error_message_ != nullptr; }
    const char* errorMessage() {
        MOZ_ASSERT(error_message_ != nullptr);
        return error_message_;
    }
    void fail(const char* error_message) {
        error_message_ = error_message;
    }

  private:
    JSContext* cx;
    bool ignore_case_;
    bool is_ascii_;
    const char* error_message_;

    Analysis(Analysis&) = delete;
    void operator=(Analysis&) = delete;
};

} }  // namespace js::irregexp

#endif  // V8_JSREGEXP_H_
