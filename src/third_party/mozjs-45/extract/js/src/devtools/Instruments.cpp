/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Instruments.h"

#ifdef __APPLE__

#include <dlfcn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

// There are now 2 paths to the DTPerformanceSession framework. We try to load
// the one contained in /Applications/Xcode.app first, falling back to the one
// contained in /Library/Developer/4.0/Instruments.
#define DTPerformanceLibraryPath "/Applications/Xcode.app/Contents/Developer/Library/Frameworks/DTPerformanceSession.framework/Versions/Current/DTPerformanceSession"
#define OldDTPerformanceLibraryPath "/Library/Developer/4.0/Instruments/Frameworks/DTPerformanceSession.framework/Versions/Current/DTPerformanceSession"

extern "C" {

typedef CFTypeRef DTPerformanceSessionRef;

#define DTPerformanceSession_TimeProfiler               "com.apple.instruments.dtps.timeprofiler"
// DTPerformanceSession_Option_SamplingInterval is measured in microseconds
#define DTPerformanceSession_Option_SamplingInterval    "com.apple.instruments.dtps.option.samplinginterval"

typedef void (*dtps_errorcallback_t)(CFStringRef, CFErrorRef);
typedef DTPerformanceSessionRef (*DTPerformanceSessionCreateFunction)(CFStringRef, CFStringRef, CFDictionaryRef, CFErrorRef*);
typedef bool (*DTPerformanceSessionAddInstrumentFunction)(DTPerformanceSessionRef, CFStringRef, CFDictionaryRef, dtps_errorcallback_t, CFErrorRef*);
typedef bool (*DTPerformanceSessionIsRecordingFunction)(DTPerformanceSessionRef);
typedef bool (*DTPerformanceSessionStartFunction)(DTPerformanceSessionRef, CFArrayRef, CFErrorRef*);
typedef bool (*DTPerformanceSessionStopFunction)(DTPerformanceSessionRef, CFArrayRef, CFErrorRef*);
typedef bool (*DTPerformanceSessionSaveFunction)(DTPerformanceSessionRef, CFStringRef, CFErrorRef*);

} // extern "C"

namespace Instruments {

static const int kSamplingInterval = 20; // microseconds

template<typename T>
class AutoReleased
{
public:
  AutoReleased(T aTypeRef) : mTypeRef(aTypeRef)
  {
  }
  ~AutoReleased()
  {
    if (mTypeRef) {
      CFRelease(mTypeRef);
    }
  }

  operator T()
  {
    return mTypeRef;
  }

private:
  T mTypeRef;
};

#define DTPERFORMANCE_SYMBOLS \
  SYMBOL(DTPerformanceSessionCreate) \
  SYMBOL(DTPerformanceSessionAddInstrument) \
  SYMBOL(DTPerformanceSessionIsRecording) \
  SYMBOL(DTPerformanceSessionStart) \
  SYMBOL(DTPerformanceSessionStop) \
  SYMBOL(DTPerformanceSessionSave)

#define SYMBOL(_sym) \
  _sym##Function _sym = nullptr;

DTPERFORMANCE_SYMBOLS

#undef SYMBOL

void*
LoadDTPerformanceLibraries(bool dontLoad)
{
  int flags = RTLD_LAZY | RTLD_LOCAL | RTLD_NODELETE;
  if (dontLoad) {
    flags |= RTLD_NOLOAD;
  }

  void* DTPerformanceLibrary = dlopen(DTPerformanceLibraryPath, flags);
  if (!DTPerformanceLibrary) {
    DTPerformanceLibrary = dlopen(OldDTPerformanceLibraryPath, flags);
  }
  return DTPerformanceLibrary;
}

bool
LoadDTPerformanceLibrary()
{
  void* DTPerformanceLibrary = LoadDTPerformanceLibraries(true);
  if (!DTPerformanceLibrary) {
    DTPerformanceLibrary = LoadDTPerformanceLibraries(false);
    if (!DTPerformanceLibrary) {
      return false;
    }
  }

#define SYMBOL(_sym) \
  _sym = reinterpret_cast<_sym##Function>(dlsym(DTPerformanceLibrary, #_sym)); \
  if (!_sym) { \
    dlclose(DTPerformanceLibrary); \
    DTPerformanceLibrary = nullptr; \
    return false; \
  }

  DTPERFORMANCE_SYMBOLS

#undef SYMBOL

  dlclose(DTPerformanceLibrary);

  return true;
}

static DTPerformanceSessionRef gSession;

bool
Error(CFErrorRef error)
{
  if (gSession) {
    CFErrorRef unused = nullptr;
    DTPerformanceSessionStop(gSession, nullptr, &unused);
    CFRelease(gSession);
    gSession = nullptr;
  }
#ifdef DEBUG
  AutoReleased<CFDataRef> data =
    CFStringCreateExternalRepresentation(nullptr,
                                         CFErrorCopyDescription(error),
                                         kCFStringEncodingUTF8, '?');
  if (data != nullptr) {
    printf("%.*s\n\n", (int)CFDataGetLength(data), CFDataGetBytePtr(data));
  }
#endif
  return false;
}

bool
Start(pid_t pid)
{
  if (gSession) {
    return false;
  }

  if (!LoadDTPerformanceLibrary()) {
    return false;
  }

  AutoReleased<CFStringRef> process =
    CFStringCreateWithFormat(kCFAllocatorDefault, nullptr, CFSTR("%d"), pid);
  if (!process) {
    return false;
  }
  CFErrorRef error = nullptr;
  gSession = DTPerformanceSessionCreate(nullptr, process, nullptr, &error);
  if (!gSession) {
    return Error(error);
  }

  AutoReleased<CFNumberRef> interval =
    CFNumberCreate(0, kCFNumberIntType, &kSamplingInterval);
  if (!interval) {
    return false;
  }
  CFStringRef keys[1] = { CFSTR(DTPerformanceSession_Option_SamplingInterval) };
  CFNumberRef values[1] = { interval };
  AutoReleased<CFDictionaryRef> options =
    CFDictionaryCreate(kCFAllocatorDefault, (const void**)keys,
                       (const void**)values, 1, &kCFTypeDictionaryKeyCallBacks,
                       &kCFTypeDictionaryValueCallBacks);
  if (!options) {
    return false;
  }

  if (!DTPerformanceSessionAddInstrument(gSession,
                                         CFSTR(DTPerformanceSession_TimeProfiler),
                                         options, nullptr, &error)) {
    return Error(error);
  }

  return Resume();
}

void
Pause()
{
  if (gSession && DTPerformanceSessionIsRecording(gSession)) {
    CFErrorRef error = nullptr;
    if (!DTPerformanceSessionStop(gSession, nullptr, &error)) {
      Error(error);
    }
  }
}

bool
Resume()
{
  if (!gSession) {
    return false;
  }

  CFErrorRef error = nullptr;
  return DTPerformanceSessionStart(gSession, nullptr, &error) ||
         Error(error);
}

void
Stop(const char* profileName)
{
  Pause();

  CFErrorRef error = nullptr;
  AutoReleased<CFStringRef> name =
    CFStringCreateWithFormat(kCFAllocatorDefault, nullptr, CFSTR("%s%s"),
                             "/tmp/", profileName ? profileName : "mozilla");
  if (!DTPerformanceSessionSave(gSession, name, &error)) {
    Error(error);
    return;
  }

  CFRelease(gSession);
  gSession = nullptr;
}

} // namespace Instruments

#endif /* __APPLE__ */
