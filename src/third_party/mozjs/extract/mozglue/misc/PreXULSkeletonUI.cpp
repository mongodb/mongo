/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PreXULSkeletonUI.h"

#include <algorithm>
#include <math.h>
#include <limits.h>
#include <cmath>
#include <locale>
#include <string>
#include <objbase.h>
#include <shlobj.h>

#include "mozilla/Assertions.h"
#include "mozilla/Attributes.h"
#include "mozilla/BaseProfilerMarkers.h"
#include "mozilla/CacheNtDllThunk.h"
#include "mozilla/FStream.h"
#include "mozilla/GetKnownFolderPath.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/HelperMacros.h"
#include "mozilla/glue/Debug.h"
#include "mozilla/Maybe.h"
#include "mozilla/mscom/ProcessRuntime.h"
#include "mozilla/ResultVariant.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/Try.h"
#include "mozilla/UniquePtr.h"
#include "mozilla/UniquePtrExtensions.h"
#include "mozilla/Unused.h"
#include "mozilla/WindowsDpiAwareness.h"
#include "mozilla/WindowsProcessMitigations.h"

namespace mozilla {

// ColorRect defines an optionally-rounded, optionally-bordered rectangle of a
// particular color that we will draw.
struct ColorRect {
  uint32_t color;
  uint32_t borderColor;
  int x;
  int y;
  int width;
  int height;
  int borderWidth;
  int borderRadius;
  bool flipIfRTL;
};

// DrawRect is mostly the same as ColorRect, but exists as an implementation
// detail to simplify drawing borders. We draw borders as a strokeOnly rect
// underneath an inner rect of a particular color. We also need to keep
// track of the backgroundColor for rounding rects, in order to correctly
// anti-alias.
struct DrawRect {
  uint32_t color;
  uint32_t backgroundColor;
  int x;
  int y;
  int width;
  int height;
  int borderRadius;
  int borderWidth;
  bool strokeOnly;
};

struct NormalizedRGB {
  double r;
  double g;
  double b;
};

NormalizedRGB UintToRGB(uint32_t color) {
  double r = static_cast<double>(color >> 16 & 0xff) / 255.0;
  double g = static_cast<double>(color >> 8 & 0xff) / 255.0;
  double b = static_cast<double>(color >> 0 & 0xff) / 255.0;
  return NormalizedRGB{r, g, b};
}

uint32_t RGBToUint(const NormalizedRGB& rgb) {
  return (static_cast<uint32_t>(rgb.r * 255.0) << 16) |
         (static_cast<uint32_t>(rgb.g * 255.0) << 8) |
         (static_cast<uint32_t>(rgb.b * 255.0) << 0);
}

double Lerp(double a, double b, double x) { return a + x * (b - a); }

NormalizedRGB Lerp(const NormalizedRGB& a, const NormalizedRGB& b, double x) {
  return NormalizedRGB{Lerp(a.r, b.r, x), Lerp(a.g, b.g, x), Lerp(a.b, b.b, x)};
}

// Produces a smooth curve in [0,1] based on a linear input in [0,1]
double SmoothStep3(double x) { return x * x * (3.0 - 2.0 * x); }

static const wchar_t kPreXULSkeletonUIKeyPath[] =
    L"SOFTWARE"
    L"\\" MOZ_APP_VENDOR L"\\" MOZ_APP_BASENAME L"\\PreXULSkeletonUISettings";

static bool sPreXULSkeletonUIShown = false;
static bool sPreXULSkeletonUIEnabled = false;
static HWND sPreXULSkeletonUIWindow;
static LPWSTR const gStockApplicationIcon = MAKEINTRESOURCEW(32512);
static LPWSTR const gIDCWait = MAKEINTRESOURCEW(32514);
static HANDLE sPreXULSKeletonUIAnimationThread;
static HANDLE sPreXULSKeletonUILockFile = INVALID_HANDLE_VALUE;

static mozilla::mscom::ProcessRuntime* sProcessRuntime;
static uint32_t* sPixelBuffer = nullptr;
static Vector<ColorRect>* sAnimatedRects = nullptr;
static int sTotalChromeHeight = 0;
static volatile LONG sAnimationControlFlag = 0;
static bool sMaximized = false;
static int sNonClientVerticalMargins = 0;
static int sNonClientHorizontalMargins = 0;
static uint32_t sDpi = 0;

// Color values needed by the animation loop
static uint32_t sAnimationColor;
static uint32_t sToolbarForegroundColor;

static ThemeMode sTheme = ThemeMode::Invalid;

typedef BOOL(WINAPI* EnableNonClientDpiScalingProc)(HWND);
static EnableNonClientDpiScalingProc sEnableNonClientDpiScaling = NULL;
typedef int(WINAPI* GetSystemMetricsForDpiProc)(int, UINT);
GetSystemMetricsForDpiProc sGetSystemMetricsForDpi = NULL;
typedef UINT(WINAPI* GetDpiForWindowProc)(HWND);
GetDpiForWindowProc sGetDpiForWindow = NULL;
typedef ATOM(WINAPI* RegisterClassWProc)(const WNDCLASSW*);
RegisterClassWProc sRegisterClassW = NULL;
typedef HICON(WINAPI* LoadIconWProc)(HINSTANCE, LPCWSTR);
LoadIconWProc sLoadIconW = NULL;
typedef HICON(WINAPI* LoadCursorWProc)(HINSTANCE, LPCWSTR);
LoadCursorWProc sLoadCursorW = NULL;
typedef HWND(WINAPI* CreateWindowExWProc)(DWORD, LPCWSTR, LPCWSTR, DWORD, int,
                                          int, int, int, HWND, HMENU, HINSTANCE,
                                          LPVOID);
CreateWindowExWProc sCreateWindowExW = NULL;
typedef BOOL(WINAPI* ShowWindowProc)(HWND, int);
ShowWindowProc sShowWindow = NULL;
typedef BOOL(WINAPI* SetWindowPosProc)(HWND, HWND, int, int, int, int, UINT);
SetWindowPosProc sSetWindowPos = NULL;
typedef HDC(WINAPI* GetWindowDCProc)(HWND);
GetWindowDCProc sGetWindowDC = NULL;
typedef int(WINAPI* FillRectProc)(HDC, const RECT*, HBRUSH);
FillRectProc sFillRect = NULL;
typedef BOOL(WINAPI* DeleteObjectProc)(HGDIOBJ);
DeleteObjectProc sDeleteObject = NULL;
typedef int(WINAPI* ReleaseDCProc)(HWND, HDC);
ReleaseDCProc sReleaseDC = NULL;
typedef HMONITOR(WINAPI* MonitorFromWindowProc)(HWND, DWORD);
MonitorFromWindowProc sMonitorFromWindow = NULL;
typedef BOOL(WINAPI* GetMonitorInfoWProc)(HMONITOR, LPMONITORINFO);
GetMonitorInfoWProc sGetMonitorInfoW = NULL;
typedef LONG_PTR(WINAPI* SetWindowLongPtrWProc)(HWND, int, LONG_PTR);
SetWindowLongPtrWProc sSetWindowLongPtrW = NULL;
typedef int(WINAPI* StretchDIBitsProc)(HDC, int, int, int, int, int, int, int,
                                       int, const VOID*, const BITMAPINFO*,
                                       UINT, DWORD);
StretchDIBitsProc sStretchDIBits = NULL;
typedef HBRUSH(WINAPI* CreateSolidBrushProc)(COLORREF);
CreateSolidBrushProc sCreateSolidBrush = NULL;

static int sWindowWidth;
static int sWindowHeight;
static double sCSSToDevPixelScaling;

static Maybe<PreXULSkeletonUIError> sErrorReason;

static const int kAnimationCSSPixelsPerFrame = 11;
static const int kAnimationCSSExtraWindowSize = 300;

// NOTE: these values were pulled out of thin air as round numbers that are
// likely to be too big to be seen in practice. If we legitimately see windows
// this big, we probably don't want to be drawing them on the CPU anyway.
static const uint32_t kMaxWindowWidth = 1 << 16;
static const uint32_t kMaxWindowHeight = 1 << 16;

static const wchar_t* sEnabledRegSuffix = L"|Enabled";
static const wchar_t* sScreenXRegSuffix = L"|ScreenX";
static const wchar_t* sScreenYRegSuffix = L"|ScreenY";
static const wchar_t* sWidthRegSuffix = L"|Width";
static const wchar_t* sHeightRegSuffix = L"|Height";
static const wchar_t* sMaximizedRegSuffix = L"|Maximized";
static const wchar_t* sUrlbarCSSRegSuffix = L"|UrlbarCSSSpan";
static const wchar_t* sCssToDevPixelScalingRegSuffix = L"|CssToDevPixelScaling";
static const wchar_t* sSearchbarRegSuffix = L"|SearchbarCSSSpan";
static const wchar_t* sSpringsCSSRegSuffix = L"|SpringsCSSSpan";
static const wchar_t* sThemeRegSuffix = L"|Theme";
static const wchar_t* sFlagsRegSuffix = L"|Flags";
static const wchar_t* sProgressSuffix = L"|Progress";

std::wstring GetRegValueName(const wchar_t* prefix, const wchar_t* suffix) {
  std::wstring result(prefix);
  result.append(suffix);
  return result;
}

// This is paraphrased from WinHeaderOnlyUtils.h. The fact that this file is
// included in standalone SpiderMonkey builds prohibits us from including that
// file directly, and it hardly warrants its own header. Bug 1674920 tracks
// only including this file for gecko-related builds.
Result<UniquePtr<wchar_t[]>, PreXULSkeletonUIError> GetBinaryPath() {
  DWORD bufLen = MAX_PATH;
  UniquePtr<wchar_t[]> buf;
  while (true) {
    buf = MakeUnique<wchar_t[]>(bufLen);
    DWORD retLen = ::GetModuleFileNameW(nullptr, buf.get(), bufLen);
    if (!retLen) {
      return Err(PreXULSkeletonUIError::FilesystemFailure);
    }

    if (retLen == bufLen && ::GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
      bufLen *= 2;
      continue;
    }

    break;
  }

  return buf;
}

// PreXULSkeletonUIDisallowed means that we don't even have the capacity to
// enable the skeleton UI, whether because we're on a platform that doesn't
// support it or because we launched with command line arguments that we don't
// support. Some of these situations are transient, so we want to make sure we
// don't mess with registry values in these scenarios that we may use in
// other scenarios in which the skeleton UI is actually enabled.
static bool PreXULSkeletonUIDisallowed() {
  return sErrorReason.isSome() &&
         (*sErrorReason == PreXULSkeletonUIError::Cmdline ||
          *sErrorReason == PreXULSkeletonUIError::EnvVars);
}

// Note: this is specifically *not* a robust, multi-locale lowercasing
// operation. It is not intended to be such. It is simply intended to match the
// way in which we look for other instances of firefox to remote into.
// See
// https://searchfox.org/mozilla-central/rev/71621bfa47a371f2b1ccfd33c704913124afb933/toolkit/components/remote/nsRemoteService.cpp#56
static void MutateStringToLowercase(wchar_t* ptr) {
  while (*ptr) {
    wchar_t ch = *ptr;
    if (ch >= L'A' && ch <= L'Z') {
      *ptr = ch + (L'a' - L'A');
    }
    ++ptr;
  }
}

static Result<Ok, PreXULSkeletonUIError> GetSkeletonUILock() {
  auto localAppDataPath = GetKnownFolderPath(FOLDERID_LocalAppData);
  if (!localAppDataPath) {
    return Err(PreXULSkeletonUIError::FilesystemFailure);
  }

  if (sPreXULSKeletonUILockFile != INVALID_HANDLE_VALUE) {
    return Ok();
  }

  // Note: because we're in mozglue, we cannot easily access things from
  // toolkit, like `GetInstallHash`. We could move `GetInstallHash` into
  // mozglue, and rip out all of its usage of types defined in toolkit headers.
  // However, it seems cleaner to just hash the bin path ourselves. We don't
  // get quite the same robustness that `GetInstallHash` might provide, but
  // we already don't have that with how we key our registry values, so it
  // probably makes sense to just match those.
  UniquePtr<wchar_t[]> binPath;
  MOZ_TRY_VAR(binPath, GetBinaryPath());

  // Lowercase the binpath to match how we look for remote instances.
  MutateStringToLowercase(binPath.get());

  // The number of bytes * 2 characters per byte + 1 for the null terminator
  uint32_t hexHashSize = sizeof(uint32_t) * 2 + 1;
  UniquePtr<wchar_t[]> installHash = MakeUnique<wchar_t[]>(hexHashSize);
  // This isn't perfect - it's a 32-bit hash of the path to our executable. It
  // could reasonably collide, or casing could potentially affect things, but
  // the theory is that that should be uncommon enough and the failure case
  // mild enough that this is fine.
  uint32_t binPathHash = HashString(binPath.get());
  swprintf(installHash.get(), hexHashSize, L"%08x", binPathHash);

  std::wstring lockFilePath;
  lockFilePath.append(localAppDataPath.get());
  lockFilePath.append(
      L"\\" MOZ_APP_VENDOR L"\\" MOZ_APP_BASENAME L"\\SkeletonUILock-");
  lockFilePath.append(installHash.get());

  // We intentionally leak this file - that is okay, and (kind of) the point.
  // We want to hold onto this handle until the application exits, and hold
  // onto it with exclusive rights. If this check fails, then we assume that
  // another instance of the executable is holding it, and thus return false.
  sPreXULSKeletonUILockFile =
      ::CreateFileW(lockFilePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                    0,  // No sharing - this is how the lock works
                    nullptr, CREATE_ALWAYS,
                    FILE_FLAG_DELETE_ON_CLOSE,  // Don't leave this lying around
                    nullptr);
  if (sPreXULSKeletonUILockFile == INVALID_HANDLE_VALUE) {
    return Err(PreXULSkeletonUIError::FailedGettingLock);
  }

  return Ok();
}

const char kGeneralSection[] = "[General]";
const char kStartWithLastProfile[] = "StartWithLastProfile=";

static bool ProfileDbHasStartWithLastProfile(IFStream& iniContents) {
  bool inGeneral = false;
  std::string line;
  while (std::getline(iniContents, line)) {
    size_t whitespace = 0;
    while (line.length() > whitespace &&
           (line[whitespace] == ' ' || line[whitespace] == '\t')) {
      whitespace++;
    }
    line.erase(0, whitespace);

    if (line.compare(kGeneralSection) == 0) {
      inGeneral = true;
    } else if (inGeneral) {
      if (line[0] == '[') {
        inGeneral = false;
      } else {
        if (line.find(kStartWithLastProfile) == 0) {
          char val = line.c_str()[sizeof(kStartWithLastProfile) - 1];
          if (val == '0') {
            return false;
          } else if (val == '1') {
            return true;
          }
        }
      }
    }
  }

  // If we don't find it in the .ini file, we interpret that as true
  return true;
}

static Result<Ok, PreXULSkeletonUIError> CheckForStartWithLastProfile() {
  auto roamingAppData = GetKnownFolderPath(FOLDERID_RoamingAppData);
  if (!roamingAppData) {
    return Err(PreXULSkeletonUIError::FilesystemFailure);
  }
  std::wstring profileDbPath(roamingAppData.get());
  profileDbPath.append(
      L"\\" MOZ_APP_VENDOR L"\\" MOZ_APP_BASENAME L"\\profiles.ini");
  IFStream profileDb(profileDbPath.c_str());
  if (profileDb.fail()) {
    return Err(PreXULSkeletonUIError::FilesystemFailure);
  }

  if (!ProfileDbHasStartWithLastProfile(profileDb)) {
    return Err(PreXULSkeletonUIError::NoStartWithLastProfile);
  }

  return Ok();
}

// We could use nsAutoRegKey, but including nsWindowsHelpers.h causes build
// failures in random places because we're in mozglue. Overall it should be
// simpler and cleaner to just step around that issue with this class:
class MOZ_RAII AutoCloseRegKey {
 public:
  explicit AutoCloseRegKey(HKEY key) : mKey(key) {}
  ~AutoCloseRegKey() { ::RegCloseKey(mKey); }

