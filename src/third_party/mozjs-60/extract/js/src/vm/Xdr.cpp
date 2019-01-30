/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "vm/Xdr.h"

#include "mozilla/PodOperations.h"

#include <string.h>

#include "jsapi.h"

#include "vm/Debugger.h"
#include "vm/EnvironmentObject.h"
#include "vm/JSContext.h"
#include "vm/JSScript.h"
#include "vm/TraceLogging.h"

using namespace js;
using mozilla::PodEqual;

template<XDRMode mode>
LifoAlloc&
XDRState<mode>::lifoAlloc() const {
    return buf.cx()->tempLifoAlloc();
}

template<XDRMode mode>
void
XDRState<mode>::postProcessContextErrors(JSContext* cx)
{
    // NOTE: This should only be called on transcode failure. Not all failure
    // paths call XDRState::fail(...), so we should update resultCode_ if it
    // doesn't hold a specific transcode error.

    if (resultCode_ & JS::TranscodeResult_Failure)
        MOZ_ASSERT_IF(!cx->helperThread(), !cx->isExceptionPending());
    else
        resultCode_ = JS::TranscodeResult_Throw;
}

template<XDRMode mode>
bool
XDRState<mode>::codeChars(const Latin1Char* chars, size_t nchars)
{
    static_assert(sizeof(Latin1Char) == sizeof(uint8_t), "Latin1Char must fit in 1 byte");

    MOZ_ASSERT(mode == XDR_ENCODE);

    if (nchars == 0)
        return true;
    uint8_t* ptr = buf.write(nchars);
    if (!ptr)
        return fail(JS::TranscodeResult_Throw);

    mozilla::PodCopy(ptr, chars, nchars);
    return true;
}

template<XDRMode mode>
bool
XDRState<mode>::codeChars(char16_t* chars, size_t nchars)
{
    if (nchars == 0)
        return true;
    size_t nbytes = nchars * sizeof(char16_t);
    if (mode == XDR_ENCODE) {
        uint8_t* ptr = buf.write(nbytes);
        if (!ptr)
            return fail(JS::TranscodeResult_Throw);
        mozilla::NativeEndian::copyAndSwapToLittleEndian(ptr, chars, nchars);
    } else {
        const uint8_t* ptr = buf.read(nbytes);
        if (!ptr)
            return fail(JS::TranscodeResult_Failure_BadDecode);
        mozilla::NativeEndian::copyAndSwapFromLittleEndian(chars, ptr, nchars);
    }
    return true;
}

template<XDRMode mode>
static bool
VersionCheck(XDRState<mode>* xdr)
{
    JS::BuildIdCharVector buildId;
    MOZ_ASSERT(xdr->cx()->buildIdOp());
    if (!xdr->cx()->buildIdOp()(&buildId)) {
        ReportOutOfMemory(xdr->cx());
        return xdr->fail(JS::TranscodeResult_Throw);
    }
    MOZ_ASSERT(!buildId.empty());

    uint32_t buildIdLength;
    if (mode == XDR_ENCODE)
        buildIdLength = buildId.length();

    if (!xdr->codeUint32(&buildIdLength))
        return false;

    if (mode == XDR_DECODE && buildIdLength != buildId.length())
        return xdr->fail(JS::TranscodeResult_Failure_BadBuildId);

    if (mode == XDR_ENCODE) {
        if (!xdr->codeBytes(buildId.begin(), buildIdLength))
            return false;
    } else {
        JS::BuildIdCharVector decodedBuildId;

        // buildIdLength is already checked against the length of current
        // buildId.
        if (!decodedBuildId.resize(buildIdLength)) {
            ReportOutOfMemory(xdr->cx());
            return xdr->fail(JS::TranscodeResult_Throw);
        }

        if (!xdr->codeBytes(decodedBuildId.begin(), buildIdLength))
            return false;

        // We do not provide binary compatibility with older scripts.
        if (!PodEqual(decodedBuildId.begin(), buildId.begin(), buildIdLength))
            return xdr->fail(JS::TranscodeResult_Failure_BadBuildId);
    }

    return true;
}

