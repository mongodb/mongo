/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_LinuxSignal_h
#define mozilla_LinuxSignal_h

namespace mozilla {

#if defined(__arm__)

// Some (old) Linux kernels on ARM have a bug where a signal handler
// can be called without clearing the IT bits in CPSR first. The result
// is that the first few instructions of the handler could be skipped,
// ultimately resulting in crashes. To workaround this bug, the handler
// on ARM is a trampoline that starts with enough NOP instructions, so
// that even if the IT bits are not cleared, only the NOP instructions
// will be skipped over.

template <void (*H)(int, siginfo_t*, void*)>
__attribute__((naked)) void
SignalTrampoline(int aSignal, siginfo_t* aInfo, void* aContext)
{
  asm volatile (
    "nop; nop; nop; nop"
    : : : "memory");

  asm volatile (
    "b %0"
    :
    : "X"(H)
    : "memory");
}

# define MOZ_SIGNAL_TRAMPOLINE(h) (mozilla::SignalTrampoline<h>)

#else // __arm__

# define MOZ_SIGNAL_TRAMPOLINE(h) (h)

#endif // __arm__

} // namespace mozilla

#endif // mozilla_LinuxSignal_h
