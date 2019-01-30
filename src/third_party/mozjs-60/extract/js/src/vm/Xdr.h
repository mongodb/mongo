/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef vm_Xdr_h
#define vm_Xdr_h

#include "mozilla/EndianUtils.h"
#include "mozilla/TypeTraits.h"

#include "jsfriendapi.h"
#include "NamespaceImports.h"

#include "js/TypeDecls.h"
#include "vm/JSAtom.h"

namespace js {

class LifoAlloc;

class XDRBufferBase
{
  public:
    explicit XDRBufferBase(JSContext* cx, size_t cursor = 0)
      : context_(cx), cursor_(cursor) { }

    JSContext* cx() const {
        return context_;
    }

    size_t cursor() const {
        return cursor_;
    }

  protected:
    JSContext* const context_;
    size_t cursor_;
};

template <XDRMode mode>
class XDRBuffer;

template <>
class XDRBuffer<XDR_ENCODE> : public XDRBufferBase
{
  public:
    XDRBuffer(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(cx, cursor),
        buffer_(buffer) { }

    uint8_t* write(size_t n) {
        MOZ_ASSERT(n != 0);
        if (!buffer_.growByUninitialized(n)) {
            ReportOutOfMemory(cx());
            return nullptr;
        }
        uint8_t* ptr = &buffer_[cursor_];
        cursor_ += n;
        return ptr;
    }

    const uint8_t* read(size_t n) {
        MOZ_CRASH("Should never read in encode mode");
        return nullptr;
    }

  private:
    JS::TranscodeBuffer& buffer_;
};

template <>
class XDRBuffer<XDR_DECODE> : public XDRBufferBase
{
  public:
    XDRBuffer(JSContext* cx, const JS::TranscodeRange& range)
      : XDRBufferBase(cx),
        buffer_(range) { }

    XDRBuffer(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : XDRBufferBase(cx, cursor),
        buffer_(buffer.begin(), buffer.length()) { }

    const uint8_t* read(size_t n) {
        MOZ_ASSERT(cursor_ < buffer_.length());
        uint8_t* ptr = &buffer_[cursor_];
        cursor_ += n;

        // Don't let buggy code read past our buffer
        if (cursor_ > buffer_.length())
            return nullptr;

        return ptr;
    }

    uint8_t* write(size_t n) {
        MOZ_CRASH("Should never write in decode mode");
        return nullptr;
    }

  private:
    const JS::TranscodeRange buffer_;
};

class XDRCoderBase;
class XDRIncrementalEncoder;

// An AutoXDRTree is used to identify section encoded by an XDRIncrementalEncoder.
//
// Its primary goal is to identify functions, such that we can first encode them
// as LazyScript, and later replaced by them by their corresponding bytecode
// once delazified.
//
// As a convenience, this is also used to identify the top-level of the content
// encoded by an XDRIncrementalEncoder.
//
// Sections can be encoded any number of times in an XDRIncrementalEncoder, and
// the latest encoded version would replace all the previous one.
class MOZ_RAII AutoXDRTree
{
  public:
    // For a JSFunction, a tree key is defined as being:
    //     script()->begin << 32 | script()->end
    //
    // Based on the invariant that |begin <= end|, we can make special
    // keys, such as the top-level script.
    using Key = uint64_t;

    AutoXDRTree(XDRCoderBase* xdr, Key key);
    ~AutoXDRTree();

    // Indicate the lack of a key for the current tree.
    static constexpr Key noKey = 0;

    // Used to end the slices when there is no children.
    static constexpr Key noSubTree = Key(1) << 32;

    // Used as the root key of the tree in the hash map.
    static constexpr Key topLevel = Key(2) << 32;

  private:
    friend class XDRIncrementalEncoder;

    Key key_;
    AutoXDRTree* parent_;
    XDRCoderBase* xdr_;
};

class XDRCoderBase
{
  protected:
    XDRCoderBase() {}

