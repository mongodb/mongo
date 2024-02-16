# Understanding Malloc Stats

## Getting Malloc Stats

Human-readable statistics can be obtained by calling
`tcmalloc::MallocExtension::GetStats()`.

## Understanding Malloc Stats Output

### It's A Lot Of Information

The output contains a lot of information. Much of it can be considered debug
info that's interesting to folks who are passingly familiar with the internals
of TCMalloc, but potentially not that useful for most people.

### Summary Section

The most generally useful section is the first few lines:

```
See https://github.com/google/tcmalloc/tree/master/docs/stats.md for an explanation of this page
------------------------------------------------
MALLOC:    10858234672 (10355.2 MiB) Bytes in use by application
MALLOC: +    827129856 (  788.8 MiB) Bytes in page heap freelist
MALLOC: +    386098400 (  368.2 MiB) Bytes in central cache freelist
MALLOC: +    105330688 (  100.5 MiB) Bytes in per-CPU cache freelist
MALLOC: +      9095680 (    8.7 MiB) Bytes in transfer cache freelist
MALLOC: +       660976 (    0.6 MiB) Bytes in thread cache freelists
MALLOC: +     49333930 (   47.0 MiB) Bytes in malloc metadata
MALLOC: +       629440 (    0.6 MiB) Bytes in malloc metadata Arena unallocated
MALLOC: +      1599704 (    1.5 MiB) Bytes in malloc metadata Arena unavailable
MALLOC:   ------------
MALLOC: =  12238113346 (11671.2 MiB) Actual memory used (physical + swap)
MALLOC: +    704643072 (  672.0 MiB) Bytes released to OS (aka unmapped)
MALLOC:   ------------
MALLOC: =  12942756418 (12343.2 MiB) Virtual address space used
```

*   **Bytes in use by application:** Number of bytes that the application is
    actively using to hold data. This is computed by the bytes requested from
    the OS minus any bytes that are held in caches and other internal data
    structures.
*   **Bytes in page heap freelist:** The pageheap is a structure that holds
    memory ready for TCMalloc to use. This memory is not actively being used,
    and could be returned to the OS. [See TCMalloc tuning](tuning.md)
*   **Bytes in central cache freelist:** This is the amount of memory currently
    held in the central freelist. This is a structure that holds partially used
    "[spans](#more-detail-on-metadata)" of memory. The spans are partially used
    because some memory has been allocated from them, but not entirely used -
    since they have some free memory on them.
*   **Bytes in per-CPU cache freelist:** In per-cpu mode (which is the default)
    each CPU holds some memory ready to quickly hand to the application. The
    maximum size of this per-cpu cache is tunable.
    [See TCMalloc tuning](tuning.md)
*   **Bytes in transfer cache freelist:** The transfer cache can be considered
    another part of the central freelist. It holds memory that is ready to be
    provided to the application for use.
*   **Bytes in thread cache freelists:** The TC in TCMalloc stands for thread
    cache. Originally each thread held its own cache of memory to provide to the
    application. Since the change of default to per-cpu caches, the thread
    caches are used by very few applications. However, TCMalloc starts in
    per-thread mode, so there may be some memory left in per-thread caches from
    before it switches into per-cpu mode.
*   **Bytes in malloc metadata:** the size of the data structures used for
    tracking memory allocation. This will grow as the amount of memory used
    grows.
*   **Bytes in malloc metadata Arena unallocated:** Metadata is allocated in an
    internal Arena. Memory requests to the OS are made in blocks which amortize
    several Arena allocations and this captures memory that is not yet allocated
    but could be by future Arena allocations.
*   **Bytes in malloc metadata Arena unavailable:** The Arena allocator may fail
    to allocate a block fully when a subsequent Arena allocation request is made
    that is larger than the block's remaining space. This memory is currently
    unavailable for allocation.

There's a couple of summary lines:

*   **Actual memory used:** This is the total amount of memory that TCMalloc
    thinks it is using in the various categories. This is computed from the size
    of the various areas, the actual contribution to RSS may be larger or
    smaller than this value. The true RSS may be less if memory is not mapped
    in. In some cases RSS can be larger if small regions end up being mapped
    with huge pages. This does not count memory that TCMalloc is not aware of
    (eg memory mapped files, text segments etc.)
*   **Bytes released to OS:** TCMalloc can release memory back to the OS (see
    [tcmalloc tuning](tuning.md)), and this is the upper bound on the amount of
    released memory. However, it is up to the OS as to whether the act of
    releasing the memory actually reduces the RSS of the application. The code
    uses `MADV_DONTNEED`/`MADV_REMOVE` which tells the OS that the memory is no
    longer needed.
*   **Virtual address space used:** This is the amount of virtual address space
    that TCMalloc believes it is using. This should match the later section on
    requested memory. There are other ways that an application can increase its
    virtual address space, and this statistic does not capture them.

### More Detail On Metadata

The next section gives some insight into the amount of metadata that TCMalloc is
using. This is really debug information, and not very actionable.

```
MALLOC:         236176               Spans in use
MALLOC:         238709 (   10.9 MiB) Spans created
MALLOC:              8               Thread heaps in use
MALLOC:             46 (    0.0 MiB) Thread heaps created
MALLOC:          13517               Stack traces in use
MALLOC:          13742 (    7.2 MiB) Stack traces created
MALLOC:              0               Table buckets in use
MALLOC:           2808 (    0.0 MiB) Table buckets created
MALLOC:       11665416 (   11.1 MiB) Pagemap bytes used
MALLOC:        4067336 (    3.9 MiB) Pagemap root resident bytes
```

