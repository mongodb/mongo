# GWP-ASan

GWP-ASan is a low-overhead sampling-based utility for finding
heap-use-after-frees and heap-buffer-overflows in production.
GWP-ASan is a recursive acronym: "**G**WP-ASan **W**ill **P**rovide
**A**llocation **San**ity".

## Why not just use ASan?

For many cases you **should** use [ASan](https://clang.llvm.org/docs/AddressSanitizer.html)
(e.g., on your tests). However, ASan comes with average execution slowdown of 2x
(compared to `-O2`), binary size increase of 2x, and significant memory
overhead. For these reasons, ASan is generally impractical for use in production
(other than in dedicated canaries). GWP-ASan is a minimal-overhead alternative
designed for widespread use in production.

## How to use GWP-ASan

You can enable GWP-ASan by calling `tcmalloc::MallocExtension::ActivateGuardedSampling()`.
To adjust GWP-ASan's sampling rate, see
[below](#what-should-i-set-the-sampling-rate-to).

When GWP-ASan detects a heap memory error, it prints stack traces for the point
of the memory error, as well as the points where the memory was allocated and
(if applicable) freed. These stack traces can then be
symbolized offline to get file names and line
numbers.

GWP-ASan will crash after printing stack traces.

## CPU and RAM Overhead

For guarded sampling rates above 100M (the default), CPU overhead is negligible. For sampling rates as low as 8M, CPU overhead is under 0.5%.

RAM overhead is up to 512 KB on x86\_64, or 4 MB on PowerPC.

## What should I set the sampling rate to?

`tcmalloc::MallocExtension::SetGuardedSamplingRate` sets the sampling rate for
GWP-ASan. GWP-ASan will guard allocations approximately every
`GuardedSamplingRate` bytes allocated. Thus, lower values will generally
increase the the chance of finding bugs but will also have higher CPU overhead.

For applications that cannot tolerate any CPU overhead, we recommend
using TCMalloc's default sampling rate.  If your application can tolerate some
CPU overhead, we recommend a sampling rate of 8MB.

## Limitations

-   The current version of GWP-ASan will only find bugs in allocations of 8 KB
    or less. This restriction was made to limit the CPU/RAM overhead required by
    GWP-ASan.

-   GWP-ASan has limited diagnostic information for buffer overflows within
    alignment padding, since overflows of this type will not touch a guard
    page. For write-overflows,
    GWP-ASan will still be able to detect the overflow during deallocation by
    checking whether magic bytes have been overwritten, but the stack trace of
    the overflow itself will not be available.

## FAQs

### Does GWP-ASan report false positives?

No. GWP-ASan crashes because your program accessed unmapped memory, which is
always a true bug, or a sign of hardware failure (see below).

### How do I know a GWP-ASan report isn't caused by hardware failure?

The vast majority of GWP-ASan reports we see are true bugs, but occasionally
faulty hardware will be the actual cause of the crash. In general, if you see
the same GWP-ASan crash on multiple machines, it is very likely there's a true
software bug.

### Can GWP-ASan cause queries of death (QoD) in my production?

Since GWP-ASan finds bugs with very low probability, QoD is generally not a
concern. Even if there is a reliable way to trigger a bug, GWP-ASan will only
detect it and crash on a tiny fraction of actual occurrences, allowing the other
99.9% to continue without crashing.

## Other versions of GWP-ASan

Separate implementations of GWP-ASan exist for Chromium and Android. For
GWP-ASan for Chromium see
[here](https://chromium.googlesource.com/chromium/src/+/lkgr/docs/gwp_asan.md).
For Android, see [here](https://developer.android.com/ndk/guides/gwp-asan).