  public:
    virtual AutoXDRTree::Key getTopLevelTreeKey() const { return AutoXDRTree::noKey; }
    virtual AutoXDRTree::Key getTreeKey(JSFunction* fun) const { return AutoXDRTree::noKey; }
    virtual void createOrReplaceSubTree(AutoXDRTree* child) {};
    virtual void endSubTree() {};
};

/*
 * XDR serialization state.  All data is encoded in little endian.
 */
template <XDRMode mode>
class XDRState : public XDRCoderBase
{
  protected:
    XDRBuffer<mode> buf;
  private:
    JS::TranscodeResult resultCode_;

  public:
    XDRState(JSContext* cx, JS::TranscodeBuffer& buffer, size_t cursor = 0)
      : buf(cx, buffer, cursor),
        resultCode_(JS::TranscodeResult_Ok)
    {
    }

    template <typename RangeType>
    XDRState(JSContext* cx, const RangeType& range)
      : buf(cx, range),
        resultCode_(JS::TranscodeResult_Ok)
    {
    }

    virtual ~XDRState() {};

    JSContext* cx() const {
        return buf.cx();
    }
    virtual LifoAlloc& lifoAlloc() const;

    virtual bool hasOptions() const { return false; }
    virtual const ReadOnlyCompileOptions& options() {
        MOZ_CRASH("does not have options");
    }
    virtual bool hasScriptSourceObjectOut() const { return false; }
    virtual ScriptSourceObject** scriptSourceObjectOut() {
        MOZ_CRASH("does not have scriptSourceObjectOut.");
    }

    // Record logical failures of XDR.
    void postProcessContextErrors(JSContext* cx);
    JS::TranscodeResult resultCode() const {
        return resultCode_;
    }
    bool fail(JS::TranscodeResult code) {
        MOZ_ASSERT(resultCode_ == JS::TranscodeResult_Ok);
        resultCode_ = code;
        return false;
    }

    bool peekData(const uint8_t** pptr, size_t length) {
        const uint8_t* ptr = buf.read(length);
        if (!ptr)
            return fail(JS::TranscodeResult_Failure_BadDecode);
        *pptr = ptr;
        return true;
    }

    bool codeUint8(uint8_t* n) {
        if (mode == XDR_ENCODE) {
            uint8_t* ptr = buf.write(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Throw);
            *ptr = *n;
        } else {
            const uint8_t* ptr = buf.read(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Failure_BadDecode);
            *n = *ptr;
        }
        return true;
    }

    bool codeUint16(uint16_t* n) {
        if (mode == XDR_ENCODE) {
            uint8_t* ptr = buf.write(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Throw);
            mozilla::LittleEndian::writeUint16(ptr, *n);
        } else {
            const uint8_t* ptr = buf.read(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Failure_BadDecode);
            *n = mozilla::LittleEndian::readUint16(ptr);
        }
        return true;
    }

    bool codeUint32(uint32_t* n) {
        if (mode == XDR_ENCODE) {
            uint8_t* ptr = buf.write(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Throw);
            mozilla::LittleEndian::writeUint32(ptr, *n);
        } else {
            const uint8_t* ptr = buf.read(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Failure_BadDecode);
            *n = mozilla::LittleEndian::readUint32(ptr);
        }
        return true;
    }

    bool codeUint64(uint64_t* n) {
        if (mode == XDR_ENCODE) {
            uint8_t* ptr = buf.write(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Throw);
            mozilla::LittleEndian::writeUint64(ptr, *n);
        } else {
            const uint8_t* ptr = buf.read(sizeof(*n));
            if (!ptr)
                return fail(JS::TranscodeResult_Failure_BadDecode);
            *n = mozilla::LittleEndian::readUint64(ptr);
        }
        return true;
    }