template<XDRMode mode>
bool
XDRState<mode>::codeFunction(MutableHandleFunction funp, HandleScriptSource sourceObject)
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx());
    TraceLoggerTextId event =
        mode == XDR_DECODE ? TraceLogger_DecodeFunction : TraceLogger_EncodeFunction;
    AutoTraceLog tl(logger, event);

    RootedScope scope(cx(), &cx()->global()->emptyGlobalScope());
    if (mode == XDR_DECODE) {
        MOZ_ASSERT(!sourceObject);
        funp.set(nullptr);
    } else if (getTreeKey(funp) != AutoXDRTree::noKey) {
        MOZ_ASSERT(sourceObject);
        scope = funp->nonLazyScript()->enclosingScope();
    } else {
        MOZ_ASSERT(!sourceObject);
        MOZ_ASSERT(funp->nonLazyScript()->enclosingScope()->is<GlobalScope>());
    }

    if (!VersionCheck(this)) {
        postProcessContextErrors(cx());
        return false;
    }

    if (!XDRInterpretedFunction(this, scope, sourceObject, funp)) {
        postProcessContextErrors(cx());
        funp.set(nullptr);
        return false;
    }

    return true;
}

template<XDRMode mode>
bool
XDRState<mode>::codeScript(MutableHandleScript scriptp)
{
    TraceLoggerThread* logger = TraceLoggerForCurrentThread(cx());
    TraceLoggerTextId event =
        mode == XDR_DECODE ? TraceLogger_DecodeScript : TraceLogger_EncodeScript;
    AutoTraceLog tl(logger, event);

    AutoXDRTree scriptTree(this, getTopLevelTreeKey());

    if (mode == XDR_DECODE)
        scriptp.set(nullptr);
    else
        MOZ_ASSERT(!scriptp->enclosingScope());

    if (!VersionCheck(this)) {
        postProcessContextErrors(cx());
        return false;
    }

    if (!XDRScript(this, nullptr, nullptr, nullptr, scriptp)) {
        postProcessContextErrors(cx());
        scriptp.set(nullptr);
        return false;
    }

    return true;
}

template<XDRMode mode>
bool
XDRState<mode>::codeConstValue(MutableHandleValue vp)
{
    return XDRScriptConst(this, vp);
}

template class js::XDRState<XDR_ENCODE>;
template class js::XDRState<XDR_DECODE>;

AutoXDRTree::AutoXDRTree(XDRCoderBase* xdr, AutoXDRTree::Key key)
  : key_(key),
    parent_(this),
    xdr_(xdr)
{
    if (key_ != AutoXDRTree::noKey)
        xdr->createOrReplaceSubTree(this);
}

AutoXDRTree::~AutoXDRTree()
{
    if (key_ != AutoXDRTree::noKey)
        xdr_->endSubTree();
}

constexpr AutoXDRTree::Key AutoXDRTree::noKey;
constexpr AutoXDRTree::Key AutoXDRTree::noSubTree;
constexpr AutoXDRTree::Key AutoXDRTree::topLevel;

AutoXDRTree::Key
XDRIncrementalEncoder::getTopLevelTreeKey() const
{
    return AutoXDRTree::topLevel;
}

AutoXDRTree::Key
XDRIncrementalEncoder::getTreeKey(JSFunction* fun) const
{
    if (fun->isInterpretedLazy()) {
        static_assert(sizeof(fun->lazyScript()->begin()) == 4 ||
                      sizeof(fun->lazyScript()->end()) == 4,
                      "AutoXDRTree key requires LazyScripts positions to be uint32");
        return uint64_t(fun->lazyScript()->begin()) << 32 | fun->lazyScript()->end();
    }

    if (fun->isInterpreted()) {
        static_assert(sizeof(fun->nonLazyScript()->sourceStart()) == 4 ||
                      sizeof(fun->nonLazyScript()->sourceEnd()) == 4,
                      "AutoXDRTree key requires JSScripts positions to be uint32");
        return uint64_t(fun->nonLazyScript()->sourceStart()) << 32 | fun->nonLazyScript()->sourceEnd();
    }

    return AutoXDRTree::noKey;
}

bool
XDRIncrementalEncoder::init()
{
    if (!tree_.init())
        return false;
    return true;
}

