# Immutable Containers

This folder contains a number of _immutable_ container classes. Sometimes called _persistent data
structures_ in the literature, these classes provide interfaces similar to STL containers with one
key difference. Operations which "modify" the container are `const` and return a modified copy of
the container rather than modifying it in-place. This makes the containers implicitly thread-safe to
read and write, but external synchronization will still be needed in many cases to address isolation
and serializability concerns (i.e.
[MVCC](https://en.wikipedia.org/wiki/Multiversion_concurrency_control)).

## When To Use Immutable Containers

If the container will be copied frequently, e.g. to support a copy-on-write pattern, consider using
an immutable container. Otherwise, a standard container may make more sense.

## Supported Containers

The currently supported containers are all based on classes from the
[`immer`](https://sinusoid.es/immer/) library.

-   [`immutable::map`](map.h): ordered map interface backed by `immer::flex_vector`
-   [`immutable::set`](set.h): ordered set interface backed by `immer::flex_vector`
-   [`immutable::unordered_map`](unordered_map.h): typedef for `immer:map`
-   [`immutable::unordered_set`](unordered_set.h): typedef for `immer:set`
-   [`immutable::vector`](vector.h): typedef for `immer::vector`

Both ordered and unordered map and set variants support heterogeneous lookup.

## A Note on Performance

The internal implementations of these containers are optimized to support an internal copy-on-write
pattern so that copies and modifications are $O(log(n))$ or even $O(1)$. However, the constants on
these runtime guarantees, as well as those for lookups, are typically worse than those of the
corresponding STL or Abseil containers. For this reason, they should not be considered a
general-purpose drop-in replacement.