*   **Spans:** structures that hold multiple [pages](#page-sizes) of allocatable
    objects.
*   **Thread heaps:** These are the per-thread structures used in per-thread
    mode.
*   **Stack traces:** These hold metadata for each sampled object.
*   **Table buckets:** These hold data for stack traces for sampled events.
*   **Pagemap:** This data structure supports the mapping of object addresses to
    information about the objects held on the page. The pagemap root is a
    potentially large array, and it is useful to know how much of it is actually
    memory resident.

### Realized Fragmentation

```
MALLOC:    12238113346 (11671.2 MiB) Actual memory used at peak
MALLOC:    11626207678 (11087.6 MiB) Estimated in-use at peak
MALLOC:              5.2632          Realized fragmentation (%)
```

Memory overhead at peak demand is more important than off-peak, since we need to
provision a process with sufficient memory to run during its peak requirements
without OOM'ing. After a peak in demand, memory may be deallocated and held in
caches in anticipation of future reuse. Overhead as a fraction of the remaining
live allocations rises, but no additional memory is required.

This metric is called "realized fragmentation" and described in ["Adaptive
Hugepage Subrelease for Non-moving Memory Allocators in Warehouse-Scale
Computers"](https://research.google/pubs/pub50436/) (ISMM 2021). The realized
fragmentation metric computed here is a snapshot over the life of the entire
process.

These realized fragmentation stats in the summary table indicate a snapshot of
conditions when TCMalloc used a peak in its physical memory. As of April 2022,
the in-use at peak number is estimated from TCMalloc's periodic allocation
sampling.

### Page Sizes

There are three relevant "page" sizes for systems and TCMalloc. It's important
to be able to disambiguate them.

*   **System default page size:** this is not reported by TCMalloc. This is 4KiB
    on x86. It's not referred to in TCMalloc, and it's not important, but it's
    important to know that it is different from the sizes of pages used in
    TCMalloc.
*   **TCMalloc page size:** This is the basic unit of memory management for
    TCMalloc. Objects on the same page are the same number of bytes in size.
    Internally TCMalloc manages memory in chunks of this size. TCMalloc supports
    4 sizes: 4KiB (small but slow), 8KiB (the default), 32 KiB (large), 256 KiB
    (256 KiB pages). There are trade-offs around the page sizes:
    *   Smaller page sizes are more memory efficient because we have less
        fragmentation (ie left over space) when trying to provide the requested
        amount of memory using 4KiB chunks. It's also more likely that all the
        objects on a 4KiB page will be freed allowing the page to be returned
        and used for a different size of data.
    *   Larger pages result in fewer fetches from the page heap to provide a
        given amount of memory. They also keep allocated objects of the same
        size in closer proximity.
*   **TCMalloc hugepage size:** This is the size of a hugepage on the system,
    for x86 this is 2MiB. This size is used as a unit of management by
    temeriare, but not used by the pre-temeraire pageheap.

```
MALLOC:          32768               Tcmalloc page size
MALLOC:        2097152               Tcmalloc hugepage size
```

### Experiments

There is an experiment framework embedded into TCMalloc.
The enabled experiments are reported as part of the statistics.

```
MALLOC EXPERIMENTS: TCMALLOC_TEMERAIRE=0 TCMALLOC_TEMERAIRE_WITH_SUBRELEASE_V3=0
```

### Actual Memory Footprint

The output also reports the memory size information recorded by the OS:

*   Bytes resident is the amount of physical memory in use by the application
    (RSS). This includes things like program text which is excluded from the
    information that TCMalloc presents.
*   Bytes mapped is the size of the virtual address space in use by the
    application (VSS). This can be substantially larger than the virtual memory
    reported by TCMalloc as applications can increase VSS in other ways. It's
    also not that useful as a metric since the VSS is a limit to the RSS, but
    not directly related to the amount of physical memory that the application
    uses.

```
Total process stats (inclusive of non-malloc sources):
TOTAL:  86880677888 (82855.9 MiB) Bytes resident (physical memory used)
TOTAL:  89124790272 (84996.0 MiB) Bytes mapped (virtual memory used)
```

### Per Size-Class Information

Requests for memory are rounded to convenient sizes. For example a request for
15 bytes could be rounded to 16 bytes. These sizes are referred to as class
sizes. There are various caches in TCMalloc where memory gets held, and the per
size-class section reports how much memory is being used by cached objects of
each size. The columns reported for each size-class are:

*   The size of each object in that size-class.
*   The number of objects of that size currently held in the per-cpu,
    per-thread, transfer, and central caches.
*   The total size of those objects in MiB - ie size of each object multiplied
    by the number of objects.
*   The cumulative size of that size-class plus all smaller size-classes.
*   The number of live pages dedicated to this size-class.
*   The number of returned and requested spans of this size-class.

```
Total size of freelists for per-thread and per-CPU caches,
transfer cache, and central cache, as well as number of
live pages, returned/requested spans by size-class
------------------------------------------------
class   1 [        8 bytes ] :    45645 objs;   0.3 MiB;   0.3 cum MiB;       73 live pages; spans:     19 ret /     92 req = 0.2065;
class   2 [       16 bytes ] :    39942 objs;   0.6 MiB;   1.0 cum MiB;      120 live pages; spans:      3 ret /    123 req = 0.0244;
class   3 [       24 bytes ] :    84130 objs;   1.9 MiB;   2.9 cum MiB;      807 live pages; spans:   1330 ret /   2137 req = 0.6224;
class   4 [       32 bytes ] :   107271 objs;   3.3 MiB;   6.2 cum MiB;     1048 live pages; spans:    420 ret /   1468 req = 0.2861;
class   5 [       40 bytes ] :    82230 objs;   3.1 MiB;   9.3 cum MiB;      790 live pages; spans:    962 ret /   1752 req = 0.5491;
...
```

### Central Cache Free List Span Utilization

Central cache free list manages memory in spans, where each span is a collection
of one or more TCMalloc pages. We track histogram of span utilization, where
each column refers to the number of spans with allocated objects less than N.

```
------------------------------------------------
Central cache freelist: Span utilization histogram
Non-cumulative number of spans with allocated objects < N
------------------------------------------------
class   1 [        8 bytes ] :      0 < 1,     0 < 2,     0 < 4,     0 < 8,     0 < 16,     1 < 32,     0 < 64,     1 < 128,     1 < 256,     1 < 512,     0 < 1024,     0 < 2048,     4 < 4096,    16 < 8192,     0 < 16384,     0 < 32768,     0 < 65536
class   2 [       16 bytes ] :      0 < 1,     0 < 2,     0 < 4,     0 < 8,     0 < 16,     0 < 32,     0 < 64,     0 < 128,     0 < 256,     0 < 512,     1 < 1024,     0 < 2048,    47 < 4096,     0 < 8192,     0 < 16384,     0 < 32768,     0 < 65536
class   3 [       24 bytes ] :      0 < 1,     0 < 2,     0 < 4,     0 < 8,     0 < 16,     0 < 32,     0 < 64,     2 < 128,     1 < 256,     3 < 512,     5 < 1024,   127 < 2048,     0 < 4096,     0 < 8192,     0 < 16384,     0 < 32768,     0 < 65536
class   4 [       32 bytes ] :      0 < 1,     0 < 2,     0 < 4,     0 < 8,     0 < 16,     0 < 32,     0 < 64,     0 < 128,     0 < 256,     1 < 512,     0 < 1024,   129 < 2048,     0 < 4096,     0 < 8192,     0 < 16384,     0 < 32768,     0 < 65536
class   5 [       40 bytes ] :      0 < 1,     1 < 2,     1 < 4,     0 < 8,     0 < 16,     0 < 32,     1 < 64,     1 < 128,     4 < 256,     5 < 512,    80 < 1024,     0 < 2048,     0 < 4096,     0 < 8192,     0 < 16384,     0 < 32768,     0 < 65536
...
```

### Transfer Cache Information

Transfer cache is used by TCMalloc, before going to central free list. For each
size-class, we track and report the following statistics:

*   The size of each object in that size-class.
*   The number of objects of that size currently held in the transfer cache.
*   The total size of those objects in MiB - i.e. size of each object multiplied
    by the number of objects in the freelist.
*   The cumulative size of that size-class plus all smaller size-classes.
*   The current capacity of the freelist.
*   The maximum capacity to which the freelist is allowed to grow.
*   The number of hits observed during inserts to the transfer cache.
*   The total number batched and non-batched misses observed during insert
    operations.
*   The number of partial (i.e. non-batch-sized) misses observed during insert
    operations.
*   The number of hits observed during removes from the transfer cache.
*   The total number batched and non-batched misses observed during remove
    operations.
*   The number of partial (i.e. non-batch-sized) misses observed during remove
    operations.

```
------------------------------------------------
Used bytes, current capacity, and maximum allowed capacity
of the transfer cache freelists.
It also reports insert/remove hits/misses by size class.
------------------------------------------------
class   1 [        8 bytes ] :     1472 objs;   0.0 MiB;    0.0 cum MiB;  2048 capacity;  2048 max_capacity;      935 insert hits;     8543 insert misses (    4507 partial);      889 remove hits;     6612 remove misses (      86 partial);
class   2 [       16 bytes ] :      608 objs;   0.0 MiB;    0.0 cum MiB;  2048 capacity;  2048 max_capacity;      575 insert hits;     3739 insert misses (    3602 partial);      556 remove hits;     3368 remove misses (      70 partial);
class   3 [       24 bytes ] :      864 objs;   0.0 MiB;    0.0 cum MiB;  2048 capacity;  2048 max_capacity;     1533 insert hits;    15594 insert misses (    9417 partial);     1506 remove hits;    11939 remove misses (      74 partial);
class   4 [       32 bytes ] :       96 objs;   0.0 MiB;    0.0 cum MiB;  2048 capacity;  2048 max_capacity;     1065 insert hits;    21772 insert misses (   19918 partial);     1061 remove hits;     6403 remove misses (     119 partial);
class   5 [       40 bytes ] :     1408 objs;   0.1 MiB;    0.1 cum MiB;  2048 capacity;  2048 max_capacity;     1475 insert hits;    16018 insert misses (   14943 partial);     1431 remove hits;     3293 remove misses (      60 partial);
class   6 [       48 bytes ] :     1664 objs;   0.1 MiB;    0.2 cum MiB;  2048 capacity;  2048 max_capacity;     1213 insert hits;    39140 insert misses (   37096 partial);     1160 remove hits;     5909 remove misses (      80 partial);
class   7 [       56 bytes ] :     1792 objs;   0.1 MiB;    0.3 cum MiB;  2048 capacity;  2048 max_capacity;      466 insert hits;      650 insert misses (     375 partial);      410 remove hits;     1264 remove misses (      55 partial);
class   8 [       64 bytes ] :     1408 objs;   0.1 MiB;    0.4 cum MiB;  2048 capacity;  2048 max_capacity;     2181 insert hits;     8816 insert misses (    8069 partial);     2137 remove hits;     2024 remove misses (      74 partial);
class   9 [       72 bytes ] :      960 objs;   0.1 MiB;    0.4 cum MiB;  1600 capacity;  2048 max_capacity;      104 insert hits;      463 insert misses (     463 partial);       74 remove hits;      287 remove misses (      62 partial);
class  10 [       80 bytes ] :     1056 objs;   0.1 MiB;    0.5 cum MiB;  2048 capacity;  2048 max_capacity;      372 insert hits;     3334 insert misses (    3287 partial);      339 remove hits;      562 remove misses (      80 partial);
...
```

As of July 2021, the `TransferCache` misses when inserting or removing a
non-batch size number of objects from the cache. These are reflected in the
"partial" column. The insert and remove miss column is *inclusive* of misses for
both batch size and non-batch size numbers of objects.

### Per-CPU Information

If the per-cpu cache is enabled then we get a report of the memory currently
being cached on each CPU.

The first number reported is the maximum size of the per-cpu cache on each CPU.
This corresponds to the parameter `MallocExtension::GetMaxPerCpuCacheSize()`,
which defaults to 1.5MiB. [See tuning](tuning.md)

The following columns are reported for each CPU:

*   The cpu ID
*   The total size of the objects held in the CPU's cache in bytes.
*   The total size of the objects held in the CPU's cache in MiB.
*   The total number of unallocated bytes.

The concept of unallocated bytes needs to be explained because the definition is
not obvious.

The per-cpu cache is an array of pointers to available memory. Each size-class
has a number of entries that it can use in the array. These entries can be used
to hold memory, or be empty.

To control the maximum memory that the per-cpu cache can use we sum up the
number of slots that can be used by a size-class multiplied by the size of
objects in that size-class. This gives us the total memory that could be held in
the cache. This is not what is reported by unallocated memory.

Unallocated memory is the amount of memory left over from the per cpu limit
after we have subtracted the total memory that could be held in the cache.

The in use memory is calculated from the sum of the number of populated entries
in the per-cpu array multiplied by the size of the objects held in those
entries.

To summarise, the per-cpu limit (which is reported before the per-cpu data) is
equal to the number of bytes in use (which is reported in the second column)
plus the number of bytes that could be used (which is not reported) plus the
unallocated "spare" bytes (which is reported as the last column).

```
Bytes in per-CPU caches (per cpu limit: 3145728 bytes)
------------------------------------------------
cpu   0:      2168200 bytes (    2.1 MiB) with       52536 bytes unallocated active
cpu   1:      1734880 bytes (    1.7 MiB) with      258944 bytes unallocated active
cpu   2:      1779352 bytes (    1.7 MiB) with        8384 bytes unallocated active
cpu   3:      1414224 bytes (    1.3 MiB) with      112432 bytes unallocated active
cpu   4:      1260016 bytes (    1.2 MiB) with      179800 bytes unallocated
...
```

Some CPU caches may be marked `active`, indicating that the process is currently
runnable on that CPU.

### Size Class Capacity Information in Per-CPU Caches

In per-CPU caches, TCMalloc caches objects of discrete sizes. These are referred
to as size classes. Memory requests for a particular object size are rounded off
to a convenient size class. TCMalloc populates objects in each size class based
on their demand, but also imposes an upper limit on the number of objects that
may be cached per size class. The statistics below measure the capacity of each
size class freelist, where capacity represents the total number of objects
currently cached by the freelist. The columns below report number of objects
cached by TCMalloc per size class:

*   Size class.
*   The size of each object in that size class.
*   Minimum capacity of the size class freelist summarized over all per-CPU
    caches.
*   Average capacity of the size class freelist summarized over all per-CPU
    caches.
*   Maximum capacity of the size class freelist summarized over all per-CPU
    caches.
*   The upper limit imposed by TCMalloc on the number of objects that can be
    cached in a per-CPU cache for that size class.

```
------------------------------------------------
Size class capacity statistics in per-cpu caches
------------------------------------------------
class   0 [        0 bytes ] :      0 (minimum),    0.0 (average),     0 (maximum),     0 maximum allowed capacity
class   1 [        8 bytes ] :      0 (minimum),  133.1 (average),   636 (maximum),  2048 maximum allowed capacity
class   2 [       16 bytes ] :      0 (minimum),   51.8 (average),   378 (maximum),  2048 maximum allowed capacity
class   3 [       24 bytes ] :      0 (minimum),  119.3 (average),   510 (maximum),  2048 maximum allowed capacity
class   4 [       32 bytes ] :      0 (minimum),  100.0 (average),   542 (maximum),  2048 maximum allowed capacity
class   5 [       40 bytes ] :      0 (minimum),   80.6 (average),   467 (maximum),  2048 maximum allowed capacity
```

### Number of per-CPU cache underflows, overflows, and reclaims

We also keep track of cache miss counts. Underflows are when the user allocates
and the cache does not have any pointers to return. Overflows are when the user
deallocates and the cache is full. The ratio of overflows to underflows gives a
rough indication of whether the cache is large enough. If the cache had infinite
capacity, then we would expect to have 0 overflows whereas if the cache had 0
capacity, we would expect to see roughly equal numbers of overflows and
underflows. Therefore, if the ratio is close to 1.0, then the cache may not be
large enough. Reclaims are when we empty out a cache for a specific CPU because
it has been idle for a period of time. In this section, we report the total
numbers of each of these metrics across all CPUs as well as the numbers for each
individual CPU.

```
------------------------------------------------
Number of per-CPU cache underflows, overflows, and reclaims
------------------------------------------------
Total  :         242 underflows,          12 overflows, overflows / underflows:  0.05,          168 reclaims
cpu   0:          69 underflows,           5 overflows, overflows / underflows:  0.07,           46 reclaims
cpu   1:          58 underflows,           0 overflows, overflows / underflows:  0.00,           42 reclaims
cpu   2:          62 underflows,           7 overflows, overflows / underflows:  0.11,           42 reclaims
cpu   3:          40 underflows,           0 overflows, overflows / underflows:  0.00,           27 reclaims
cpu   4:          13 underflows,           0 overflows, overflows / underflows:  0.00,           11 reclaims
cpu   5:           0 underflows,           0 overflows, overflows / underflows:  0.00,            0 reclaims
```

### Pageheap Information

The pageheap holds pages of memory that are not currently being used either by
the application or by TCMalloc's internal caches. These pages are grouped into
spans - which are ranges of contiguous pages, and these spans can be either
mapped (backed by physical memory) or unmapped (not necessarily backed by
physical memory).

Memory from the pageheap is used either to replenish the per-thread or per-cpu
caches, or to directly satisfy requests that are larger than the sizes supported
by the per-thread or per-cpu caches.

**Note:** TCMalloc cannot tell whether a span of memory is actually backed by
physical memory, but it uses *unmapped* to indicate that it has told the OS that
the span is not used and does not need the associated physical memory. For this
reason the physical memory of an application may be larger that the amount that
TCMalloc reports.

The pageheap section contains the following information:

*   The first line reports the number of sizes of spans, the total memory that
    these spans cover, and the total amount of that memory that is unmapped.
*   The size of the span in number of pages.
*   The number of spans of that size.
*   The total memory consumed by those spans in MiB.
*   The cumulative total memory held in spans of that size and fewer pages.
*   The amount of that memory that has been unmapped.
*   The cumulative amount of unmapped memory for spans of that size and smaller.

```
PageHeap: 30 sizes;  480.1 MiB free;  318.4 MiB unmapped
------------------------------------------------
  1 pages *  341 spans ~  10.7 MiB;   10.7 MiB cum; unmapped:    1.9 MiB;    1.9 MiB cum
  2 pages *  469 spans ~  29.3 MiB;   40.0 MiB cum; unmapped:    0.0 MiB;    1.9 MiB cum
  3 pages *  462 spans ~  43.3 MiB;   83.3 MiB cum; unmapped:    3.3 MiB;    5.2 MiB cum
  4 pages *  119 spans ~  14.9 MiB;   98.2 MiB cum; unmapped:    0.1 MiB;    5.3 MiB cum
...
```

### Pageheap Cache Age

The next section gives some indication of the age of the various spans in the
pageheap. Live (ie backed by physical memory) and unmapped spans are reported
separately.

The columns indicate roughly how long the span has been in the pageheap, ranging
from less than a second to more than 8 hours.

```
------------------------------------------------
PageHeap cache entry age (count of pages in spans of a given size that have been idle for up to the given period of time)
------------------------------------------------
                            mean     <1s      1s     30s      1m     30m      1h     8+h
Live span     TOTAL PAGES:   9.1     533   13322      26    1483       0       0       0
Live span,        1 pages:   7.4       0     256       0      24       0       0       0
Live span,        2 pages:   1.6      38     900       0       0       0       0       0
…
Unmapped span TOTAL PAGES: 153.9     153    2245    1801    5991       0       0       0
Unmapped span,    1 pages:  34.6       0      35      15      11       0       0       0
Unmapped span,    3 pages:  28.4       0      60      42       3       0       0       0
...
```

### Pageheap Allocation Summary

This reports some stats on the number of pages allocated.

*   The number of live (i.e., not on page heap) pages that were "small"
    allocations. Small allocations are ones that are tracked in the pageheap by
    size (e.g., a region of two pages in size). Larger allocations are just kept
    in an array that has to be scanned linearly.
*   The pages of slack result from situations where allocation is rounded up to
    hugepages, and this leaves some spare pages.
*   The largest seen allocation is self explanatory.

```
PageHeap: stats on allocation sizes
PageHeap: 344420 pages live small allocation
PageHeap: 12982 pages of slack on large allocations
PageHeap: largest seen allocation 29184 pages
```

### Pageheap Per Number Of Pages In Range

This starts off reporting the activity for small ranges of pages, but at the end
of the list starts aggregating information for groups of page ranges.

*   The first column contains the number of pages (or the range of pages if the
    bucket is wider than a single page).
*   The second and third columns are the number of allocated and freed pages we
    have seen of this size.
*   The fourth column is the number of live allocations of this size.
*   The fifth column is the size of those live allocations in MiB.
*   The sixth column is the allocation rate in pages per second since the start
    of the application.
*   The seventh column is the allocation rate in MiB per second since the start
    of the application.

```
PageHeap: per-size information:
PageHeap: 1 page info: 23978897 / 23762891 a/f, 216006 (6750.2 MiB) live, 2.43e+03 allocs/s ( 76.1 MiB/s)
PageHeap: 2 page info: 21442844 / 21436331 a/f,   6513 ( 407.1 MiB) live, 2.18e+03 allocs/s (136.0 MiB/s)
PageHeap: 3 page info:  2333686 /  2329225 a/f,   4461 ( 418.2 MiB) live,      237 allocs/s ( 22.2 MiB/s)
PageHeap: 4 page info: 21509168 / 21508751 a/f,    417 (  52.1 MiB) live, 2.18e+03 allocs/s (272.9 MiB/s)
PageHeap: 5 page info:  3356076 /  3354188 a/f,   1888 ( 295.0 MiB) live,      341 allocs/s ( 53.2 MiB/s)
PageHeap: 6 page info:  1718534 /  1718486 a/f,     48 (   9.0 MiB) live,      174 allocs/s ( 32.7 MiB/s)
...
```

### GWP-ASan Status

The GWP-ASan section displays information about allocations guarded by
[GWP-ASan](gwp-asan.md).

*   The number of successful and failed GWP-ASan allocations. If there are 0
    successful and 0 failed allocations, GWP-ASan is probably disabled on your
    binary. If there are a large number of failed allocations, it probably means
    your sampling rate is too high, causing the guarded slots to be exhausted.
    See
    [GWP-ASan sampling rate](gwp-asan.md#what-should-i-set-the-sampling-rate-to).
*   The number of "slots" currently allocated and quarantined. An allocated slot
    contains an allocation that is still active (i.e., not freed) while a
    quarantined slot has either not been used yet or contains an allocation that
    was freed.
*   The maximum number of slots that have been allocated at the same time. This
    number is printed along with the allocated slot limit. If the maximum slots
    allocated matches the limit, you may want to reduce your sampling rate to
    avoid failed GWP-ASan allocations.

```
------------------------------------------------
GWP-ASan Status
------------------------------------------------
Successful Allocations: 1823
Failed Allocations: 0
Slots Currently Allocated: 33
Slots Currently Quarantined: 95
Moximum Slots Allocated: 51 / 64
```

### Memory Requested From The OS

The stats also report the amount of memory requested from the OS by mmap.

Memory is also requested, but may not actually be backed by physical memory, so
these stats should resemble the VSS of the application, not the RSS.

```
Low-level allocator stats:
MmapSysAllocator: 18083741696 bytes (17246.0 MiB) allocated
```

## Temeraire

### Introduction

Temeraire (or Huge Page Aware Allocator) is a new page heap for TCMalloc that is
hugepage aware. It is designed to better handle memory backed by hugepages -
avoiding breaking them up. Since it is more elaborate code, it reports
additional information.

See the [Temeraire design doc](temeraire.md) for more complete information.

### Summary Statistics

The initial set of statistics from the Huge Page Aware Allocator are similar to
the old page heap, and show a summary of the number of instances of each range
of contiguous pages.

```
------------------------------------------------
HugePageAware: 75 sizes;  938.8 MiB free; 1154.0 MiB unmapped
------------------------------------------------
 1 pages * 86655 spans ~ 677.0 MiB;  677.0 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
 2 pages *  3632 spans ~  56.8 MiB;  733.7 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
 3 pages *   288 spans ~   6.8 MiB;  740.5 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
 4 pages *   250 spans ~   7.8 MiB;  748.3 MiB cum; unmapped:    0.0 MiB;    0.0 MiB cum
...
```

The first line indicates the number of different sizes of ranges, the total MiB
available, and the total MiB of unmapped ranges. The next lines are per number
of continuous pages:

*   The number of contiguous pages
*   The number of spans of that number of pages
*   The total number of MiB of that span size that are mapped.
*   The cumulative total of the mapped pages.
*   The total number of MiB of that span size that are unmapped.
*   The cumulative total of the unmapped pages.

### Per Component Information

The Huge Page Aware Allocator has multiple places where pages of memory are
held. More details of its workings can be found in
[the Temeraire design doc](temeraire.md). There are four caches where pages of
memory can be located:

*   The filler, used for allocating ranges of a few TCMalloc pages in size.
*   The region cache, used for allocating ranges of multiple pages.
*   The huge cache which contains huge pages that are backed with memory.
*   The huge page allocator which contains huge pages that are not backed by
    memory.

We get some summary information for the various caches, before we report
detailed information for each of the caches.

```
Huge page aware allocator components:
------------------------------------------------
HugePageAware: breakdown of free / unmapped / used space:
HugePageAware: filler 38825.2 MiB used,  938.8 MiB free,    0.0 MiB unmapped
HugePageAware: region     0.0 MiB used,    0.0 MiB free,    0.0 MiB unmapped
HugePageAware: cache    908.0 MiB used,    0.0 MiB free,    0.0 MiB unmapped
HugePageAware: alloc      0.0 MiB used,    0.0 MiB free, 1154.0 MiB unmapped
```

The summary information tells us:

*   The first column shows how much memory has been allocated from each of the
    caches
*   The second column indicates how much backed memory is available in each
    cache.
*   The third column indicates how much unmapped memory is available in each
    cache.

### Filler Cache

The filler cache contains TCMalloc sized pages from within a single hugepage. So
if we want a single TCMalloc page we will look for it in the filler.

There are three sections of stats around the filler cache. The first section
gives an indication of the number and state of the hugepages in the filler
cache.

```
HugePageFiller: densely pack small requests into hugepages
HugePageFiller: 19882 total, 8083 full, 11799 partial, 0 released (0 partially), 0 quarantined
HugePageFiller: 120168 pages free in 19882 hugepages, 0.0236 free
HugePageFiller: among non-fulls, 0.0398 free
HugePageFiller: 499 used pages in subreleased hugepages (0 of them in partially released)
HugePageFiller: 0 hugepages partially released, 0.0000 released
HugePageFiller: 1.0000 of used pages hugepageable
HugePageFiller: Since startup, 26159 pages subreleased, 345 hugepages broken
```

The summary stats are as follows:

*   "total" refers to the total number of hugepages in the filler cache.
*   "full" is the number of those hugepages that have multiple in-use
    allocations.
*   "partial" is the remaining number of hugepages that have a single in-use
    allocation.
*   "released" is the number of hugepages that are released - i.e., partially
    unmapped. If partially released hugepages are enabled, the number in
    parentheses shows the number of hugepages in this category.
*   "quarantined" is a feature has been disabled, so the result is currently
    zero.

The second section gives an indication of the number of pages in various states
in the filler cache:

*   "pages free" refers to the number of free TCMalloc pages in the filler, as
    well as the ratio to the total number of hugepages.
*   "among non-fulls" states this ratio to the number of non-full hugepages.
*   "used pages" refers to the number of occupied pages in the different types
    of partially unmapped hugepages.

```
HugePageFiller: fullness histograms

HugePageFiller: # of regular hps with a<= # of free pages <b
HugePageFiller: <  0<=  8083 <  1<=     6 <  2<=     1 <  3<=     1 <  4<=     0 < 16<=   103
HugePageFiller: < 32<=     1 < 48<=     0 < 64<=     3 < 80<=     1 < 96<=     0 <112<=     0
HugePageFiller: <128<=    28 <144<=     0 <160<=     0 <176<=     1 <192<=     0 <208<=     0
HugePageFiller: <224<=     2 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of donated hps with a<= # of free pages <b
HugePageFiller: <  0<=     0 <  1<=     0 <  2<=     0 <  3<=     0 <  4<=     0 < 16<=     0
HugePageFiller: < 32<=     0 < 48<=     0 < 64<=     0 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=     1 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     0
HugePageFiller: <224<=     0 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of released hps with a<= # of free pages <b
...

HugePageFiller: # of regular hps with a<= longest free range <b
HugePageFiller: <  0<=  8083 <  1<=     6 <  2<=     1 <  3<=     1 <  4<=     0 < 16<=   103
HugePageFiller: < 32<=     1 < 48<=     0 < 64<=     4 < 80<=     0 < 96<=     0 <112<=     0
HugePageFiller: <128<=    29 <144<=     0 <160<=     0 <176<=     0 <192<=     0 <208<=     1
HugePageFiller: <224<=     1 <240<=     0 <252<=     0 <253<=     0 <254<=     0 <255<=     0

HugePageFiller: # of released hps with a<= longest free range <b
...

HugePageFiller: # of regular hps with a<= # of allocations <b
HugePageFiller: <  1<=     8 <  2<=     7 <  3<=    10 <  4<=    10 <  5<=    12 < 17<=    15
HugePageFiller: < 33<=    12 < 49<=     2 < 65<=     0 < 81<=     2 < 97<=    17 <113<=   166
HugePageFiller: <129<=    42 <145<=     6 <161<=    20 <177<=    48 <193<=   398 <209<=  1968
HugePageFiller: <225<=  5062 <241<=   425 <253<=     0 <254<=     0 <255<=     0 <256<=     0

HugePageFiller: # of released hps with a<= # of allocations <b
...
```

Some sections have been elided here for space.

There are three sections, split by three tracker types. They use the same
reporting format and indicate:

*   The available TCMalloc pages in the hugepages of the given type.
*   The longest contiguous range of available TCMalloc pages in the hugepages of
    the given type.
*   The number of current allocations from each of the hugepages of the given
    type. The ranges are offset by one here, because a hugepage can't have zero
    allocations.

The reporting format is the number of hugepages that are between a particular
range for the characteristic of interest. For example:

*   There are 3 regular hugepages with TCMalloc free pages >= 64 and < 80.
*   There are 6 regular hugepages with a longest contiguous length of exactly 1
    page.
*   There are 2 regular hugepages with between 81 and 96 allocations.

The three tracker types are "regular," "donated," and "released." "Regular" is
by far the most common, and indicates regular memory in the filler.

"Donated" is hugepages that have been donated to the filler from the tail of
large (multi-hugepage) allocations, so that the leftover space can be packed
with smaller allocations. But we prefer to use up all useable regular hugepages
before touching the donated ones, which devolve to "regular" type once they are
used. Because of this last property, donated hugepages always have only one
allocation and their longest range equals their free space, so those histograms
aren't shown.

"Released" is partially released hugepages. Normally the entirety of a hugepage
is backed by real RAM, but in partially released hugepages most of it has been
returned to the OS. Because this defeats the primary goal of the hugepage-aware
allocator, this is done rarely, and we only reuse partially-released hugepages
for new allocations as a last resort.

The final section shows a summary of the filler's state over the past 5 minute
time period:

```
HugePageFiller: time series over 5 min interval

HugePageFiller: realized fragmentation: 0.0 MiB
HugePageFiller: minimum free pages: 0 (0 backed)
HugePageFiller: at peak demand: 1774 pages (and 261 free, 13 unmapped)
HugePageFiller: at peak demand: 8 hps (5 regular, 1 donated, 0 partial, 2 released)
HugePageFiller: at peak hps: 1774 pages (and 261 free, 13 unmapped)
HugePageFiller: at peak hps: 8 hps (5 regular, 1 donated, 0 partial, 2 released)
```

The first line shows the minimum number of free pages over the time interval,
which is an indication of how much memory could have been "usefully" reclaimed
(i.e., free for long enough that the OS would likely be able to use the memory
for another process). The line shows both the total number of free pages in the
filler (whether or not released to the OS) as well as only those that were
backed by physical memory for the full 5-min interval. The realized
fragmentation metric computed here uses a bounded window.

The next two sections show the state of the filler at peak demand (i.e., when
the maximum number of pages was in use) and at peak hps (i.e., when the maximum
number of hugepages was in use). For each, we show the number of free (backed)
pages as well as unmapped pages, and the number of the four different types of
hugepages active at that time. If there are multiple peaks, we return the state
at the latest one of them.

If applicable, an additional section tracks the behavior that skips subreleasing
hugepages if behind the recent demand requirement, which is either the peak
within `--tcmalloc_skip_subrelease_interval`, or the sum of short-term
fluctuation peak within `--tcmalloc_skip_subrelease_short_interval` and
long-term trend within `--tcmalloc_skip_subrelease_long_interval`.

**Note:** Conducting skip-subrelease using both short-term and long-term
intervals is an experimental feature, and should not be enabled without
understanding its performance tradeoffs.

```
HugePageFiller: Since the start of the execution, 0 subreleases (0 pages) were skipped due to either recent (0s) peaks, or the sum of short-term (0s) fluctuations and long-term (0s) trends..
HugePageFiller: 100.0000% of decisions confirmed correct, 0 pending (100.0000% of pages, 0 pending), as per anticipated 300s realized fragmentation.
```

This shows how many times a page that was meant to be subreleased was not (note
that this can refer to the same page multiple times if subrelease of this page
would have been triggered multiple times). The percentage shows what fraction of
times this decision would have been correct (i.e., if we decided not to
subrelease a page because of the calculated demand requirement, did memory
consumption increase again within the *next* five minutes?). "Pending" refers to
subrelease decisions that were less than five minutes in the past and we
therefore do not know yet whether or not they were correct. The correctness
evaluation chooses to use the five minutes interval as it is the interval used
for realized fragmentation.

The skip-subrelease feature prioritizes using the recent peak if
`--tcmalloc_skip_subrelease_interval` is configured, otherwise it uses the
combination of the recent short-term fluctuation peak and long-term trend. The
feature is disabled if all three intervals are zero.

### Region Cache

The region cache holds a chunk of memory from which can be allocated spans of
multiple TCMalloc pages. The region cache may not be populated, and it can
contain multiple regions.

```
HugeRegionSet: 1 MiB+ allocations best-fit into 1024 MiB slabs
HugeRegionSet: 0 total regions
HugeRegionSet: 0 hugepages backed out of 0 total
HugeRegionSet: 0 pages free in backed region, 0.0000 free
```

The lines of output indicate:

*   The size of each region in MiB - this is currently 1GiB.
*   The total number of regions in the region cache, in the example above there
    are no regions in the cache.
*   The number of backed hugepages in the cache out of the total number of
    hugepages in the region cache.
*   The number of free TCMalloc pages in the regions, and as a ratio of the
    number of backed pages.

### Huge Cache

The huge cache contains backed hugepages, it grows and shrinks in size depending
on runtime conditions. Attempting to hold onto backed memory ready to be
provided for the application.

```
HugeCache: contains unused, backed hugepage(s)
HugeCache: 0 / 10 hugepages cached / cache limit (0.053 hit rate, 0.436 overflow rate)
HugeCache: 88880 MiB fast unbacked, 6814 MiB periodic
HugeCache: 1234 MiB*s cached since startup
HugeCache: recent usage range: 40672 min - 40672 curr -  40672 max MiB
HugeCache: recent offpeak range: 0 min - 0 curr - 0 max MiB
HugeCache: recent cache range: 0 min - 0 curr - 0 max MiB
```

The output shows the following information:

*   The number of hugepages out of the maximum number of hugepages we will hold
    in the huge cache. The hit rate is how often we get pages from the huge
    cache vs getting them from the huge allocator. The overflow rate is the
    number of times we added something to the huge cache causing it to exceed
    its size limit.
*   The fast unbacked is the cumulative amount of memory unbacked due size
    limitations, the periodic count is the cumulative amount of memory unbacked
    by periodic calls to release unused memory.
*   The amount of cumulative memory stored in HugeCache since the startup of the
    process. In other words, the area under the cached-memory-vs-time curve.
*   The usage range is the range minimum, current, maximum in MiB of memory
    obtained from the huge cache.
*   The off-peak range is the minimum, current, maximum cache size in MiB
    compared to the peak cache size.
*   The recent range is the minimum, current, maximum size of memory in MiB in
    the huge cache.

### Huge Allocator

The huge allocator holds unmapped memory ranges. We allocate from here if we are
unable to allocate from any of the caches.

```
HugeAllocator: contiguous, unbacked hugepage(s)
HugeAddressMap: treap 5 / 10 nodes used / created
HugeAddressMap: 256 contiguous hugepages available
HugeAllocator: 20913 requested - 20336 in use = 577 hugepages free
```

The information reported here is:

*   The number of nodes used and created to handle regions of memory.
*   The size of the longest contiguous region of available hugepages.
*   The number of hugepages requested from the system, the number of hugepages
    in used, and the number of hugepages available in the cache.

### Pageheap Summary Information

The new pageheap reports some summary information:

```
HugePageAware: stats on allocation sizes
HugePageAware: 4969003 pages live small allocation
HugePageAware: 659 pages of slack on large allocations
HugePageAware: largest seen allocation 45839 pages
```

These are:

*   The number of live "small" TCMalloc pages allocated (these less than 2MiB in
    size). [Note: the 2MiB size distinction is separate from the size of
    hugepages]
*   The number of TCMalloc pages which are left over from "large" allocations.
    These allocations are larger than 2MiB in size, and are rounded to a
    hugepage - the slack being the amount left over after rounding.
*   The largest seen allocation request in TCMalloc pages.

### Per Size Range Info:

The per size range info is the same format as the old pageheap:

*   The first column contains the number of pages (or the range of pages if the
    bucket is wider than a single page).
*   The second and third columns are the number of allocated and freed pages we
    have seen of this size.
*   The fourth column is the number of live allocations of this size.
*   The fifth column is the size of those live allocations in MiB.
*   The sixth column is the allocation rate in pages per second since the start
    of the application.
*   The seventh column is the allocation rate in MiB per second since the start
    of the application.

```
HugePageAware: per-size information:
HugePageAware: 1 page info: 5817510 / 3863506 a/f, 1954004 (15265.7 MiB) live,  16    allocs/s (   0.1 MiB/s)
HugePageAware: 2 page info: 1828473 / 1254096 a/f,  574377 ( 8974.6 MiB) live,   5.03 allocs/s (   0.1 MiB/s)
HugePageAware: 3 page info: 1464568 / 1227253 a/f,  237315 ( 5562.1 MiB) live,   4.03 allocs/s (   0.1 MiB/s)
...
```

### Pageheap Age Information:

The new pageheap allocator also reports information on the age of the various
page ranges. In this example you can see that there was a large number of
unmapped pages in the last minute.

```
------------------------------------------------
HugePageAware cache entry age (count of pages in spans of a given size that have been idle for up to the given period of time)
------------------------------------------------
                              mean    <1s     1s     30s      1m     30m      1h     8+h
Live span     TOTAL PAGES:  29317.6   145    549    1775   13059   13561   58622   32457
Live span,        1 pages:  35933.7     0     55     685    6354    8111   43853   27597
...
Unmapped span TOTAL PAGES:     51.3     0      0  131072   16640       0       0       0
Unmapped span,   >=64 pages:   51.3     0      0  131072   16640       0       0       0
...
```