    /*
     * Use SFINAE to refuse any specialization which is not an enum.  Uses of
     * this function do not have to specialize the type of the enumerated field
     * as C++ will extract the parameterized from the argument list.
     */
    template <typename T>
    bool codeEnum32(T* val, typename mozilla::EnableIf<mozilla::IsEnum<T>::value, T>::Type * = NULL)
    {
        // Mix the enumeration value with a random magic number, such that a
        // corruption with a low-ranged value (like 0) is less likely to cause a
        // miss-interpretation of the XDR content and instead cause a failure.
        const uint32_t MAGIC = 0x21AB218C;
        uint32_t tmp;
        if (mode == XDR_ENCODE)
            tmp = uint32_t(*val) ^ MAGIC;
        if (!codeUint32(&tmp))
            return false;
        if (mode == XDR_DECODE)
            *val = T(tmp ^ MAGIC);
        return true;
    }

    bool codeDouble(double* dp) {
        union DoublePun {
            double d;
            uint64_t u;
        } pun;
        if (mode == XDR_ENCODE)
            pun.d = *dp;
        if (!codeUint64(&pun.u))
            return false;
        if (mode == XDR_DECODE)
            *dp = pun.d;
        return true;
    }

    bool codeMarker(uint32_t magic) {
        uint32_t actual = magic;
        if (!codeUint32(&actual))
            return false;
        if (actual != magic) {
            // Fail in debug, but only soft-fail in release
            MOZ_ASSERT(false, "Bad XDR marker");
            return fail(JS::TranscodeResult_Failure_BadDecode);
        }
        return true;
    }

    bool codeBytes(void* bytes, size_t len) {
        if (len == 0)
            return true;
        if (mode == XDR_ENCODE) {
            uint8_t* ptr = buf.write(len);
            if (!ptr)
                return fail(JS::TranscodeResult_Throw);
            memcpy(ptr, bytes, len);
        } else {
            const uint8_t* ptr = buf.read(len);
            if (!ptr)
                return fail(JS::TranscodeResult_Failure_BadDecode);
            memcpy(bytes, ptr, len);
        }
        return true;
    }

    /*
     * During encoding the string is written into the buffer together with its
     * terminating '\0'. During decoding the method returns a pointer into the
     * decoding buffer and the caller must copy the string if it will outlive
     * the decoding buffer.
     */
    bool codeCString(const char** sp) {
        uint64_t len64;
        if (mode == XDR_ENCODE)
            len64 = (uint64_t)(strlen(*sp) + 1);
        if (!codeUint64(&len64))
            return false;
        size_t len = (size_t) len64;

        if (mode == XDR_ENCODE) {
            uint8_t* ptr = buf.write(len);
            if (!ptr)
                return fail(JS::TranscodeResult_Throw);
            memcpy(ptr, *sp, len);
        } else {
            const uint8_t* ptr = buf.read(len);
            if (!ptr || ptr[len] != '\0')
                return fail(JS::TranscodeResult_Failure_BadDecode);
            *sp = reinterpret_cast<const char*>(ptr);
        }
        return true;
    }

    bool codeChars(const JS::Latin1Char* chars, size_t nchars);
    bool codeChars(char16_t* chars, size_t nchars);

    bool codeFunction(JS::MutableHandleFunction objp, HandleScriptSource sourceObject = nullptr);
    bool codeScript(MutableHandleScript scriptp);
    bool codeConstValue(MutableHandleValue vp);
};

using XDREncoder = XDRState<XDR_ENCODE>;
using XDRDecoder = XDRState<XDR_DECODE>;

class XDROffThreadDecoder : public XDRDecoder
{
    const ReadOnlyCompileOptions* options_;
    ScriptSourceObject** sourceObjectOut_;
    LifoAlloc& alloc_;

