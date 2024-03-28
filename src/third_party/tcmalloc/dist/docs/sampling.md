# How sampling in TCMalloc works.

## Introduction

TCMalloc uses sampling to get representative data on memory usage and
allocation.

## Sampling

We chose to sample an allocation every N bytes where N is a random value using
[Sampler::PickNextSamplingPoint()](https://github.com/google/tcmalloc/blob/master/tcmalloc/sampler.cc)
with a mean set by the profile sample rate using
[MallocExtension::SetProfileSamplingRate()](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h).

By default this is every 2MiB, and can be overridden in code. Note that this is
an *statistical expectation* and it's not the case that every 2 MiB block of
memory has exactly one sampled byte.

## How We Sample Allocations

We'd like to sample each byte in memory with a uniform probability. The
granularity there is too fine; for better fast-path performance, we can use a
simple counter and sample an allocation if 1 or more bytes in it are sampled
instead. This causes a slight statistical skew; the larger the allocation, the
more likely it is that more than one byte of it should be sampled, which we
correct for, as well as the fact that requested and allocated size may be
different, in the weighting process. We defer a
[detailed treatment of the weighting process to the appendix](#weighting).

[Sampler::RecordAllocationSlow()](https://github.com/google/tcmalloc/blob/master/tcmalloc/sampler.cc)
determines when we should sample an allocation; if we should, it returns a
*weight* that indicates how many bytes that the sampled allocation represents in
expectation.

We do some additional processing around that allocation using
[SampleifyAllocation()](https://github.com/google/tcmalloc/blob/master/tcmalloc/allocation_sampling.h)
to record the call stack, alignment, request size, and allocation size. Then we
go through all the active samplers using
[ReportMalloc()](https://github.com/google/tcmalloc/blob/master/tcmalloc/allocation_sample.h)
and tell them about the allocation.

We also tell the span that we're sampling it. We can do this because we do
sampling at tcmalloc page sizes, so each sample corresponds to a particular page
in the pagemap.

For small allocations, we make two allocations: the returned allocation (which
uses an entire tcmalloc page, not shared with any other allocations) and a proxy
allocation in a non-sampled span (the proxy object is used when computing
fragmentation profiles).

When allocations are sampled, the virtual addresses associated with the
allocation are
[`madvise`d with the `MADV_NOHUGEPAGE` flag](https://github.com/google/tcmalloc/blob/master/tcmalloc/system-alloc.cc).
This, combined with the whole-page behavior above, means that *every allocation
gets its own native (OS) page(s)* shared with no other allocations.

## How We Free Sampled Objects

Each sampled allocation is tagged. Using this, we can quickly test whether a
particular allocation might be a sample.

When we are done with the sampled span we release it using
[tcmalloc::Span::Unsample()](https://github.com/google/tcmalloc/blob/master/tcmalloc/span.cc).

## How Do We Handle Heap and Fragmentation Profiling

To handle heap and fragmentation profiling we just need to traverse the list of
sampled objects and compute either their degree of fragmentation (with the proxy
object), or the amount of heap they consume.

Each allocation gets additional metadata associated with it when it is exposed
in the heap profile. In the preparation for writing the heap profile,
[MergeProfileSamplesAndMaybeGetResidencyInfo()](https://github.com/google/tcmalloc/blob/master/tcmalloc/internal/profile_builder.cc)
probes the operating system for whether or not the underlying memory pages in
the sampled allocation are swapped or not resident at all (this can happen if
they've never been written to). We use
[`/proc/pid/pagemap`](https://www.kernel.org/doc/Documentation/vm/pagemap.txt)
to obtain this information for each underlying OS page.

The OS is more aggressive at swapping out pages for sampled allocations than the
statistics might otherwise indicate. Sampled allocations do not share memory
pages (either huge or otherwise) with any other allocations, so a sampled
rarely-accessed allocation becomes eligible for reclaim more readily than an
allocation that has not been sampled (which might share pages with other
allocations that are heavily accessed). This design is a fortunate consequence
of other aspects of sampling; we want to identify specific allocations as being
more readily swapped independent of our memory allocation behavior.

More information is available via pageflags, but these require `root` to access
`/proc/kpageflags`. To make this information available to tcmalloc,
[proposed kernel changes](https://patchwork.kernel.org/project/linux-mm/list/?series=572147)
would need to be merged.

## How Do We Handle Allocation Profiling

Allocation profiling reports a list of sampled allocations during a length of
time. We start an allocation profile using
[MallocExtension::StartAllocationProfiling()](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h),
then wait until time has elapsed, then call `Stop` on the token. and report the
profile.

While the allocation sampler is active it is added to the list of samplers for
allocations and removed from the list when it is claimed.

## How Do We Handle Lifetime Profiling

Lifetime profiling reports a list of object lifetimes as pairs of allocation and
deallocation records. Profiling is initiated by calling
[MallocExtension::StartLifetimeProfiling()](https://github.com/google/tcmalloc/blob/master/tcmalloc/malloc_extension.h).
Profiling continues until `Stop` is invoked on the token. Lifetimes are only
reported for objects where allocation *and* deallocation are observed while
profiling is active. A description of the sampling based lifetime profiler can
be found in Section 4 of
["Learning-based Memory Allocation for C++ Server Workloads, ASPLOS 2020"](https://research.google/pubs/pub49008/).

## Appendix

### Detailed treatment of weighting {#weighting}

#### Sampling via a distance counter; simplification for sampled allocations.

We have one parameter, sampling period, that we denote with $$T$$ throughout.

Ideally we'd like to sample each byte of allocated memory with a constant
probability $$p = 1/T$$. The distance between each successive pair of sampled
bytes is a geometric distribution on $$\mathbb N$$; the actual code takes the
[equivalent](https://en.wikipedia.org/wiki/Exponential_distribution#Related_distributions)
ceiling of an exponentially-distributed variable with parameter $$\lambda =
1/T$$. Whenever we make an allocation, we decrement the requested size plus one
byte (we concentrate more on this in the next section; we can treat this as a
decrement of the allocated size for now) from a counter initialized to one
realization of this random variable.

If all allocations were one byte wide, this would work perfectly. Each sampled
byte would represent $$T$$ bytes in the total, and each byte of memory would be
uniformly likely to be sampled. Unfortunately, allocations are typically more
than one byte wide, so we will need to compensate.

Take an allocation that has been chosen to be sampled (of size $$k$$). The byte
marked `*` is the byte that decreases the counter to zero; the counter starts at
$$f$$ before this allocation.

$$
\underbrace{
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{*}
}_\text{$f$}
\underbrace{
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{\phantom{*}}
\boxed{\phantom{*}}
}_\text{$k-f$}
$$

The *sampling weight* $$W$$ (n.b. not `allocation count estimate`, which is
reported as `weight` in `MallocHook::SampledAlloc`) of this allocation is the
number of bytes represented by this sample. The byte marked `*` contributes
$$T$$ bytes, as that is the average sampling rate; the bytes before the sampled
byte were specifically not sampled and don't contribute to the sampling weight.

The $$k-f$$ bytes after `*` have some probability of being sampled.
Specifically, each byte continues to have a $$1/T$$ probability of being
sampled, which means that a remaining $$X$$ bytes would be sampled if we had
continued our technique, where

$$
X \sim \mathrm{Pois}\left(\frac{k-f}{T}\right),
$$

which follows by applying the definition of the Poisson distribution. So each
sampled allocation has a weight of

$$
W = T + TX.
$$

Instead of realizing an instantiation of $$X$$, which is computationally
cumbersome, we simplify in this case by taking $$X$$ to be its expectation:

$$
W = T + T\hat X = T + T \left(\frac{k - f}{T}\right) = T + k - f.
$$

Now let's consider our estimate on the total memory usage

$$
M = \sum_i W_i
$$

where the sum ranges over all sampled allocations. In either the world where we
did not make the simplifying assumption or in the limit where all allocations
are one byte, there is no $$k-f$$ part to worry about; the variance is just that
of the underlying Poisson process, which is

$$\sigma^2 = \lambda = TM.$$

In the other direction with one gigantic allocation $$k = M \gg T$$, the
variance is just on a single realization of our geometric random variable
representing the distance between sampled allocations, which is

$$\sigma^2 = \frac{1-p}{p^2} = \frac{1 - \frac1T}{\frac{1}{T^2}} = T^2 - T.$$

Thus, depending on allocation pattern, the variance for our estimate on $$M$$
varies between $$T^2 - T$$ and $$TM$$.

#### Requested memory size versus actual allocation size.

To keep the fast path fast, all of the above logic works on requested size plus
one byte, not actual allocation sizes. For larger allocations (as of the time of
writing, greater than 262144 bytes), these two values differ by a byte; for
smaller allocations, allocations are
[bucketed](https://github.com/google/tcmalloc/blob/master/tcmalloc/size_classes.cc)
into a set of discrete size classes for optimal cache performance.

For the smallest allocation, zero bytes, we run into an issue: we actually *do*
allocate memory (we round it up to the smallest class size), so we need to
ensure that there is some chance of sampling even zero byte allocations. We
increment all requested sizes by one in order to deal with this and we come up
with the final term of "requested plus one."

When we choose an allocation to be sampled, we report an allocation estimate,
which is called `weight` in `MallocHook::SampledAlloc`; this variable means "how
many allocations this sample represents." We take the sampling weight $$W$$ from
the previous section and divide by the actual requested size plus one (not the
allocated size). Thus the estimate on the number of bytes a sampled allocation
represents is

$$\frac{a}{r+1} W$$

where $$r$$ is the requested size, $$a$$ is the actual allocated size, and $$W$$
is the allocation weight from the previous section.

For an intuitive sense of how this works, suppose we only have 1-byte requested
allocations (which are currently rounded up to 8-byte actual allocations). Each
allocation decrements the distance counter from the previous section by 2 bytes
($$r+1$$) instead of the true value of 8 bytes. When an allocation is chosen to
be sampled, we multiply by the same unskewing ratio (4, which is the allocated
size, 8, over the requested size plus one, 2).

Essentially, what we are doing here is actually sampling slightly less than the
requested sampling rate, at a variable rate depending on the
requested-to-allocated ratio. This will increase the variance on allocations
geometrically far from the next size class, but does not change the underlying
expectation of the distribution; the memorylessness nature of the distribution
means that no allocation pattern can result in over- or under- reporting of
allocations that differ significantly in requested-to-allocated.

In the extreme case (all allocations are much smaller than the smallest size
class), this approach becomes identical to a "per-allocation" sampling strategy.
A per-allocation sampling strategy results in a higher variance for larger
allocations; here, we note that our actual distribution of size classes covers
any larger allocations and the variance term can only increase by a small
amount.