void
XDRIncrementalEncoder::createOrReplaceSubTree(AutoXDRTree* child)
{
    AutoXDRTree* parent = scope_;
    child->parent_ = parent;
    scope_ = child;
    if (oom_)
        return;

    size_t cursor = buf.cursor();

    // End the parent slice here, set the key to the child.
    if (parent) {
        Slice& last = node_->back();
        last.sliceLength = cursor - last.sliceBegin;
        last.child = child->key_;
        MOZ_ASSERT_IF(uint32_t(parent->key_) != 0,
                      uint32_t(parent->key_ >> 32) <= uint32_t(child->key_ >> 32) &&
                      uint32_t(child->key_) <= uint32_t(parent->key_));
    }

    // Create or replace the part with what is going to be encoded next.
    SlicesTree::AddPtr p = tree_.lookupForAdd(child->key_);
    SlicesNode tmp;
    if (!p) {
        // Create a new sub-tree node.
        if (!tree_.add(p, child->key_, mozilla::Move(tmp))) {
            oom_ = true;
            return;
        }
    } else {
        // Replace an exisiting sub-tree.
        p->value() = mozilla::Move(tmp);
    }
    node_ = &p->value();

    // Add content to the root of the new sub-tree,
    // i-e an empty slice with no children.
    if (!node_->append(Slice { cursor, 0, AutoXDRTree::noSubTree }))
        MOZ_CRASH("SlicesNode have a reserved space of 1.");
}

void
XDRIncrementalEncoder::endSubTree()
{
    AutoXDRTree* child = scope_;
    AutoXDRTree* parent = child->parent_;
    scope_ = parent;
    if (oom_)
        return;

    size_t cursor = buf.cursor();

    // End the child sub-tree.
    Slice& last = node_->back();
    last.sliceLength = cursor - last.sliceBegin;
    MOZ_ASSERT(last.child == AutoXDRTree::noSubTree);

    // Stop at the top-level.
    if (!parent) {
        node_ = nullptr;
        return;
    }

    // Restore the parent node.
    SlicesTree::Ptr p = tree_.lookup(parent->key_);
    node_ = &p->value();

    // Append the new slice in the parent node.
    if (!node_->append(Slice { cursor, 0, AutoXDRTree::noSubTree })) {
        oom_ = true;
        return;
    }
}

bool
XDRIncrementalEncoder::linearize(JS::TranscodeBuffer& buffer)
{
    if (oom_) {
        ReportOutOfMemory(cx());
        return fail(JS::TranscodeResult_Throw);
    }

    // Do not linearize while we are currently adding bytes.
    MOZ_ASSERT(scope_ == nullptr);

    // Visit the tree parts in a depth first order, to linearize the bits.
    Vector<SlicesNode::ConstRange> depthFirst(cx());

    SlicesTree::Ptr p = tree_.lookup(AutoXDRTree::topLevel);
    MOZ_ASSERT(p);

    if (!depthFirst.append(((const SlicesNode&) p->value()).all())) {
        ReportOutOfMemory(cx());
        return fail(JS::TranscodeResult_Throw);
    }

    while (!depthFirst.empty()) {
        SlicesNode::ConstRange& iter = depthFirst.back();
        Slice slice = iter.popCopyFront();
        // These fields have different meaning, but they should be correlated if
        // the tree is well formatted.
        MOZ_ASSERT_IF(slice.child == AutoXDRTree::noSubTree, iter.empty());
        if (iter.empty())
            depthFirst.popBack();

        // Copy the bytes associated with the current slice to the transcode
        // buffer which would be serialized.
        MOZ_ASSERT(slice.sliceBegin <= slices_.length());
        MOZ_ASSERT(slice.sliceBegin + slice.sliceLength <= slices_.length());
        if (!buffer.append(slices_.begin() + slice.sliceBegin, slice.sliceLength)) {
            ReportOutOfMemory(cx());
            return fail(JS::TranscodeResult_Throw);
        }

        // If we are at the end, go to back to the parent script.
        if (slice.child == AutoXDRTree::noSubTree)
            continue;

        // Visit the sub-parts before visiting the rest of the current slice.
        SlicesTree::Ptr p = tree_.lookup(slice.child);
        MOZ_ASSERT(p);
        if (!depthFirst.append(((const SlicesNode&) p->value()).all())) {
            ReportOutOfMemory(cx());
            return fail(JS::TranscodeResult_Throw);
        }
    }

    tree_.finish();
    slices_.clearAndFree();
    return true;
}