  public:
    // Note, when providing an JSContext, where isJSContext is false,
    // then the initialization of the ScriptSourceObject would remain
    // incomplete. Thus, the sourceObjectOut must be used to finish the
    // initialization with ScriptSourceObject::initFromOptions after the
    // decoding.
    //
    // When providing a sourceObjectOut pointer, you have to ensure that it is
    // marked by the GC to avoid dangling pointers.
    XDROffThreadDecoder(JSContext* cx, LifoAlloc& alloc,
                        const ReadOnlyCompileOptions* options,
                        ScriptSourceObject** sourceObjectOut,
                        const JS::TranscodeRange& range)
      : XDRDecoder(cx, range),
        options_(options),
        sourceObjectOut_(sourceObjectOut),
        alloc_(alloc)
    {
        MOZ_ASSERT(options);
        MOZ_ASSERT(sourceObjectOut);
        MOZ_ASSERT(*sourceObjectOut == nullptr);
    }

    LifoAlloc& lifoAlloc() const override {
        return alloc_;
    }

    bool hasOptions() const override { return true; }
    const ReadOnlyCompileOptions& options() override {
        return *options_;
    }
    bool hasScriptSourceObjectOut() const override { return true; }
    ScriptSourceObject** scriptSourceObjectOut() override {
        return sourceObjectOut_;
    }
};

class XDRIncrementalEncoder : public XDREncoder
{
    // The incremental encoder encodes the content of scripts and functions in
    // the XDRBuffer. It can be used to encode multiple times the same AutoXDRTree,
    // and uses its key to identify which part to replace.
    //
    // Internally, this encoder keeps a tree representation of the scopes. Each
    // node is composed of a vector of slices which are interleaved by child
    // nodes.
    //
    // A slice corresponds to an index and a length within the content of the
    // slices_ buffer. The index is updated when a slice is created, and the
    // length is updated when the slice is ended, either by creating a new scope
    // child, or by closing the scope and going back to the parent.
    //
    //                  +---+---+---+
    //        begin     |   |   |   |
    //        length    |   |   |   |
    //        child     | . | . | . |
    //                  +-|-+-|-+---+
    //                    |   |
    //          +---------+   +---------+
    //          |                       |
    //          v                       v
    //      +---+---+                 +---+
    //      |   |   |                 |   |
    //      |   |   |                 |   |
    //      | . | . |                 | . |
    //      +-|-+---+                 +---+
    //        |
    //        |
    //        |
    //        v
    //      +---+
    //      |   |
    //      |   |
    //      | . |
    //      +---+
    //
    //
    // The tree key is used to identify the child nodes, and to make them
    // easily replaceable.
    //
    // The tree is rooted at the |topLevel| key.
    //

    struct Slice {
        size_t sliceBegin;
        size_t sliceLength;
        AutoXDRTree::Key child;
    };

    using SlicesNode = Vector<Slice, 1, SystemAllocPolicy>;
    using SlicesTree = HashMap<AutoXDRTree::Key, SlicesNode, DefaultHasher<AutoXDRTree::Key>,
                               SystemAllocPolicy>;

    // Last opened XDR-tree on the stack.
    AutoXDRTree* scope_;
    // Node corresponding to the opened scope.
    SlicesNode* node_;
    // Tree of slices.
    SlicesTree tree_;
    JS::TranscodeBuffer slices_;
    bool oom_;

  public:
    explicit XDRIncrementalEncoder(JSContext* cx)
      : XDREncoder(cx, slices_, 0),
        scope_(nullptr),
        node_(nullptr),
        oom_(false)
    {
    }

    virtual ~XDRIncrementalEncoder() {}

    AutoXDRTree::Key getTopLevelTreeKey() const override;
    AutoXDRTree::Key getTreeKey(JSFunction* fun) const override;

    MOZ_MUST_USE bool init();

    void createOrReplaceSubTree(AutoXDRTree* child) override;
    void endSubTree() override;

    // Append the content collected during the incremental encoding into the
    // buffer given as argument.
    MOZ_MUST_USE bool linearize(JS::TranscodeBuffer& buffer);
};

} /* namespace js */

#endif /* vm_Xdr_h */
