/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef builtin_Stream_h
#define builtin_Stream_h

#include "builtin/Promise.h"
#include "vm/NativeObject.h"


namespace js {

class AutoSetNewObjectMetadata;

class ReadableStream : public NativeObject
{
  public:
    static ReadableStream* createDefaultStream(JSContext* cx, HandleValue underlyingSource,
                                               HandleValue size, HandleValue highWaterMark,
                                               HandleObject proto = nullptr);
    static ReadableStream* createByteStream(JSContext* cx, HandleValue underlyingSource,
                                            HandleValue highWaterMark,
                                            HandleObject proto = nullptr);
    static ReadableStream* createExternalSourceStream(JSContext* cx, void* underlyingSource,
                                                      uint8_t flags, HandleObject proto = nullptr);

    bool readable() const;
    bool closed() const;
    bool errored() const;
    bool disturbed() const;

    bool locked() const;

    void desiredSize(bool* hasSize, double* size) const;

    JS::ReadableStreamMode mode() const;

    static MOZ_MUST_USE bool close(JSContext* cx, Handle<ReadableStream*> stream);
    static MOZ_MUST_USE JSObject* cancel(JSContext* cx, Handle<ReadableStream*> stream,
                                         HandleValue reason);
    static MOZ_MUST_USE bool error(JSContext* cx, Handle<ReadableStream*> stream,
                                   HandleValue error);

    static MOZ_MUST_USE NativeObject* getReader(JSContext* cx, Handle<ReadableStream*> stream,
                                                JS::ReadableStreamReaderMode mode);

    static MOZ_MUST_USE bool tee(JSContext* cx,
                                 Handle<ReadableStream*> stream, bool cloneForBranch2,
                                 MutableHandle<ReadableStream*> branch1Stream,
                                 MutableHandle<ReadableStream*> branch2Stream);

    static MOZ_MUST_USE bool enqueue(JSContext* cx, Handle<ReadableStream*> stream,
                                     HandleValue chunk);
    static MOZ_MUST_USE bool enqueueBuffer(JSContext* cx, Handle<ReadableStream*> stream,
                                           Handle<ArrayBufferObject*> chunk);
    static MOZ_MUST_USE bool getExternalSource(JSContext* cx, Handle<ReadableStream*> stream,
                                               void** source);
    void releaseExternalSource();
    uint8_t embeddingFlags() const;
    static MOZ_MUST_USE bool updateDataAvailableFromSource(JSContext* cx,
                                                           Handle<ReadableStream*> stream,
                                                           uint32_t availableData);

    enum State {
         Readable  = 1 << 0,
         Closed    = 1 << 1,
         Errored   = 1 << 2,
         Disturbed = 1 << 3
    };

  private:
    static MOZ_MUST_USE ReadableStream* createStream(JSContext* cx, HandleObject proto = nullptr);

  public:
    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

class ReadableStreamDefaultReader : public NativeObject
{
  public:
    static MOZ_MUST_USE JSObject* read(JSContext* cx, Handle<ReadableStreamDefaultReader*> reader);

    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

class ReadableStreamBYOBReader : public NativeObject
{
  public:
    static MOZ_MUST_USE JSObject* read(JSContext* cx, Handle<ReadableStreamBYOBReader*> reader,
                                       Handle<ArrayBufferViewObject*> view);

    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

bool ReadableStreamReaderIsClosed(const JSObject* reader);

MOZ_MUST_USE bool ReadableStreamReaderCancel(JSContext* cx, HandleObject reader,
                                             HandleValue reason);

MOZ_MUST_USE bool ReadableStreamReaderReleaseLock(JSContext* cx, HandleObject reader);

class ReadableStreamDefaultController : public NativeObject
{
  public:
    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

class ReadableByteStreamController : public NativeObject
{
  public:
    bool hasExternalSource();

    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

class ReadableStreamBYOBRequest : public NativeObject
{
  public:
    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

class ByteLengthQueuingStrategy : public NativeObject
{
  public:
    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

class CountQueuingStrategy : public NativeObject
{
  public:
    static bool constructor(JSContext* cx, unsigned argc, Value* vp);
    static const ClassSpec classSpec_;
    static const Class class_;
    static const ClassSpec protoClassSpec_;
    static const Class protoClass_;
};

} // namespace js

#endif /* builtin_Stream_h */
