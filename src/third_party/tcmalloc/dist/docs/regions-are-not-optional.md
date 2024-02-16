# Regions Are Not Optional!

Andrew Hunter

Discussion on the design of [Temeraire](temeraire.md) posited that `HugeRegion`
is a weird/complex feature that possibly is a premature optimization.
`HugeRegion` is neither optional, nor really all that complex. We claim this is
actually a fairly simple approach that fixes what would otherwise be a very
serious flaw.

This expands on the description of `HugeRegion` in the main design doc.

## Our Trilemma

`HugeRegion` exists because of three key framing requirements for a
Temeraire-enabled TCMalloc:

1.  We must support allocations of any (reasonable) size, and in particular a
    heap composed of any set of reasonable sizes in any ratio; "sorry, tcmalloc
    detonates if you mostly use requests of size X" is not acceptable.
1.  We must be able to back (most, ideally all) of our heap with hugepages.
1.  We would like to tightly bound global space overhead[^1] on our heap.

Consider requests R<sub>i</sub> that are larger than a hugepage, but small
enough that the rounding error from extending to a hugepage boundary is
significant by (3). (Note that rounding up to a hugepage boundary would
introduce a significant amount of overhead for allocations between 1 and 10
hugepages, and the overhead could still be considered significant for
allocations larger than that.)

*   We *cannot* unback the unused tail of the last hugepage (requirement (2)
    would be violated).
*   We *cannot* assume these requests are necessarily rare and we will have many
    smaller ones to fill the unused tail (requirement (1) would be violated).
    Moreover this is **empirically false** for widely used
    binaries.

In summary, we must be able to use the unused tail of a hugepage from one
R<sub>i</sub> as space for another large R<sub>j</sub>. If we do not enable such
usage in our allocator, we will either potentially have space overhead of up to
100%, or dramatically reduce our hugepage usage. The conclusion we came to is
that we **must support**, in some form, allocating multiple such R<sub>i</sub>
contiguously; that is, using the unused tail from R<sub>1 </sub>as the beginning
of R<sub>2</sub> and so on.

**This is all `HugeRegion{,Set}` does.**

## The "Simple" Truth

The above argument is why we have `HugeRegion`: we need a way to allocate
multiple large (>1 hugepage) allocations on overlapping hugepages. So how can we
do that? Clearly, we need some range of hugepages, large enough for several such
R<sub>i</sub>, from which we allocate. What should we do in that space? A
best-fit algorithm that tracks the free lengths seems appropriate.

As allocations become free, it seems reasonable (by requirement (3) above) that
we unback empty hugepages.

Finally, what happens if the the range we allocated is full? We could do two
things

1.  extend it
1.  obtain a new one and do allocations from there as needed.

(1) is an interesting choice, but not actually possible with the `SysAllocator`
interface. We might get lucky with `sbrk` (or even `mmap`, though it is less
likely) placement choice, but we also might not; we cannot rely on it. So we
must be able to fall back to (2) anyway, and given that there's very little
disadvantages to having multiple such ranges (we won’t need very many in any
case), why not just only do that?

It should not be surprising that we have just described the algorithm
`HugeRegion{,Set}` uses: inside some fixed-size range, do best-fit allocation
for large allocations, backing and unbacking hugepages on demand. When one
region fills, obtain another; fill from the most fragmented to bound total
overhead (a policy derived from `HugePageFiller`).

That is *really it*. We do not see this as particularly complicated. The only
thing left is the implementation of that policy: We used `RangeTracker` because
it was convenient, supported exactly the API we needed, and fast enough (even
though we're tracking fairly large bitsets).

## But what about...

There are some reasonable objections to particular details, which we are happy
to address.

### Why are regions so big?

Because it worked. Virtual address space is virtually free. :) We can easily
justify why they aren’t 32 MiB (our original choice, as it happens):
[Temeraire](temeraire.md) contains a simple argument, it is trivial to waste a
full hugepage per region, and this scales down nicely with increasing region
size. Why did we go to a gigabyte? Because it worked. :) It had an added
advantage: even large binaries would only use a handful of regions, and thus
walking the list was cheap and we could print a lot of info about each in
mallocz.

We've run more tests; 128 MiB and 512 MiB both perform reasonably, but this
isn't a compelling reason to change the size. We don't really support VSS limits
(and in practice we don't have them, outside badly behaved sandbox programs and
some daemons that use `SMALL_BUT_SLOW` anyway, which we're not currently
changing).

### How did we pick the current policy for what goes to regions?

Because it worked. The arguments above make it clear that anything larger than
one hugepage and smaller than &lt;some value we can agree is many&gt; hugepages
must go there. It seemed reasonable to allow slightly smaller ones to slip into
the region if we had space and it was needed; we saw no reason not to allow
many-hugepage allocations there if they fit. In practice, this seems to work
well. There really isn’t more thought than that.

### Can’t we fix binaries with problematic allocation patterns?

Yes, we can. We probably should. It'd be good to do anyway. However: doing so
doesn’t stop us from needing Regions:

*   Changing workloads takes a long time.
*   We cannot successfully change, all the programs that make any significant
    use of allocations &gt;2 MiB and less than (say) 50 MiB. We cannot tell
    users "Eh, no, tcmalloc does terribly if you allocate a couple megabytes at
    a time?" Requirement (1) above is our expression of how we don't think
    that's reasonable at all: we should able to handle 3 MiB allocations without
    embarrassing ourselves.

Recall that the trilemma leading to regions applies for **anything more than 2
MiB which we can't just ignore the tail on**. It's easiest to show the potential
huge problems with the canonical "2.1 MiB" allocation, but 5 MiB or 6.1 MiB or
even 10.1 MiB allocations, if they're a significant component of heap usage,
will lead to unacceptable overhead without `HugeRegion`, and we don't think we
can say "don't do that."

## Conclusion

`HugeRegion` is the simplest possible solution we've found to a pressing problem
in a hugepage-oriented allocator. When you read the [design doc](temeraire.md),
please don't assume that HugeRegion is a speculative fix for a potential
problem, that we might not need, nor that it's a roughed out attempt. This is a
key part of the algorithm, and one we've thought a lot about the best fix for.
We don't claim it is perfect and must surely have hit on the best fix, but
"nothing" is not an acceptable solution. This gets reasonable space performance
with badly sized allocations.

**In short, `HugeRegion` is neither optional nor particularly complex. Having it
produces dramatic savings in a number of realistic scenarios, and costs us very
little.**

## Notes

[^1]: What our designed bound of overhead is...a very interesting question.
    Different places accept different forms of overhead. While we could target
    the current overhead, we can and must do better than this. One goal of
    Temeraire is to dramatically cut this (in the pageheap).