 private:
  HKEY mKey;
};

int CSSToDevPixels(double cssPixels, double scaling) {
  return floor(cssPixels * scaling + 0.5);
}

int CSSToDevPixels(int cssPixels, double scaling) {
  return CSSToDevPixels((double)cssPixels, scaling);
}

int CSSToDevPixelsFloor(double cssPixels, double scaling) {
  return floor(cssPixels * scaling);
}

// Some things appear to floor to device pixels rather than rounding. A good
// example of this is border widths.
int CSSToDevPixelsFloor(int cssPixels, double scaling) {
  return CSSToDevPixelsFloor((double)cssPixels, scaling);
}

double SignedDistanceToCircle(double x, double y, double radius) {
  return sqrt(x * x + y * y) - radius;
}

// For more details, see
// https://searchfox.org/mozilla-central/rev/a5d9abfda1e26b1207db9549549ab0bdd73f735d/gfx/wr/webrender/res/shared.glsl#141-187
// which was a reference for this function.
double DistanceAntiAlias(double signedDistance) {
  // Distance assumed to be in device pixels. We use an aa range of 0.5 for
  // reasons detailed in the linked code above.
  const double aaRange = 0.5;
  double dist = 0.5 * signedDistance / aaRange;
  if (dist <= -0.5 + std::numeric_limits<double>::epsilon()) return 1.0;
  if (dist >= 0.5 - std::numeric_limits<double>::epsilon()) return 0.0;
  return 0.5 + dist * (0.8431027 * dist * dist - 1.14453603);
}

void RasterizeRoundedRectTopAndBottom(const DrawRect& rect) {
  if (rect.height <= 2 * rect.borderRadius) {
    MOZ_ASSERT(false, "Skeleton UI rect height too small for border radius.");
    return;
  }
  if (rect.width <= 2 * rect.borderRadius) {
    MOZ_ASSERT(false, "Skeleton UI rect width too small for border radius.");
    return;
  }

  NormalizedRGB rgbBase = UintToRGB(rect.backgroundColor);
  NormalizedRGB rgbBlend = UintToRGB(rect.color);

  for (int rowIndex = 0; rowIndex < rect.borderRadius; ++rowIndex) {
    int yTop = rect.y + rect.borderRadius - 1 - rowIndex;
    int yBottom = rect.y + rect.height - rect.borderRadius + rowIndex;

    uint32_t* lineStartTop = &sPixelBuffer[yTop * sWindowWidth];
    uint32_t* innermostPixelTopLeft =
        lineStartTop + rect.x + rect.borderRadius - 1;
    uint32_t* innermostPixelTopRight =
        lineStartTop + rect.x + rect.width - rect.borderRadius;
    uint32_t* lineStartBottom = &sPixelBuffer[yBottom * sWindowWidth];
    uint32_t* innermostPixelBottomLeft =
        lineStartBottom + rect.x + rect.borderRadius - 1;
    uint32_t* innermostPixelBottomRight =
        lineStartBottom + rect.x + rect.width - rect.borderRadius;

    // Add 0.5 to x and y to get the pixel center.
    double pixelY = (double)rowIndex + 0.5;
    for (int columnIndex = 0; columnIndex < rect.borderRadius; ++columnIndex) {
      double pixelX = (double)columnIndex + 0.5;
      double distance =
          SignedDistanceToCircle(pixelX, pixelY, (double)rect.borderRadius);
      double alpha = DistanceAntiAlias(distance);
      NormalizedRGB rgb = Lerp(rgbBase, rgbBlend, alpha);
      uint32_t color = RGBToUint(rgb);

      innermostPixelTopLeft[-columnIndex] = color;
      innermostPixelTopRight[columnIndex] = color;
      innermostPixelBottomLeft[-columnIndex] = color;
      innermostPixelBottomRight[columnIndex] = color;
    }

    std::fill(innermostPixelTopLeft + 1, innermostPixelTopRight, rect.color);
    std::fill(innermostPixelBottomLeft + 1, innermostPixelBottomRight,
              rect.color);
  }
}

void RasterizeAnimatedRoundedRectTopAndBottom(
    const ColorRect& colorRect, const uint32_t* animationLookup,
    int priorUpdateAreaMin, int priorUpdateAreaMax, int currentUpdateAreaMin,
    int currentUpdateAreaMax, int animationMin) {
  // We iterate through logical pixel rows here, from inside to outside, which
  // for the top of the rounded rect means from bottom to top, and for the
  // bottom of the rect means top to bottom. We paint pixels from left to
  // right on the top and bottom rows at the same time for the entire animation
  // window. (If the animation window does not overlap any rounded corners,
  // however, we won't be called at all)
  for (int rowIndex = 0; rowIndex < colorRect.borderRadius; ++rowIndex) {
    int yTop = colorRect.y + colorRect.borderRadius - 1 - rowIndex;
    int yBottom =
        colorRect.y + colorRect.height - colorRect.borderRadius + rowIndex;

    uint32_t* lineStartTop = &sPixelBuffer[yTop * sWindowWidth];
    uint32_t* lineStartBottom = &sPixelBuffer[yBottom * sWindowWidth];

    // Add 0.5 to x and y to get the pixel center.
    double pixelY = (double)rowIndex + 0.5;
    for (int x = priorUpdateAreaMin; x < currentUpdateAreaMax; ++x) {
      // The column index is the distance from the innermost pixel, which
      // is different depending on whether we're on the left or right
      // side of the rect. It will always be the max here, and if it's
      // negative that just means we're outside the rounded area.
      int columnIndex =
          std::max((int)colorRect.x + (int)colorRect.borderRadius - x - 1,
                   x - ((int)colorRect.x + (int)colorRect.width -
                        (int)colorRect.borderRadius));

      double alpha = 1.0;
      if (columnIndex >= 0) {
        double pixelX = (double)columnIndex + 0.5;
        double distance = SignedDistanceToCircle(
            pixelX, pixelY, (double)colorRect.borderRadius);
        alpha = DistanceAntiAlias(distance);
      }
      // We don't do alpha blending for the antialiased pixels at the
      // shape's border. It is not noticeable in the animation.
      if (alpha > 1.0 - std::numeric_limits<double>::epsilon()) {
        // Overwrite the tail end of last frame's animation with the
        // rect's normal, unanimated color.
        uint32_t color = x < priorUpdateAreaMax
                             ? colorRect.color
                             : animationLookup[x - animationMin];
        lineStartTop[x] = color;
        lineStartBottom[x] = color;
      }
    }
  }
}

void RasterizeColorRect(const ColorRect& colorRect) {
  // We sometimes split our rect into two, to simplify drawing borders. If we
  // have a border, we draw a stroke-only rect first, and then draw the smaller
  // inner rect on top of it.
  Vector<DrawRect, 2> drawRects;
  Unused << drawRects.reserve(2);
  if (colorRect.borderWidth == 0) {
    DrawRect rect = {};
    rect.color = colorRect.color;
    rect.backgroundColor =
        sPixelBuffer[colorRect.y * sWindowWidth + colorRect.x];
    rect.x = colorRect.x;
    rect.y = colorRect.y;
    rect.width = colorRect.width;
    rect.height = colorRect.height;
    rect.borderRadius = colorRect.borderRadius;
    rect.strokeOnly = false;
    drawRects.infallibleAppend(rect);
  } else {
    DrawRect borderRect = {};
    borderRect.color = colorRect.borderColor;
    borderRect.backgroundColor =
        sPixelBuffer[colorRect.y * sWindowWidth + colorRect.x];
    borderRect.x = colorRect.x;
    borderRect.y = colorRect.y;
    borderRect.width = colorRect.width;
    borderRect.height = colorRect.height;
    borderRect.borderRadius = colorRect.borderRadius;
    borderRect.borderWidth = colorRect.borderWidth;
    borderRect.strokeOnly = true;
    drawRects.infallibleAppend(borderRect);

    DrawRect baseRect = {};
    baseRect.color = colorRect.color;
    baseRect.backgroundColor = borderRect.color;
    baseRect.x = colorRect.x + colorRect.borderWidth;
    baseRect.y = colorRect.y + colorRect.borderWidth;
    baseRect.width = colorRect.width - 2 * colorRect.borderWidth;
    baseRect.height = colorRect.height - 2 * colorRect.borderWidth;
    baseRect.borderRadius =
        std::max(0, (int)colorRect.borderRadius - (int)colorRect.borderWidth);
    baseRect.borderWidth = 0;
    baseRect.strokeOnly = false;
    drawRects.infallibleAppend(baseRect);
  }

  for (const DrawRect& rect : drawRects) {
    if (rect.height <= 0 || rect.width <= 0) {
      continue;
    }

    // For rounded rectangles, the first thing we do is draw the top and
    // bottom of the rectangle, with the more complicated logic below. After
    // that we can just draw the vertically centered part of the rect like
    // normal.
    RasterizeRoundedRectTopAndBottom(rect);

    // We then draw the flat, central portion of the rect (which in the case of
    // non-rounded rects, is just the entire thing.)
    int solidRectStartY =
        std::clamp(rect.y + rect.borderRadius, 0, sTotalChromeHeight);
    int solidRectEndY = std::clamp(rect.y + rect.height - rect.borderRadius, 0,
                                   sTotalChromeHeight);
    for (int y = solidRectStartY; y < solidRectEndY; ++y) {
      // For strokeOnly rects (used to draw borders), we just draw the left
      // and right side here. Looping down a column of pixels is not the most
      // cache-friendly thing, but it shouldn't be a big deal given the height
      // of the urlbar.
      // Also, if borderRadius is less than borderWidth, we need to ensure
      // that we fully draw the top and bottom lines, so we make sure to check
      // that we're inside the middle range range before excluding pixels.
      if (rect.strokeOnly && y - rect.y > rect.borderWidth &&
          rect.y + rect.height - y > rect.borderWidth) {
        int startXLeft = std::clamp(rect.x, 0, sWindowWidth);
        int endXLeft = std::clamp(rect.x + rect.borderWidth, 0, sWindowWidth);
        int startXRight =
            std::clamp(rect.x + rect.width - rect.borderWidth, 0, sWindowWidth);
        int endXRight = std::clamp(rect.x + rect.width, 0, sWindowWidth);

        uint32_t* lineStart = &sPixelBuffer[y * sWindowWidth];
        uint32_t* dataStartLeft = lineStart + startXLeft;
        uint32_t* dataEndLeft = lineStart + endXLeft;
        uint32_t* dataStartRight = lineStart + startXRight;
        uint32_t* dataEndRight = lineStart + endXRight;
        std::fill(dataStartLeft, dataEndLeft, rect.color);
        std::fill(dataStartRight, dataEndRight, rect.color);
      } else {
        int startX = std::clamp(rect.x, 0, sWindowWidth);
        int endX = std::clamp(rect.x + rect.width, 0, sWindowWidth);
        uint32_t* lineStart = &sPixelBuffer[y * sWindowWidth];
        uint32_t* dataStart = lineStart + startX;
        uint32_t* dataEnd = lineStart + endX;
        std::fill(dataStart, dataEnd, rect.color);
      }
    }
  }
}

// Paints the pixels to sPixelBuffer for the skeleton UI animation (a light
// gradient which moves from left to right across the grey placeholder rects).
// Takes in the rect to draw, together with a lookup table for the gradient,
// and the bounds of the previous and current frame of the animation.
bool RasterizeAnimatedRect(const ColorRect& colorRect,
                           const uint32_t* animationLookup,
                           int priorAnimationMin, int animationMin,
                           int animationMax) {
  int rectMin = colorRect.x;
  int rectMax = colorRect.x + colorRect.width;
  bool animationWindowOverlaps =
      rectMax >= priorAnimationMin && rectMin < animationMax;

  int priorUpdateAreaMin = std::max(rectMin, priorAnimationMin);
  int priorUpdateAreaMax = std::min(rectMax, animationMin);
  int currentUpdateAreaMin = std::max(rectMin, animationMin);
  int currentUpdateAreaMax = std::min(rectMax, animationMax);

  if (!animationWindowOverlaps) {
    return false;
  }

  bool animationWindowOverlapsBorderRadius =
      rectMin + colorRect.borderRadius > priorAnimationMin ||
      rectMax - colorRect.borderRadius <= animationMax;

  // If we don't overlap the left or right side of the rounded rectangle,
  // just pretend it's not rounded. This is a small optimization but
  // there's no point in doing all of this rounded rectangle checking if
  // we aren't even overlapping
  int borderRadius =
      animationWindowOverlapsBorderRadius ? colorRect.borderRadius : 0;

  if (borderRadius > 0) {
    // Similarly to how we draw the rounded rects in DrawSkeletonUI, we
    // first draw the rounded top and bottom, and then we draw the center
    // rect.
    RasterizeAnimatedRoundedRectTopAndBottom(
        colorRect, animationLookup, priorUpdateAreaMin, priorUpdateAreaMax,
        currentUpdateAreaMin, currentUpdateAreaMax, animationMin);
  }

  for (int y = colorRect.y + borderRadius;
       y < colorRect.y + colorRect.height - borderRadius; ++y) {
    uint32_t* lineStart = &sPixelBuffer[y * sWindowWidth];
    // Overwrite the tail end of last frame's animation with the rect's
    // normal, unanimated color.
    for (int x = priorUpdateAreaMin; x < priorUpdateAreaMax; ++x) {
      lineStart[x] = colorRect.color;
    }
    // Then apply the animated color
    for (int x = currentUpdateAreaMin; x < currentUpdateAreaMax; ++x) {
      lineStart[x] = animationLookup[x - animationMin];
    }
  }

  return true;
}

Result<Ok, PreXULSkeletonUIError> DrawSkeletonUI(
    HWND hWnd, CSSPixelSpan urlbarCSSSpan, CSSPixelSpan searchbarCSSSpan,
    Vector<CSSPixelSpan>& springs, const ThemeColors& currentTheme,
    const EnumSet<SkeletonUIFlag, uint32_t>& flags) {
  // NOTE: we opt here to paint a pixel buffer for the application chrome by
  // hand, without using native UI library methods. Why do we do this?
  //
  // 1) It gives us a little bit more control, especially if we want to animate
  //    any of this.
  // 2) It's actually more portable. We can do this on any platform where we
  //    can blit a pixel buffer to the screen, and it only has to change
  //    insofar as the UI is different on those platforms (and thus would have
  //    to change anyway.)
  //
  // The performance impact of this ought to be negligible. As far as has been
  // observed, on slow reference hardware this might take up to a millisecond,
  // for a startup which otherwise takes 30 seconds.
  //
  // The readability and maintainability are a greater concern. When the
  // silhouette of Firefox's core UI changes, this code will likely need to
  // change. However, for the foreseeable future, our skeleton UI will be mostly
  // axis-aligned geometric shapes, and the thought is that any code which is
  // manipulating raw pixels should not be *too* hard to maintain and
  // understand so long as it is only painting such simple shapes.

  sAnimationColor = currentTheme.animationColor;
  sToolbarForegroundColor = currentTheme.toolbarForegroundColor;

  bool menubarShown = flags.contains(SkeletonUIFlag::MenubarShown);
  bool bookmarksToolbarShown =
      flags.contains(SkeletonUIFlag::BookmarksToolbarShown);
  bool rtlEnabled = flags.contains(SkeletonUIFlag::RtlEnabled);

  int chromeHorMargin = CSSToDevPixels(2, sCSSToDevPixelScaling);
  int verticalOffset = sMaximized ? sNonClientVerticalMargins : 0;
  int horizontalOffset =
      sNonClientHorizontalMargins - (sMaximized ? 0 : chromeHorMargin);

  // found in tabs.inc.css, "--tab-min-height" + 2 * "--tab-block-margin"
  int tabBarHeight = CSSToDevPixels(44, sCSSToDevPixelScaling);
  int selectedTabBorderWidth = CSSToDevPixels(2, sCSSToDevPixelScaling);
  // found in tabs.inc.css, "--tab-block-margin"
  int titlebarSpacerWidth = horizontalOffset +
                            CSSToDevPixels(2, sCSSToDevPixelScaling) -
                            selectedTabBorderWidth;
  if (!sMaximized && !menubarShown) {
    // found in tabs.inc.css, ".titlebar-spacer"
    titlebarSpacerWidth += CSSToDevPixels(40, sCSSToDevPixelScaling);
  }
  // found in tabs.inc.css, "--tab-block-margin"
  int selectedTabMarginTop =
      CSSToDevPixels(4, sCSSToDevPixelScaling) - selectedTabBorderWidth;
  int selectedTabMarginBottom =
      CSSToDevPixels(4, sCSSToDevPixelScaling) - selectedTabBorderWidth;
  int selectedTabBorderRadius = CSSToDevPixels(4, sCSSToDevPixelScaling);
  int selectedTabWidth =
      CSSToDevPixels(221, sCSSToDevPixelScaling) + 2 * selectedTabBorderWidth;
  int toolbarHeight = CSSToDevPixels(40, sCSSToDevPixelScaling);
  // found in browser.css, "#PersonalToolbar"
  int bookmarkToolbarHeight = CSSToDevPixels(28, sCSSToDevPixelScaling);
  if (bookmarksToolbarShown) {
    toolbarHeight += bookmarkToolbarHeight;
  }
  // found in urlbar-searchbar.inc.css, "#urlbar[breakout]"
  int urlbarTopOffset = CSSToDevPixels(4, sCSSToDevPixelScaling);
  int urlbarHeight = CSSToDevPixels(32, sCSSToDevPixelScaling);
  // found in browser-aero.css, "#navigator-toolbox::after" border-bottom
  int chromeContentDividerHeight = CSSToDevPixels(1, sCSSToDevPixelScaling);

  int tabPlaceholderBarMarginTop = CSSToDevPixels(14, sCSSToDevPixelScaling);
  int tabPlaceholderBarMarginLeft = CSSToDevPixels(10, sCSSToDevPixelScaling);
  int tabPlaceholderBarHeight = CSSToDevPixels(10, sCSSToDevPixelScaling);
  int tabPlaceholderBarWidth = CSSToDevPixels(120, sCSSToDevPixelScaling);

  int toolbarPlaceholderHeight = CSSToDevPixels(10, sCSSToDevPixelScaling);
  int toolbarPlaceholderMarginRight =
      rtlEnabled ? CSSToDevPixels(11, sCSSToDevPixelScaling)
                 : CSSToDevPixels(9, sCSSToDevPixelScaling);
  int toolbarPlaceholderMarginLeft =
      rtlEnabled ? CSSToDevPixels(9, sCSSToDevPixelScaling)
                 : CSSToDevPixels(11, sCSSToDevPixelScaling);
  int placeholderMargin = CSSToDevPixels(8, sCSSToDevPixelScaling);

  int menubarHeightDevPixels =
      menubarShown ? CSSToDevPixels(28, sCSSToDevPixelScaling) : 0;

  // defined in urlbar-searchbar.inc.css as --urlbar-margin-inline: 5px
  int urlbarMargin =
      CSSToDevPixels(5, sCSSToDevPixelScaling) + horizontalOffset;

  int urlbarTextPlaceholderMarginTop =
      CSSToDevPixels(12, sCSSToDevPixelScaling);
  int urlbarTextPlaceholderMarginLeft =
      CSSToDevPixels(12, sCSSToDevPixelScaling);
  int urlbarTextPlaceHolderWidth = CSSToDevPixels(
      std::clamp(urlbarCSSSpan.end - urlbarCSSSpan.start - 10.0, 0.0, 260.0),
      sCSSToDevPixelScaling);
  int urlbarTextPlaceholderHeight = CSSToDevPixels(10, sCSSToDevPixelScaling);

  int searchbarTextPlaceholderWidth = CSSToDevPixels(62, sCSSToDevPixelScaling);

  auto scopeExit = MakeScopeExit([&] {
    delete sAnimatedRects;
    sAnimatedRects = nullptr;
  });

  Vector<ColorRect> rects;

  ColorRect menubar = {};
  menubar.color = currentTheme.tabBarColor;
  menubar.x = 0;
  menubar.y = verticalOffset;
  menubar.width = sWindowWidth;
  menubar.height = menubarHeightDevPixels;
  menubar.flipIfRTL = false;
  if (!rects.append(menubar)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  int placeholderBorderRadius = CSSToDevPixels(4, sCSSToDevPixelScaling);
  // found in browser.css "--toolbarbutton-border-radius"
  int urlbarBorderRadius = CSSToDevPixels(4, sCSSToDevPixelScaling);

  // The (traditionally dark blue on Windows) background of the tab bar.
  ColorRect tabBar = {};
  tabBar.color = currentTheme.tabBarColor;
  tabBar.x = 0;
  tabBar.y = menubar.y + menubar.height;
  tabBar.width = sWindowWidth;
  tabBar.height = tabBarHeight;
  tabBar.flipIfRTL = false;
  if (!rects.append(tabBar)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  // The initial selected tab
  ColorRect selectedTab = {};
  selectedTab.color = currentTheme.tabColor;
  selectedTab.x = titlebarSpacerWidth;
  selectedTab.y = menubar.y + menubar.height + selectedTabMarginTop;
  selectedTab.width = selectedTabWidth;
  selectedTab.height =
      tabBar.y + tabBar.height - selectedTab.y - selectedTabMarginBottom;
  selectedTab.borderColor = currentTheme.tabOutlineColor;
  selectedTab.borderWidth = selectedTabBorderWidth;
  selectedTab.borderRadius = selectedTabBorderRadius;
  selectedTab.flipIfRTL = true;
  if (!rects.append(selectedTab)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  // A placeholder rect representing text that will fill the selected tab title
  ColorRect tabTextPlaceholder = {};
  tabTextPlaceholder.color = currentTheme.toolbarForegroundColor;
  tabTextPlaceholder.x = selectedTab.x + tabPlaceholderBarMarginLeft;
  tabTextPlaceholder.y = selectedTab.y + tabPlaceholderBarMarginTop;
  tabTextPlaceholder.width = tabPlaceholderBarWidth;
  tabTextPlaceholder.height = tabPlaceholderBarHeight;
  tabTextPlaceholder.borderRadius = placeholderBorderRadius;
  tabTextPlaceholder.flipIfRTL = true;
  if (!rects.append(tabTextPlaceholder)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  // The toolbar background
  ColorRect toolbar = {};
  toolbar.color = currentTheme.backgroundColor;
  toolbar.x = 0;
  toolbar.y = tabBar.y + tabBarHeight;
  toolbar.width = sWindowWidth;
  toolbar.height = toolbarHeight;
  toolbar.flipIfRTL = false;
  if (!rects.append(toolbar)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  // The single-pixel divider line below the toolbar
  ColorRect chromeContentDivider = {};
  chromeContentDivider.color = currentTheme.chromeContentDividerColor;
  chromeContentDivider.x = 0;
  chromeContentDivider.y = toolbar.y + toolbar.height;
  chromeContentDivider.width = sWindowWidth;
  chromeContentDivider.height = chromeContentDividerHeight;
  chromeContentDivider.flipIfRTL = false;
  if (!rects.append(chromeContentDivider)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  // The urlbar
  ColorRect urlbar = {};
  urlbar.color = currentTheme.urlbarColor;
  urlbar.x = CSSToDevPixels(urlbarCSSSpan.start, sCSSToDevPixelScaling) +
             horizontalOffset;
  urlbar.y = tabBar.y + tabBarHeight + urlbarTopOffset;
  urlbar.width = CSSToDevPixels((urlbarCSSSpan.end - urlbarCSSSpan.start),
                                sCSSToDevPixelScaling);
  urlbar.height = urlbarHeight;
  urlbar.borderColor = currentTheme.urlbarBorderColor;
  urlbar.borderWidth = CSSToDevPixels(1, sCSSToDevPixelScaling);
  urlbar.borderRadius = urlbarBorderRadius;
  urlbar.flipIfRTL = false;
  if (!rects.append(urlbar)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  // The urlbar placeholder rect representating text that will fill the urlbar
  // If rtl is enabled, it is flipped relative to the the urlbar rectangle, not
  // sWindowWidth.
  ColorRect urlbarTextPlaceholder = {};
  urlbarTextPlaceholder.color = currentTheme.toolbarForegroundColor;
  urlbarTextPlaceholder.x =
      rtlEnabled
          ? ((urlbar.x + urlbar.width) - urlbarTextPlaceholderMarginLeft -
             urlbarTextPlaceHolderWidth)
          : (urlbar.x + urlbarTextPlaceholderMarginLeft);
  urlbarTextPlaceholder.y = urlbar.y + urlbarTextPlaceholderMarginTop;
  urlbarTextPlaceholder.width = urlbarTextPlaceHolderWidth;
  urlbarTextPlaceholder.height = urlbarTextPlaceholderHeight;
  urlbarTextPlaceholder.borderRadius = placeholderBorderRadius;
  urlbarTextPlaceholder.flipIfRTL = false;
  if (!rects.append(urlbarTextPlaceholder)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  // The searchbar and placeholder text, if present
  // This is y-aligned with the urlbar
  bool hasSearchbar = searchbarCSSSpan.start != 0 && searchbarCSSSpan.end != 0;
  ColorRect searchbarRect = {};
  if (hasSearchbar == true) {
    searchbarRect.color = currentTheme.urlbarColor;
    searchbarRect.x =
        CSSToDevPixels(searchbarCSSSpan.start, sCSSToDevPixelScaling) +
        horizontalOffset;
    searchbarRect.y = urlbar.y;
    searchbarRect.width = CSSToDevPixels(
        searchbarCSSSpan.end - searchbarCSSSpan.start, sCSSToDevPixelScaling);
    searchbarRect.height = urlbarHeight;
    searchbarRect.borderRadius = urlbarBorderRadius;
    searchbarRect.borderColor = currentTheme.urlbarBorderColor;
    searchbarRect.borderWidth = CSSToDevPixels(1, sCSSToDevPixelScaling);
    searchbarRect.flipIfRTL = false;
    if (!rects.append(searchbarRect)) {
      return Err(PreXULSkeletonUIError::OOM);
    }

    // The placeholder rect representating text that will fill the searchbar
    // This uses the same margins as the urlbarTextPlaceholder
    // If rtl is enabled, it is flipped relative to the the searchbar rectangle,
    // not sWindowWidth.
    ColorRect searchbarTextPlaceholder = {};
    searchbarTextPlaceholder.color = currentTheme.toolbarForegroundColor;
    searchbarTextPlaceholder.x =
        rtlEnabled
            ? ((searchbarRect.x + searchbarRect.width) -
               urlbarTextPlaceholderMarginLeft - searchbarTextPlaceholderWidth)
            : (searchbarRect.x + urlbarTextPlaceholderMarginLeft);
    searchbarTextPlaceholder.y =
        searchbarRect.y + urlbarTextPlaceholderMarginTop;
    searchbarTextPlaceholder.width = searchbarTextPlaceholderWidth;
    searchbarTextPlaceholder.height = urlbarTextPlaceholderHeight;
    searchbarTextPlaceholder.flipIfRTL = false;
    if (!rects.append(searchbarTextPlaceholder) ||
        !sAnimatedRects->append(searchbarTextPlaceholder)) {
      return Err(PreXULSkeletonUIError::OOM);
    }
  }

  // Determine where the placeholder rectangles should not go. This is
  // anywhere occupied by a spring, urlbar, or searchbar
  Vector<DevPixelSpan> noPlaceholderSpans;

  DevPixelSpan urlbarSpan;
  urlbarSpan.start = urlbar.x - urlbarMargin;
  urlbarSpan.end = urlbar.width + urlbar.x + urlbarMargin;

  DevPixelSpan searchbarSpan;
  if (hasSearchbar) {
    searchbarSpan.start = searchbarRect.x - urlbarMargin;
    searchbarSpan.end = searchbarRect.width + searchbarRect.x + urlbarMargin;
  }

  DevPixelSpan marginLeftPlaceholder;
  marginLeftPlaceholder.start = toolbarPlaceholderMarginLeft;
  marginLeftPlaceholder.end = toolbarPlaceholderMarginLeft;
  if (!noPlaceholderSpans.append(marginLeftPlaceholder)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  if (rtlEnabled) {
    // If we're RTL, then the springs as ordered in the DOM will be from right
    // to left, which will break our comparison logic below
    springs.reverse();
  }

  for (auto spring : springs) {
    DevPixelSpan springDevPixels;
    springDevPixels.start =
        CSSToDevPixels(spring.start, sCSSToDevPixelScaling) + horizontalOffset;
    springDevPixels.end =
        CSSToDevPixels(spring.end, sCSSToDevPixelScaling) + horizontalOffset;
    if (!noPlaceholderSpans.append(springDevPixels)) {
      return Err(PreXULSkeletonUIError::OOM);
    }
  }

  DevPixelSpan marginRightPlaceholder;
  marginRightPlaceholder.start = sWindowWidth - toolbarPlaceholderMarginRight;
  marginRightPlaceholder.end = sWindowWidth - toolbarPlaceholderMarginRight;
  if (!noPlaceholderSpans.append(marginRightPlaceholder)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  Vector<DevPixelSpan, 2> spansToAdd;
  Unused << spansToAdd.reserve(2);
  spansToAdd.infallibleAppend(urlbarSpan);
  if (hasSearchbar) {
    spansToAdd.infallibleAppend(searchbarSpan);
  }

  for (auto& toAdd : spansToAdd) {
    for (auto& span : noPlaceholderSpans) {
      if (span.start > toAdd.start) {
        if (!noPlaceholderSpans.insert(&span, toAdd)) {
          return Err(PreXULSkeletonUIError::OOM);
        }
        break;
      }
    }
  }

  for (size_t i = 1; i < noPlaceholderSpans.length(); i++) {
    int start = noPlaceholderSpans[i - 1].end + placeholderMargin;
    int end = noPlaceholderSpans[i].start - placeholderMargin;
    if (start + 2 * placeholderBorderRadius >= end) {
      continue;
    }

    // The placeholder rects should all be y-aligned.
    ColorRect placeholderRect = {};
    placeholderRect.color = currentTheme.toolbarForegroundColor;
    placeholderRect.x = start;
    placeholderRect.y = urlbarTextPlaceholder.y;
    placeholderRect.width = end - start;
    placeholderRect.height = toolbarPlaceholderHeight;
    placeholderRect.borderRadius = placeholderBorderRadius;
    placeholderRect.flipIfRTL = false;
    if (!rects.append(placeholderRect) ||
        !sAnimatedRects->append(placeholderRect)) {
      return Err(PreXULSkeletonUIError::OOM);
    }
  }

  sTotalChromeHeight = chromeContentDivider.y + chromeContentDivider.height;
  if (sTotalChromeHeight > sWindowHeight) {
    return Err(PreXULSkeletonUIError::BadWindowDimensions);
  }

  if (!sAnimatedRects->append(tabTextPlaceholder) ||
      !sAnimatedRects->append(urlbarTextPlaceholder)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  sPixelBuffer =
      (uint32_t*)calloc(sWindowWidth * sTotalChromeHeight, sizeof(uint32_t));

  for (auto& rect : *sAnimatedRects) {
    if (rtlEnabled && rect.flipIfRTL) {
      rect.x = sWindowWidth - rect.x - rect.width;
    }
    rect.x = std::clamp(rect.x, 0, sWindowWidth);
    rect.width = std::clamp(rect.width, 0, sWindowWidth - rect.x);
    rect.y = std::clamp(rect.y, 0, sTotalChromeHeight);
    rect.height = std::clamp(rect.height, 0, sTotalChromeHeight - rect.y);
  }

  for (auto& rect : rects) {
    if (rtlEnabled && rect.flipIfRTL) {
      rect.x = sWindowWidth - rect.x - rect.width;
    }
    rect.x = std::clamp(rect.x, 0, sWindowWidth);
    rect.width = std::clamp(rect.width, 0, sWindowWidth - rect.x);
    rect.y = std::clamp(rect.y, 0, sTotalChromeHeight);
    rect.height = std::clamp(rect.height, 0, sTotalChromeHeight - rect.y);
    RasterizeColorRect(rect);
  }

  HDC hdc = sGetWindowDC(hWnd);
  if (!hdc) {
    return Err(PreXULSkeletonUIError::FailedGettingDC);
  }
  auto cleanupDC = MakeScopeExit([=] { sReleaseDC(hWnd, hdc); });

  BITMAPINFO chromeBMI = {};
  chromeBMI.bmiHeader.biSize = sizeof(chromeBMI.bmiHeader);
  chromeBMI.bmiHeader.biWidth = sWindowWidth;
  chromeBMI.bmiHeader.biHeight = -sTotalChromeHeight;
  chromeBMI.bmiHeader.biPlanes = 1;
  chromeBMI.bmiHeader.biBitCount = 32;
  chromeBMI.bmiHeader.biCompression = BI_RGB;

  // First, we just paint the chrome area with our pixel buffer
  int scanLinesCopied = sStretchDIBits(
      hdc, 0, 0, sWindowWidth, sTotalChromeHeight, 0, 0, sWindowWidth,
      sTotalChromeHeight, sPixelBuffer, &chromeBMI, DIB_RGB_COLORS, SRCCOPY);
  if (scanLinesCopied == 0) {
    return Err(PreXULSkeletonUIError::FailedBlitting);
  }

  // Then, we just fill the rest with FillRect
  RECT rect = {0, sTotalChromeHeight, sWindowWidth, sWindowHeight};
  HBRUSH brush =
      sCreateSolidBrush(RGB((currentTheme.backgroundColor & 0xff0000) >> 16,
                            (currentTheme.backgroundColor & 0x00ff00) >> 8,
                            (currentTheme.backgroundColor & 0x0000ff) >> 0));
  int fillRectResult = sFillRect(hdc, &rect, brush);

  sDeleteObject(brush);

  if (fillRectResult == 0) {
    return Err(PreXULSkeletonUIError::FailedFillingBottomRect);
  }

  scopeExit.release();
  return Ok();
}

DWORD WINAPI AnimateSkeletonUI(void* aUnused) {
  if (!sPixelBuffer || sAnimatedRects->empty()) {
    return 0;
  }

  // See the comments above the InterlockedIncrement calls below here - we
  // atomically flip this up and down around sleep so the main thread doesn't
  // have to wait for us if we're just sleeping.
  if (InterlockedIncrement(&sAnimationControlFlag) != 1) {
    return 0;
  }
  // Sleep for two seconds - startups faster than this don't really benefit
  // from an animation, and we don't want to take away cycles from them.
  // Startups longer than this, however, are more likely to be blocked on IO,
  // and thus animating does not substantially impact startup times for them.
  ::Sleep(2000);
  if (InterlockedDecrement(&sAnimationControlFlag) != 0) {
    return 0;
  }

  // On each of the animated rects (which happen to all be placeholder UI
  // rects sharing the same color), we want to animate a gradient moving across
  // the screen from left to right. The gradient starts as the rect's color on,
  // the left side, changes to the background color of the window by the middle
  // of the gradient, and then goes back down to the rect's color. To make this
  // faster than interpolating between the two colors for each pixel for each
  // frame, we simply create a lookup buffer in which we can look up the color
  // for a particular offset into the gradient.
  //
  // To do this we just interpolate between the two values, and to give the
  // gradient a smoother transition between colors, we transform the linear
  // blend amount via the cubic smooth step function (SmoothStep3) to produce
  // a smooth start and stop for the gradient. We do this for the first half
  // of the gradient, and then simply copy that backwards for the second half.
  //
  // The CSS width of 80 chosen here is effectively is just to match the size
  // of the animation provided in the design mockup. We define it in CSS pixels
  // simply because the rest of our UI is based off of CSS scalings.
  int animationWidth = CSSToDevPixels(80, sCSSToDevPixelScaling);
  UniquePtr<uint32_t[]> animationLookup =
      MakeUnique<uint32_t[]>(animationWidth);
  uint32_t animationColor = sAnimationColor;
  NormalizedRGB rgbBlend = UintToRGB(animationColor);

  // Build the first half of the lookup table
  for (int i = 0; i < animationWidth / 2; ++i) {
    uint32_t baseColor = sToolbarForegroundColor;
    double blendAmountLinear =
        static_cast<double>(i) / (static_cast<double>(animationWidth / 2));
    double blendAmount = SmoothStep3(blendAmountLinear);

    NormalizedRGB rgbBase = UintToRGB(baseColor);
    NormalizedRGB rgb = Lerp(rgbBase, rgbBlend, blendAmount);
    animationLookup[i] = RGBToUint(rgb);
  }

  // Copy the first half of the lookup table into the second half backwards
  for (int i = animationWidth / 2; i < animationWidth; ++i) {
    int j = animationWidth - 1 - i;
    if (j == animationWidth / 2) {
      // If animationWidth is odd, we'll be left with one pixel at the center.
      // Just color that as the animation color.
      animationLookup[i] = animationColor;
    } else {
      animationLookup[i] = animationLookup[j];
    }
  }

  // The bitmap info remains unchanged throughout the animation - this just
  // effectively describes the contents of sPixelBuffer
  BITMAPINFO chromeBMI = {};
  chromeBMI.bmiHeader.biSize = sizeof(chromeBMI.bmiHeader);
  chromeBMI.bmiHeader.biWidth = sWindowWidth;
  chromeBMI.bmiHeader.biHeight = -sTotalChromeHeight;
  chromeBMI.bmiHeader.biPlanes = 1;
  chromeBMI.bmiHeader.biBitCount = 32;
  chromeBMI.bmiHeader.biCompression = BI_RGB;

  uint32_t animationIteration = 0;

  int devPixelsPerFrame =
      CSSToDevPixels(kAnimationCSSPixelsPerFrame, sCSSToDevPixelScaling);
  int devPixelsExtraWindowSize =
      CSSToDevPixels(kAnimationCSSExtraWindowSize, sCSSToDevPixelScaling);

  if (::InterlockedCompareExchange(&sAnimationControlFlag, 0, 0)) {
    // The window got consumed before we were able to draw anything.
    return 0;
  }

  while (true) {
    // The gradient will move across the screen at devPixelsPerFrame at
    // 60fps, and then loop back to the beginning. However, we add a buffer of
    // devPixelsExtraWindowSize around the edges so it doesn't immediately
    // jump back, giving it a more pulsing feel.
    int animationMin = ((animationIteration * devPixelsPerFrame) %
                        (sWindowWidth + devPixelsExtraWindowSize)) -
                       devPixelsExtraWindowSize / 2;
    int animationMax = animationMin + animationWidth;
    // The priorAnimationMin is the beginning of the previous frame's animation.
    // Since we only want to draw the bits of the image that we updated, we need
    // to overwrite the left bit of the animation we drew last frame with the
    // default color.
    int priorAnimationMin = animationMin - devPixelsPerFrame;
    animationMin = std::max(0, animationMin);
    priorAnimationMin = std::max(0, priorAnimationMin);
    animationMax = std::min((int)sWindowWidth, animationMax);

    // The gradient only affects the specific rects that we put into
    // sAnimatedRects. So we simply update those rects, and maintain a flag
    // to avoid drawing when we don't need to.
    bool updatedAnything = false;
    for (ColorRect rect : *sAnimatedRects) {
      bool hadUpdates =
          RasterizeAnimatedRect(rect, animationLookup.get(), priorAnimationMin,
                                animationMin, animationMax);
      updatedAnything = updatedAnything || hadUpdates;
    }

    if (updatedAnything) {
      HDC hdc = sGetWindowDC(sPreXULSkeletonUIWindow);
      if (!hdc) {
        return 0;
      }

      sStretchDIBits(hdc, priorAnimationMin, 0,
                     animationMax - priorAnimationMin, sTotalChromeHeight,
                     priorAnimationMin, 0, animationMax - priorAnimationMin,
                     sTotalChromeHeight, sPixelBuffer, &chromeBMI,
                     DIB_RGB_COLORS, SRCCOPY);

      sReleaseDC(sPreXULSkeletonUIWindow, hdc);
    }

    animationIteration++;

    // We coordinate around our sleep here to ensure that the main thread does
    // not wait on us if we're sleeping. If we don't get 1 here, it means the
    // window has been consumed and we don't need to sleep. If in
    // ConsumePreXULSkeletonUIHandle we get a value other than 1 after
    // incrementing, it means we're sleeping, and that function can assume that
    // we will safely exit after the sleep because of the observed value of
    // sAnimationControlFlag.
    if (InterlockedIncrement(&sAnimationControlFlag) != 1) {
      return 0;
    }

    // Note: Sleep does not guarantee an exact time interval. If the system is
    // busy, for instance, we could easily end up taking several frames longer,
    // and really we could be left unscheduled for an arbitrarily long time.
    // This is fine, and we don't really care. We could track how much time this
    // actually took and jump the animation forward the appropriate amount, but
    // its not even clear that that's a better user experience. So we leave this
    // as simple as we can.
    ::Sleep(16);

    // Here we bring sAnimationControlFlag back down - again, if we don't get a
    // 0 here it means we consumed the skeleton UI window in the mean time, so
    // we can simply exit.
    if (InterlockedDecrement(&sAnimationControlFlag) != 0) {
      return 0;
    }
  }
}

LRESULT WINAPI PreXULSkeletonUIProc(HWND hWnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
  // Exposing a generic oleacc proxy for the skeleton isn't useful and may cause
  // screen readers to report spurious information when the skeleton appears.
  if (msg == WM_GETOBJECT && sPreXULSkeletonUIWindow) {
    return E_FAIL;
  }

  // NOTE: this block was copied from WinUtils.cpp, and needs to be kept in
  // sync.
  if (msg == WM_NCCREATE && sEnableNonClientDpiScaling) {
    sEnableNonClientDpiScaling(hWnd);
  }

  // NOTE: this block was paraphrased from the WM_NCCALCSIZE handler in
  // nsWindow.cpp, and will need to be kept in sync.
  if (msg == WM_NCCALCSIZE) {
    RECT* clientRect =
        wParam ? &(reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam))->rgrc[0]
               : (reinterpret_cast<RECT*>(lParam));

    // These match the margins set in browser-tabsintitlebar.js with
    // default prefs on Windows. Bug 1673092 tracks lining this up with
    // that more correctly instead of hard-coding it.
    int horizontalOffset =
        sNonClientHorizontalMargins -
        (sMaximized ? 0 : CSSToDevPixels(2, sCSSToDevPixelScaling));
    int verticalOffset =
        sNonClientHorizontalMargins -
        (sMaximized ? 0 : CSSToDevPixels(2, sCSSToDevPixelScaling));
    clientRect->top = clientRect->top;
    clientRect->left += horizontalOffset;
    clientRect->right -= horizontalOffset;
    clientRect->bottom -= verticalOffset;
    return 0;
  }

  return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

bool IsSystemDarkThemeEnabled() {
  DWORD result;
  HKEY themeKey;
  DWORD dataLen = sizeof(uint32_t);
  LPCWSTR keyName =
      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

  result = ::RegOpenKeyExW(HKEY_CURRENT_USER, keyName, 0, KEY_READ, &themeKey);
  if (result != ERROR_SUCCESS) {
    return false;
  }
  AutoCloseRegKey closeKey(themeKey);

  uint32_t lightThemeEnabled;
  result = ::RegGetValueW(
      themeKey, nullptr, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr,
      reinterpret_cast<PBYTE>(&lightThemeEnabled), &dataLen);
  if (result != ERROR_SUCCESS) {
    return false;
  }
  return !lightThemeEnabled;
}

ThemeColors GetTheme(ThemeMode themeId) {
  ThemeColors theme = {};
  switch (themeId) {
    case ThemeMode::Dark:
      // Dark theme or default theme when in dark mode

      // controlled by css variable --toolbar-bgcolor
      theme.backgroundColor = 0x2b2a33;
      theme.tabColor = 0x42414d;
      theme.toolbarForegroundColor = 0x6a6a6d;
      theme.tabOutlineColor = 0x1c1b22;
      // controlled by css variable --lwt-accent-color
      theme.tabBarColor = 0x1c1b22;
      // controlled by --toolbar-non-lwt-textcolor in browser.css
      theme.chromeContentDividerColor = 0x0c0c0d;
      // controlled by css variable --toolbar-field-background-color
      theme.urlbarColor = 0x42414d;
      theme.urlbarBorderColor = 0x42414d;
      theme.animationColor = theme.urlbarColor;
      return theme;
    case ThemeMode::Light:
    case ThemeMode::Default:
    default:
      // --toolbar-non-lwt-bgcolor in browser.css
      theme.backgroundColor = 0xf9f9fb;
      theme.tabColor = 0xf9f9fb;
      theme.toolbarForegroundColor = 0xdddde1;
      theme.tabOutlineColor = 0xdddde1;
      // found in browser-aero.css ":root[tabsintitlebar]:not(:-moz-lwtheme)"
      // (set to "hsl(235,33%,19%)")
      theme.tabBarColor = 0xf0f0f4;
      // --chrome-content-separator-color in browser.css
      theme.chromeContentDividerColor = 0xe1e1e2;
      // controlled by css variable --toolbar-color
      theme.urlbarColor = 0xffffff;
      theme.urlbarBorderColor = 0xdddde1;
      theme.animationColor = theme.backgroundColor;
      return theme;
  }
}

Result<HKEY, PreXULSkeletonUIError> OpenPreXULSkeletonUIRegKey() {
  HKEY key;
  DWORD disposition;
  LSTATUS result =
      ::RegCreateKeyExW(HKEY_CURRENT_USER, kPreXULSkeletonUIKeyPath, 0, nullptr,
                        0, KEY_ALL_ACCESS, nullptr, &key, &disposition);

  if (result != ERROR_SUCCESS) {
    return Err(PreXULSkeletonUIError::FailedToOpenRegistryKey);
  }

  if (disposition == REG_CREATED_NEW_KEY ||
      disposition == REG_OPENED_EXISTING_KEY) {
    return key;
  }

  ::RegCloseKey(key);
  return Err(PreXULSkeletonUIError::FailedToOpenRegistryKey);
}

Result<Ok, PreXULSkeletonUIError> LoadGdi32AndUser32Procedures() {
  HMODULE user32Dll = ::LoadLibraryW(L"user32");
  HMODULE gdi32Dll = ::LoadLibraryW(L"gdi32");

  if (!user32Dll || !gdi32Dll) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }

  auto getThreadDpiAwarenessContext =
      (decltype(GetThreadDpiAwarenessContext)*)::GetProcAddress(
          user32Dll, "GetThreadDpiAwarenessContext");
  auto areDpiAwarenessContextsEqual =
      (decltype(AreDpiAwarenessContextsEqual)*)::GetProcAddress(
          user32Dll, "AreDpiAwarenessContextsEqual");
  if (getThreadDpiAwarenessContext && areDpiAwarenessContextsEqual &&
      areDpiAwarenessContextsEqual(getThreadDpiAwarenessContext(),
                                   DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
    // EnableNonClientDpiScaling is optional - we can handle not having it.
    sEnableNonClientDpiScaling =
        (EnableNonClientDpiScalingProc)::GetProcAddress(
            user32Dll, "EnableNonClientDpiScaling");
  }

  sGetSystemMetricsForDpi = (GetSystemMetricsForDpiProc)::GetProcAddress(
      user32Dll, "GetSystemMetricsForDpi");
  if (!sGetSystemMetricsForDpi) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sGetDpiForWindow =
      (GetDpiForWindowProc)::GetProcAddress(user32Dll, "GetDpiForWindow");
  if (!sGetDpiForWindow) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sRegisterClassW =
      (RegisterClassWProc)::GetProcAddress(user32Dll, "RegisterClassW");
  if (!sRegisterClassW) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sCreateWindowExW =
      (CreateWindowExWProc)::GetProcAddress(user32Dll, "CreateWindowExW");
  if (!sCreateWindowExW) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sShowWindow = (ShowWindowProc)::GetProcAddress(user32Dll, "ShowWindow");
  if (!sShowWindow) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sSetWindowPos = (SetWindowPosProc)::GetProcAddress(user32Dll, "SetWindowPos");
  if (!sSetWindowPos) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sGetWindowDC = (GetWindowDCProc)::GetProcAddress(user32Dll, "GetWindowDC");
  if (!sGetWindowDC) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sFillRect = (FillRectProc)::GetProcAddress(user32Dll, "FillRect");
  if (!sFillRect) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sReleaseDC = (ReleaseDCProc)::GetProcAddress(user32Dll, "ReleaseDC");
  if (!sReleaseDC) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sLoadIconW = (LoadIconWProc)::GetProcAddress(user32Dll, "LoadIconW");
  if (!sLoadIconW) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sLoadCursorW = (LoadCursorWProc)::GetProcAddress(user32Dll, "LoadCursorW");
  if (!sLoadCursorW) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sMonitorFromWindow =
      (MonitorFromWindowProc)::GetProcAddress(user32Dll, "MonitorFromWindow");
  if (!sMonitorFromWindow) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sGetMonitorInfoW =
      (GetMonitorInfoWProc)::GetProcAddress(user32Dll, "GetMonitorInfoW");
  if (!sGetMonitorInfoW) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sSetWindowLongPtrW =
      (SetWindowLongPtrWProc)::GetProcAddress(user32Dll, "SetWindowLongPtrW");
  if (!sSetWindowLongPtrW) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sStretchDIBits =
      (StretchDIBitsProc)::GetProcAddress(gdi32Dll, "StretchDIBits");
  if (!sStretchDIBits) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sCreateSolidBrush =
      (CreateSolidBrushProc)::GetProcAddress(gdi32Dll, "CreateSolidBrush");
  if (!sCreateSolidBrush) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }
  sDeleteObject = (DeleteObjectProc)::GetProcAddress(gdi32Dll, "DeleteObject");
  if (!sDeleteObject) {
    return Err(PreXULSkeletonUIError::FailedLoadingDynamicProcs);
  }

  return Ok();
}

// Strips "--", "-", and "/" from the front of the arg if one of those exists,
// returning `arg + 2`, `arg + 1`, and `arg + 1` respectively. If none of these
// prefixes are found, the argument is not a flag, and nullptr is returned.
const char* NormalizeFlag(const char* arg) {
  if (strstr(arg, "--") == arg) {
    return arg + 2;
  }

  if (arg[0] == '-') {
    return arg + 1;
  }

  if (arg[0] == '/') {
    return arg + 1;
  }

  return nullptr;
}

static bool EnvHasValue(const char* name) {
  const char* val = getenv(name);
  return (val && *val);
}

// Ensures that we only see arguments in the command line which are acceptable.
// This is based on manual inspection of the list of arguments listed in the MDN
// page for Gecko/Firefox commandline options:
// https://developer.mozilla.org/en-US/docs/Mozilla/Command_Line_Options
// Broadly speaking, we want to reject any argument which causes us to show
// something other than the default window at its normal size. Here is a non-
// exhaustive list of command line options we want to *exclude*:
//
//   -ProfileManager : This will display the profile manager window, which does
//                     not match the skeleton UI at all.
//
//   -CreateProfile  : This will display a firefox window with the default
//                     screen position and size, and not the position and size
//                     which we have recorded in the registry.
//
//   -P <profile>    : This could cause us to display firefox with a position
//                     and size of a different profile than that in which we
//                     were previously running.
//
//   -width, -height : This will cause the width and height values in the
//                     registry to be incorrect.
//
//   -kiosk          : See above.
//
//   -headless       : This one should be rather obvious.
//
//   -migration      : This will start with the import wizard, which of course
//                     does not match the skeleton UI.
//
//   -private-window : This is tricky, but the colors of the main content area
//                     make this not feel great with the white content of the
//                     default skeleton UI.
//
// NOTE: we generally want to skew towards erroneous rejections of the command
// line rather than erroneous approvals. The consequence of a bad rejection
// is that we don't show the skeleton UI, which is business as usual. The
// consequence of a bad approval is that we show it when we're not supposed to,
// which is visually jarring and can also be unpredictable - there's no
// guarantee that the code which handles the non-default window is set up to
// properly handle the transition from the skeleton UI window.
static Result<Ok, PreXULSkeletonUIError> ValidateCmdlineArguments(
    int argc, char** argv, bool* explicitProfile) {
  const char* approvedArgumentsArray[] = {
      // These won't cause the browser to be visualy different in any way
      "new-instance", "no-remote", "browser", "foreground", "setDefaultBrowser",
      "attach-console", "wait-for-browser", "osint",

      // These will cause the chrome to be a bit different or extra windows to
      // be created, but overall the skeleton UI should still be broadly
      // correct enough.
      "new-tab", "new-window",

      // To the extent possible, we want to ensure that existing tests cover
      // the skeleton UI, so we need to allow marionette
      "marionette",

      // These will cause the content area to appear different, but won't
      // meaningfully affect the chrome
      "preferences", "search", "url",

#ifndef MOZILLA_OFFICIAL
      // On local builds, we want to allow -profile, because it's how `mach run`
      // operates, and excluding that would create an unnecessary blind spot for
      // Firefox devs.
      "profile"
#endif

      // There are other arguments which are likely okay. However, they are
      // not included here because this list is not intended to be
      // exhaustive - it only intends to green-light some somewhat commonly
      // used arguments. We want to err on the side of an unnecessary
      // rejection of the command line.
  };

  int approvedArgumentsArraySize =
      sizeof(approvedArgumentsArray) / sizeof(approvedArgumentsArray[0]);
  Vector<const char*> approvedArguments;
  if (!approvedArguments.reserve(approvedArgumentsArraySize)) {
    return Err(PreXULSkeletonUIError::OOM);
  }

  for (int i = 0; i < approvedArgumentsArraySize; ++i) {
    approvedArguments.infallibleAppend(approvedArgumentsArray[i]);
  }

#ifdef MOZILLA_OFFICIAL
  int profileArgIndex = -1;
  // If we're running mochitests or direct marionette tests, those specify a
  // temporary profile, and we want to ensure that we get the added coverage
  // from those.
  for (int i = 1; i < argc; ++i) {
    const char* flag = NormalizeFlag(argv[i]);
    if (flag && !strcmp(flag, "marionette")) {
      if (!approvedArguments.append("profile")) {
        return Err(PreXULSkeletonUIError::OOM);
      }
      profileArgIndex = approvedArguments.length() - 1;

      break;
    }
  }
#else
  int profileArgIndex = approvedArguments.length() - 1;
#endif

  for (int i = 1; i < argc; ++i) {
    const char* flag = NormalizeFlag(argv[i]);
    if (!flag) {
      // If this is not a flag, then we interpret it as a URL, similar to
      // BrowserContentHandler.sys.mjs. Some command line options take
      // additional arguments, which may or may not be URLs. We don't need to
      // know this, because we don't need to parse them out; we just rely on the
      // assumption that if arg X is actually a parameter for the preceding
      // arg Y, then X must not look like a flag (starting with "--", "-",
      // or "/").
      //
      // The most important thing here is the assumption that if something is
      // going to meaningfully alter the appearance of the window itself, it
      // must be a flag.
      continue;
    }

    bool approved = false;
    for (const char* approvedArg : approvedArguments) {
      // We do a case-insensitive compare here with _stricmp. Even though some
      // of these arguments are *not* read as case-insensitive, others *are*.
      // Similar to the flag logic above, we don't really care about this
      // distinction, because we don't need to parse the arguments - we just
      // rely on the assumption that none of the listed flags in our
      // approvedArguments are overloaded in such a way that a different
      // casing would visually alter the firefox window.
      if (!_stricmp(flag, approvedArg)) {
        approved = true;

        if (i == profileArgIndex) {
          *explicitProfile = true;
        }
        break;
      }
    }

    if (!approved) {
      return Err(PreXULSkeletonUIError::Cmdline);
    }
  }

  return Ok();
}

static Result<Ok, PreXULSkeletonUIError> ValidateEnvVars() {
  if (EnvHasValue("MOZ_SAFE_MODE_RESTART") ||
      EnvHasValue("MOZ_APP_SILENT_START") ||
      EnvHasValue("MOZ_RESET_PROFILE_RESTART") || EnvHasValue("MOZ_HEADLESS") ||
      (EnvHasValue("XRE_PROFILE_PATH") &&
       !EnvHasValue("MOZ_SKELETON_UI_RESTARTING"))) {
    return Err(PreXULSkeletonUIError::EnvVars);
  }

  return Ok();
}

static bool VerifyWindowDimensions(uint32_t windowWidth,
                                   uint32_t windowHeight) {
  return windowWidth <= kMaxWindowWidth && windowHeight <= kMaxWindowHeight;
}

static Result<Vector<CSSPixelSpan>, PreXULSkeletonUIError> ReadRegCSSPixelSpans(
    HKEY regKey, const std::wstring& valueName) {
  DWORD dataLen = 0;
  LSTATUS result = ::RegQueryValueExW(regKey, valueName.c_str(), nullptr,
                                      nullptr, nullptr, &dataLen);
  if (result != ERROR_SUCCESS) {
    return Err(PreXULSkeletonUIError::RegistryError);
  }

  if (dataLen % (2 * sizeof(double)) != 0) {
    return Err(PreXULSkeletonUIError::CorruptData);
  }

  auto buffer = MakeUniqueFallible<wchar_t[]>(dataLen);
  if (!buffer) {
    return Err(PreXULSkeletonUIError::OOM);
  }
  result =
      ::RegGetValueW(regKey, nullptr, valueName.c_str(), RRF_RT_REG_BINARY,
                     nullptr, reinterpret_cast<PBYTE>(buffer.get()), &dataLen);
  if (result != ERROR_SUCCESS) {
    return Err(PreXULSkeletonUIError::RegistryError);
  }

  Vector<CSSPixelSpan> resultVector;
  double* asDoubles = reinterpret_cast<double*>(buffer.get());
  for (size_t i = 0; i < dataLen / (2 * sizeof(double)); i++) {
    CSSPixelSpan span = {};
    span.start = *(asDoubles++);
    span.end = *(asDoubles++);
    if (!resultVector.append(span)) {
      return Err(PreXULSkeletonUIError::OOM);
    }
  }

  return resultVector;
}

static Result<double, PreXULSkeletonUIError> ReadRegDouble(
    HKEY regKey, const std::wstring& valueName) {
  double value = 0;
  DWORD dataLen = sizeof(double);
  LSTATUS result =
      ::RegGetValueW(regKey, nullptr, valueName.c_str(), RRF_RT_REG_BINARY,
                     nullptr, reinterpret_cast<PBYTE>(&value), &dataLen);
  if (result != ERROR_SUCCESS || dataLen != sizeof(double)) {
    return Err(PreXULSkeletonUIError::RegistryError);
  }

  return value;
}

static Result<uint32_t, PreXULSkeletonUIError> ReadRegUint(
    HKEY regKey, const std::wstring& valueName) {
  DWORD value = 0;
  DWORD dataLen = sizeof(uint32_t);
  LSTATUS result =
      ::RegGetValueW(regKey, nullptr, valueName.c_str(), RRF_RT_REG_DWORD,
                     nullptr, reinterpret_cast<PBYTE>(&value), &dataLen);
  if (result != ERROR_SUCCESS) {
    return Err(PreXULSkeletonUIError::RegistryError);
  }

  return value;
}

static Result<bool, PreXULSkeletonUIError> ReadRegBool(
    HKEY regKey, const std::wstring& valueName) {
  uint32_t value;
  MOZ_TRY_VAR(value, ReadRegUint(regKey, valueName));
  return !!value;
}

static Result<Ok, PreXULSkeletonUIError> WriteRegCSSPixelSpans(
    HKEY regKey, const std::wstring& valueName, const CSSPixelSpan* spans,
    int spansLength) {
  // No guarantee on the packing of CSSPixelSpan. We could #pragma it, but it's
  // also trivial to just copy them into a buffer of doubles.
  auto doubles = MakeUnique<double[]>(spansLength * 2);
  for (int i = 0; i < spansLength; ++i) {
    doubles[i * 2] = spans[i].start;
    doubles[i * 2 + 1] = spans[i].end;
  }

  LSTATUS result =
      ::RegSetValueExW(regKey, valueName.c_str(), 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(doubles.get()),
                       spansLength * sizeof(double) * 2);
  if (result != ERROR_SUCCESS) {
    return Err(PreXULSkeletonUIError::RegistryError);
  }
  return Ok();
}

static Result<Ok, PreXULSkeletonUIError> WriteRegDouble(
    HKEY regKey, const std::wstring& valueName, double value) {
  LSTATUS result =
      ::RegSetValueExW(regKey, valueName.c_str(), 0, REG_BINARY,
                       reinterpret_cast<const BYTE*>(&value), sizeof(value));
  if (result != ERROR_SUCCESS) {
    return Err(PreXULSkeletonUIError::RegistryError);
  }

  return Ok();
}

static Result<Ok, PreXULSkeletonUIError> WriteRegUint(
    HKEY regKey, const std::wstring& valueName, uint32_t value) {
  LSTATUS result =
      ::RegSetValueExW(regKey, valueName.c_str(), 0, REG_DWORD,
                       reinterpret_cast<PBYTE>(&value), sizeof(value));
  if (result != ERROR_SUCCESS) {
    return Err(PreXULSkeletonUIError::RegistryError);
  }

  return Ok();
}

static Result<Ok, PreXULSkeletonUIError> WriteRegBool(
    HKEY regKey, const std::wstring& valueName, bool value) {
  return WriteRegUint(regKey, valueName, value ? 1 : 0);
}

static Result<Ok, PreXULSkeletonUIError> CreateAndStorePreXULSkeletonUIImpl(
    HINSTANCE hInstance, int argc, char** argv) {
  // Initializing COM below may load modules via SetWindowHookEx, some of
  // which may modify the executable's IAT for ntdll.dll.  If that happens,
  // this browser process fails to launch sandbox processes because we cannot
  // copy a modified IAT into a remote process (See SandboxBroker::LaunchApp).
  // To prevent that, we cache the intact IAT before COM initialization.
  // If EAF+ is enabled, CacheNtDllThunk() causes a crash, but EAF+ will
  // also prevent an injected module from parsing the PE headers and modifying
  // the IAT.  Therefore, we can skip CacheNtDllThunk().
  if (!mozilla::IsEafPlusEnabled()) {
    CacheNtDllThunk();
  }

  // NOTE: it's important that we initialize sProcessRuntime before showing a
  // window. Historically we ran into issues where showing the window would
  // cause an accessibility win event to fire, which could cause in-process
  // system or third party components to initialize COM and prevent us from
  // initializing it with important settings we need.

  // Some COM settings are global to the process and must be set before any non-
  // trivial COM is run in the application. Since these settings may affect
  // stability, we should instantiate COM ASAP so that we can ensure that these
  // global settings are configured before anything can interfere.
  sProcessRuntime = new mscom::ProcessRuntime(
      mscom::ProcessRuntime::ProcessCategory::GeckoBrowserParent);

  const TimeStamp skeletonStart = TimeStamp::Now();

  HKEY regKey;
  MOZ_TRY_VAR(regKey, OpenPreXULSkeletonUIRegKey());
  AutoCloseRegKey closeKey(regKey);

  UniquePtr<wchar_t[]> binPath;
  MOZ_TRY_VAR(binPath, GetBinaryPath());

  std::wstring regProgressName =
      GetRegValueName(binPath.get(), sProgressSuffix);
  auto progressResult = ReadRegUint(regKey, regProgressName);
  if (!progressResult.isErr() &&
      progressResult.unwrap() !=
          static_cast<uint32_t>(PreXULSkeletonUIProgress::Completed)) {
    return Err(PreXULSkeletonUIError::CrashedOnce);
  }

  MOZ_TRY(
      WriteRegUint(regKey, regProgressName,
                   static_cast<uint32_t>(PreXULSkeletonUIProgress::Started)));
  auto writeCompletion = MakeScopeExit([&] {
    Unused << WriteRegUint(
        regKey, regProgressName,
        static_cast<uint32_t>(PreXULSkeletonUIProgress::Completed));
  });

  MOZ_TRY(GetSkeletonUILock());

  bool explicitProfile = false;
  MOZ_TRY(ValidateCmdlineArguments(argc, argv, &explicitProfile));
  MOZ_TRY(ValidateEnvVars());

  auto enabledResult =
      ReadRegBool(regKey, GetRegValueName(binPath.get(), sEnabledRegSuffix));
  if (enabledResult.isErr()) {
    return Err(PreXULSkeletonUIError::EnabledKeyDoesNotExist);
  }
  if (!enabledResult.unwrap()) {
    return Err(PreXULSkeletonUIError::Disabled);
  }
  sPreXULSkeletonUIEnabled = true;

  MOZ_ASSERT(!sAnimatedRects);
  sAnimatedRects = new Vector<ColorRect>();

  MOZ_TRY(LoadGdi32AndUser32Procedures());

  if (!explicitProfile) {
    MOZ_TRY(CheckForStartWithLastProfile());
  }

  WNDCLASSW wc;
  wc.style = CS_DBLCLKS;
  wc.lpfnWndProc = PreXULSkeletonUIProc;
  wc.cbClsExtra = 0;
  wc.cbWndExtra = 0;
  wc.hInstance = hInstance;
  wc.hIcon = sLoadIconW(::GetModuleHandleW(nullptr), gStockApplicationIcon);
  wc.hCursor = sLoadCursorW(hInstance, gIDCWait);
  wc.hbrBackground = nullptr;
  wc.lpszMenuName = nullptr;

  // TODO: just ensure we disable this if we've overridden the window class
  wc.lpszClassName = L"MozillaWindowClass";

  if (!sRegisterClassW(&wc)) {
    return Err(PreXULSkeletonUIError::FailedRegisteringWindowClass);
  }

  uint32_t screenX;
  MOZ_TRY_VAR(screenX, ReadRegUint(regKey, GetRegValueName(binPath.get(),
                                                           sScreenXRegSuffix)));
  uint32_t screenY;
  MOZ_TRY_VAR(screenY, ReadRegUint(regKey, GetRegValueName(binPath.get(),
                                                           sScreenYRegSuffix)));
  uint32_t windowWidth;
  MOZ_TRY_VAR(
      windowWidth,
      ReadRegUint(regKey, GetRegValueName(binPath.get(), sWidthRegSuffix)));
  uint32_t windowHeight;
  MOZ_TRY_VAR(
      windowHeight,
      ReadRegUint(regKey, GetRegValueName(binPath.get(), sHeightRegSuffix)));
  MOZ_TRY_VAR(
      sMaximized,
      ReadRegBool(regKey, GetRegValueName(binPath.get(), sMaximizedRegSuffix)));
  MOZ_TRY_VAR(
      sCSSToDevPixelScaling,
      ReadRegDouble(regKey, GetRegValueName(binPath.get(),
                                            sCssToDevPixelScalingRegSuffix)));
  Vector<CSSPixelSpan> urlbar;
  MOZ_TRY_VAR(urlbar,
              ReadRegCSSPixelSpans(
                  regKey, GetRegValueName(binPath.get(), sUrlbarCSSRegSuffix)));
  Vector<CSSPixelSpan> searchbar;
  MOZ_TRY_VAR(searchbar,
              ReadRegCSSPixelSpans(
                  regKey, GetRegValueName(binPath.get(), sSearchbarRegSuffix)));
  Vector<CSSPixelSpan> springs;
  MOZ_TRY_VAR(springs, ReadRegCSSPixelSpans(
                           regKey, GetRegValueName(binPath.get(),
                                                   sSpringsCSSRegSuffix)));

  if (urlbar.empty() || searchbar.empty()) {
    return Err(PreXULSkeletonUIError::CorruptData);
  }

  EnumSet<SkeletonUIFlag, uint32_t> flags;
  uint32_t flagsUint;
  MOZ_TRY_VAR(flagsUint, ReadRegUint(regKey, GetRegValueName(binPath.get(),
                                                             sFlagsRegSuffix)));
  flags.deserialize(flagsUint);

  if (flags.contains(SkeletonUIFlag::TouchDensity) ||
      flags.contains(SkeletonUIFlag::CompactDensity)) {
    return Err(PreXULSkeletonUIError::BadUIDensity);
  }

  uint32_t theme;
  MOZ_TRY_VAR(theme, ReadRegUint(regKey, GetRegValueName(binPath.get(),
                                                         sThemeRegSuffix)));
  ThemeMode themeMode = static_cast<ThemeMode>(theme);
  if (themeMode == ThemeMode::Default) {
    if (IsSystemDarkThemeEnabled() == true) {
      themeMode = ThemeMode::Dark;
    }
  }
  ThemeColors currentTheme = GetTheme(themeMode);

  if (!VerifyWindowDimensions(windowWidth, windowHeight)) {
    return Err(PreXULSkeletonUIError::BadWindowDimensions);
  }

  int showCmd = SW_SHOWNORMAL;
  DWORD windowStyle = kPreXULSkeletonUIWindowStyle;
  if (sMaximized) {
    showCmd = SW_SHOWMAXIMIZED;
    windowStyle |= WS_MAXIMIZE;
  }

  sPreXULSkeletonUIWindow =
      sCreateWindowExW(kPreXULSkeletonUIWindowStyleEx, L"MozillaWindowClass",
                       L"", windowStyle, screenX, screenY, windowWidth,
                       windowHeight, nullptr, nullptr, hInstance, nullptr);
  if (!sPreXULSkeletonUIWindow) {
    return Err(PreXULSkeletonUIError::CreateWindowFailed);
  }

  sShowWindow(sPreXULSkeletonUIWindow, showCmd);

  sDpi = sGetDpiForWindow(sPreXULSkeletonUIWindow);
  sNonClientHorizontalMargins =
      sGetSystemMetricsForDpi(SM_CXFRAME, sDpi) +
      sGetSystemMetricsForDpi(SM_CXPADDEDBORDER, sDpi);
  sNonClientVerticalMargins = sGetSystemMetricsForDpi(SM_CYFRAME, sDpi) +
                              sGetSystemMetricsForDpi(SM_CXPADDEDBORDER, sDpi);

  if (sMaximized) {
    HMONITOR monitor =
        sMonitorFromWindow(sPreXULSkeletonUIWindow, MONITOR_DEFAULTTONULL);
    if (!monitor) {
      // NOTE: we specifically don't clean up the window here. If we're unable
      // to finish setting up the window how we want it, we still need to keep
      // it around and consume it with the first real toplevel window we
      // create, to avoid flickering.
      return Err(PreXULSkeletonUIError::FailedGettingMonitorInfo);
    }
    MONITORINFO mi = {sizeof(MONITORINFO)};
    if (!sGetMonitorInfoW(monitor, &mi)) {
      return Err(PreXULSkeletonUIError::FailedGettingMonitorInfo);
    }

    sWindowWidth =
        mi.rcWork.right - mi.rcWork.left + sNonClientHorizontalMargins * 2;
    sWindowHeight =
        mi.rcWork.bottom - mi.rcWork.top + sNonClientVerticalMargins * 2;
  } else {
    sWindowWidth = static_cast<int>(windowWidth);
    sWindowHeight = static_cast<int>(windowHeight);
  }

  sSetWindowPos(sPreXULSkeletonUIWindow, 0, 0, 0, 0, 0,
                SWP_FRAMECHANGED | SWP_NOACTIVATE | SWP_NOMOVE |
                    SWP_NOOWNERZORDER | SWP_NOSIZE | SWP_NOZORDER);
  MOZ_TRY(DrawSkeletonUI(sPreXULSkeletonUIWindow, urlbar[0], searchbar[0],
                         springs, currentTheme, flags));
  if (sAnimatedRects) {
    sPreXULSKeletonUIAnimationThread = ::CreateThread(
        nullptr, 256 * 1024, AnimateSkeletonUI, nullptr, 0, nullptr);
  }

  BASE_PROFILER_MARKER_UNTYPED(
      "CreatePreXULSkeletonUI", OTHER,
      MarkerTiming::IntervalUntilNowFrom(skeletonStart));

  return Ok();
}

void CreateAndStorePreXULSkeletonUI(HINSTANCE hInstance, int argc,
                                    char** argv) {
  auto result = CreateAndStorePreXULSkeletonUIImpl(hInstance, argc, argv);

  if (result.isErr()) {
    sErrorReason.emplace(result.unwrapErr());
  }
}

void CleanupProcessRuntime() {
  delete sProcessRuntime;
  sProcessRuntime = nullptr;
}

bool WasPreXULSkeletonUIMaximized() { return sMaximized; }

bool GetPreXULSkeletonUIWasShown() {
  return sPreXULSkeletonUIShown || !!sPreXULSkeletonUIWindow;
}

HWND ConsumePreXULSkeletonUIHandle() {
  // NOTE: we need to make sure that everything that runs here is a no-op if
  // it failed to be set, which is a possibility. If anything fails to be set
  // we don't want to clean everything up right away, because if we have a
  // blank window up, we want that to stick around and get consumed by nsWindow
  // as normal, otherwise the window will flicker in and out, which we imagine
  // is unpleasant.

  // If we don't get 1 here, it means the thread is actually just sleeping, so
  // we don't need to worry about giving out ownership of the window, because
  // the thread will simply exit after its sleep. However, if it is 1, we need
  // to wait for the thread to exit to be safe, as it could be doing anything.
  if (InterlockedIncrement(&sAnimationControlFlag) == 1) {
    ::WaitForSingleObject(sPreXULSKeletonUIAnimationThread, INFINITE);
  }
  ::CloseHandle(sPreXULSKeletonUIAnimationThread);
  sPreXULSKeletonUIAnimationThread = nullptr;
  HWND result = sPreXULSkeletonUIWindow;
  sPreXULSkeletonUIWindow = nullptr;
  free(sPixelBuffer);
  sPixelBuffer = nullptr;
  delete sAnimatedRects;
  sAnimatedRects = nullptr;

  return result;
}

Maybe<PreXULSkeletonUIError> GetPreXULSkeletonUIErrorReason() {
  return sErrorReason;
}

Result<Ok, PreXULSkeletonUIError> PersistPreXULSkeletonUIValues(
    const SkeletonUISettings& settings) {
  if (!sPreXULSkeletonUIEnabled) {
    return Err(PreXULSkeletonUIError::Disabled);
  }

  HKEY regKey;
  MOZ_TRY_VAR(regKey, OpenPreXULSkeletonUIRegKey());
  AutoCloseRegKey closeKey(regKey);

  UniquePtr<wchar_t[]> binPath;
  MOZ_TRY_VAR(binPath, GetBinaryPath());

  MOZ_TRY(WriteRegUint(regKey,
                       GetRegValueName(binPath.get(), sScreenXRegSuffix),
                       settings.screenX));
  MOZ_TRY(WriteRegUint(regKey,
                       GetRegValueName(binPath.get(), sScreenYRegSuffix),
                       settings.screenY));
  MOZ_TRY(WriteRegUint(regKey, GetRegValueName(binPath.get(), sWidthRegSuffix),
                       settings.width));
  MOZ_TRY(WriteRegUint(regKey, GetRegValueName(binPath.get(), sHeightRegSuffix),
                       settings.height));

  MOZ_TRY(WriteRegBool(regKey,
                       GetRegValueName(binPath.get(), sMaximizedRegSuffix),
                       settings.maximized));

  EnumSet<SkeletonUIFlag, uint32_t> flags;
  if (settings.menubarShown) {
    flags += SkeletonUIFlag::MenubarShown;
  }
  if (settings.bookmarksToolbarShown) {
    flags += SkeletonUIFlag::BookmarksToolbarShown;
  }
  if (settings.rtlEnabled) {
    flags += SkeletonUIFlag::RtlEnabled;
  }
  if (settings.uiDensity == SkeletonUIDensity::Touch) {
    flags += SkeletonUIFlag::TouchDensity;
  }
  if (settings.uiDensity == SkeletonUIDensity::Compact) {
    flags += SkeletonUIFlag::CompactDensity;
  }

  uint32_t flagsUint = flags.serialize();
  MOZ_TRY(WriteRegUint(regKey, GetRegValueName(binPath.get(), sFlagsRegSuffix),
                       flagsUint));

  MOZ_TRY(WriteRegDouble(
      regKey, GetRegValueName(binPath.get(), sCssToDevPixelScalingRegSuffix),
      settings.cssToDevPixelScaling));
  MOZ_TRY(WriteRegCSSPixelSpans(
      regKey, GetRegValueName(binPath.get(), sUrlbarCSSRegSuffix),
      &settings.urlbarSpan, 1));
  MOZ_TRY(WriteRegCSSPixelSpans(
      regKey, GetRegValueName(binPath.get(), sSearchbarRegSuffix),
      &settings.searchbarSpan, 1));
  MOZ_TRY(WriteRegCSSPixelSpans(
      regKey, GetRegValueName(binPath.get(), sSpringsCSSRegSuffix),
      settings.springs.begin(), settings.springs.length()));

  return Ok();
}

MFBT_API bool GetPreXULSkeletonUIEnabled() { return sPreXULSkeletonUIEnabled; }

MFBT_API Result<Ok, PreXULSkeletonUIError> SetPreXULSkeletonUIEnabledIfAllowed(
    bool value) {
  // If the pre-XUL skeleton UI was disallowed for some reason, we just want to
  // ignore changes to the registry. An example of how things could be bad if
  // we didn't: someone running firefox with the -profile argument could
  // turn the skeleton UI on or off for the default profile. Turning it off
  // maybe isn't so bad (though it's likely still incorrect), but turning it
  // on could be bad if the user had specifically disabled it for a profile for
  // some reason. Ultimately there's no correct decision here, and the
  // messiness of this is just a consequence of sharing the registry values
  // across profiles. However, whatever ill effects we observe should be
  // correct themselves after one session.
  if (PreXULSkeletonUIDisallowed()) {
    return Err(PreXULSkeletonUIError::Disabled);
  }

  HKEY regKey;
  MOZ_TRY_VAR(regKey, OpenPreXULSkeletonUIRegKey());
  AutoCloseRegKey closeKey(regKey);

  UniquePtr<wchar_t[]> binPath;
  MOZ_TRY_VAR(binPath, GetBinaryPath());
  MOZ_TRY(WriteRegBool(
      regKey, GetRegValueName(binPath.get(), sEnabledRegSuffix), value));

  if (!sPreXULSkeletonUIEnabled && value) {
    // We specifically don't care if we fail to get this lock. We just want to
    // do our best effort to lock it so that future instances don't create
    // skeleton UIs while we're still running, since they will immediately exit
    // and tell us to open a new window.
    Unused << GetSkeletonUILock();
  }

  sPreXULSkeletonUIEnabled = value;

  return Ok();
}

MFBT_API Result<Ok, PreXULSkeletonUIError> SetPreXULSkeletonUIThemeId(
    ThemeMode theme) {
  if (theme == sTheme) {
    return Ok();
  }
  sTheme = theme;

  // If we fail below, invalidate sTheme
  auto invalidateTheme = MakeScopeExit([] { sTheme = ThemeMode::Invalid; });

  HKEY regKey;
  MOZ_TRY_VAR(regKey, OpenPreXULSkeletonUIRegKey());
  AutoCloseRegKey closeKey(regKey);

  UniquePtr<wchar_t[]> binPath;
  MOZ_TRY_VAR(binPath, GetBinaryPath());
  MOZ_TRY(WriteRegUint(regKey, GetRegValueName(binPath.get(), sThemeRegSuffix),
                       static_cast<uint32_t>(theme)));

  invalidateTheme.release();
  return Ok();
}

MFBT_API void PollPreXULSkeletonUIEvents() {
  if (sPreXULSkeletonUIEnabled && sPreXULSkeletonUIWindow) {
    MSG outMsg = {};
    PeekMessageW(&outMsg, sPreXULSkeletonUIWindow, 0, 0, 0);
  }
}

Result<Ok, PreXULSkeletonUIError> NotePreXULSkeletonUIRestarting() {
  if (!sPreXULSkeletonUIEnabled) {
    return Err(PreXULSkeletonUIError::Disabled);
  }

  ::SetEnvironmentVariableW(L"MOZ_SKELETON_UI_RESTARTING", L"1");

  // We assume that we are going to exit the application very shortly after
  // this. It should thus be fine to release this lock, and we'll need to,
  // since during a restart we launch the new instance before closing this
  // one.
  if (sPreXULSKeletonUILockFile != INVALID_HANDLE_VALUE) {
    ::CloseHandle(sPreXULSKeletonUILockFile);
  }
  return Ok();
}

}  // namespace mozilla
