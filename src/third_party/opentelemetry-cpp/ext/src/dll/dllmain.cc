// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <windows.h>  // NOLINT

// Include API header files here for exporting
#include <opentelemetry/trace/provider.h>

#include <opentelemetry/logs/logger_provider.h>
#include <opentelemetry/logs/provider.h>

extern "C" BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{
  UNREFERENCED_PARAMETER(hInstance);
  UNREFERENCED_PARAMETER(dwReason);
  UNREFERENCED_PARAMETER(lpReserved);

  return TRUE;
}
