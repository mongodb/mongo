/*
 * Copyright 2013 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */


#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <bson/bson-atomic.h>
#include <bson/bson-config.h>
#include <bson/bson-memory.h>


// Ensure size of exported structs are stable.
BSON_STATIC_ASSERT2 (bson_mem_vtable_t, sizeof (bson_mem_vtable_t) == sizeof (void *) * 8u);


// For compatibility with C standards prior to C11.
static void *
_aligned_alloc_impl (size_t alignment, size_t num_bytes)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(_WIN32) && !defined(__ANDROID__) && \
   !defined(_AIX)
{
   return aligned_alloc (alignment, num_bytes);
}
#elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
{
   void *mem = NULL;

   // Workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=66425.
   BSON_MAYBE_UNUSED int ret = posix_memalign (&mem, alignment, num_bytes);

   return mem;
}
#else
{
   // Fallback to simple malloc even if it does not satisfy alignment
   // requirements. Note: Visual C++ _aligned_malloc requires using
   // _aligned_free instead of free and modifies errno on failure, both of which
   // breaks symmetry with C11 aligned_alloc, so it is deliberately not used.
   BSON_UNUSED (alignment);
   return malloc (num_bytes);
}
#endif


static bson_mem_vtable_t gMemVtable = {.malloc = malloc,
                                       .calloc = calloc,
                                       .realloc = realloc,
                                       .free = free,
                                       .aligned_alloc = _aligned_alloc_impl,
                                       .padding = {0}};


/*
 *--------------------------------------------------------------------------
 *
 * bson_malloc --
 *
 *       Allocates @num_bytes of memory and returns a pointer to it.  If
 *       malloc failed to allocate the memory, abort() is called.
 *
 *       Libbson does not try to handle OOM conditions as it is beyond the
 *       scope of this library to handle so appropriately.
 *
 * Parameters:
 *       @num_bytes: The number of bytes to allocate.
 *
 * Returns:
 *       A pointer if successful; otherwise abort() is called and this
 *       function will never return.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void *
bson_malloc (size_t num_bytes) /* IN */
{
   void *mem = NULL;

   if (BSON_LIKELY (num_bytes)) {
      if (BSON_UNLIKELY (!(mem = gMemVtable.malloc (num_bytes)))) {
         fprintf (stderr, "Failure to allocate memory in bson_malloc(). errno: %d.\n", errno);
         abort ();
      }
   }

   return mem;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_malloc0 --
 *
 *       Like bson_malloc() except the memory is zeroed first. This is
 *       similar to calloc() except that abort() is called in case of
 *       failure to allocate memory.
 *
 * Parameters:
 *       @num_bytes: The number of bytes to allocate.
 *
 * Returns:
 *       A pointer if successful; otherwise abort() is called and this
 *       function will never return.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void *
bson_malloc0 (size_t num_bytes) /* IN */
{
   void *mem = NULL;

   if (BSON_LIKELY (num_bytes)) {
      if (BSON_UNLIKELY (!(mem = gMemVtable.calloc (1, num_bytes)))) {
         fprintf (stderr, "Failure to allocate memory in bson_malloc0(). errno: %d.\n", errno);
         abort ();
      }
   }

   return mem;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_aligned_alloc --
 *
 *       Allocates @num_bytes of memory with an alignment of @alignment and
 *       returns a pointer to it.  If malloc failed to allocate the memory,
 *       abort() is called.
 *
 *       Libbson does not try to handle OOM conditions as it is beyond the
 *       scope of this library to handle so appropriately.
 *
 * Parameters:
 *       @alignment: The alignment of the allocated bytes of memory.
 *       @num_bytes: The number of bytes to allocate.
 *
 * Returns:
 *       A pointer if successful; otherwise abort() is called and this
 *       function will never return.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void *
bson_aligned_alloc (size_t alignment /* IN */, size_t num_bytes /* IN */)
{
   void *mem = NULL;

   if (BSON_LIKELY (num_bytes)) {
      if (BSON_UNLIKELY (!(mem = gMemVtable.aligned_alloc (alignment, num_bytes)))) {
         fprintf (stderr, "Failure to allocate memory in bson_aligned_alloc()\n");
         abort ();
      }
   }

   return mem;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_aligned_alloc0 --
 *
 *       Like bson_aligned_alloc() except the memory is zeroed after allocation
 *       for convenience.
 *
 * Parameters:
 *       @alignment: The alignment of the allocated bytes of memory.
 *       @num_bytes: The number of bytes to allocate.
 *
 * Returns:
 *       A pointer if successful; otherwise abort() is called and this
 *       function will never return.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void *
bson_aligned_alloc0 (size_t alignment /* IN */, size_t num_bytes /* IN */)
{
   void *mem = NULL;

   if (BSON_LIKELY (num_bytes)) {
      if (BSON_UNLIKELY (!(mem = gMemVtable.aligned_alloc (alignment, num_bytes)))) {
         fprintf (stderr, "Failure to allocate memory in bson_aligned_alloc0()\n");
         abort ();
      }
      memset (mem, 0, num_bytes);
   }

   return mem;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_realloc --
 *
 *       This function behaves similar to realloc() except that if there is
 *       a failure abort() is called.
 *
 * Parameters:
 *       @mem: The memory to realloc, or NULL.
 *       @num_bytes: The size of the new allocation or 0 to free.
 *
 * Returns:
 *       The new allocation if successful; otherwise abort() is called and
 *       this function never returns.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void *
bson_realloc (void *mem,        /* IN */
              size_t num_bytes) /* IN */
{
   /*
    * Not all platforms are guaranteed to free() the memory if a call to
    * realloc() with a size of zero occurs. Windows, Linux, and FreeBSD do,
    * however, OS X does not.
    */
   if (BSON_UNLIKELY (num_bytes == 0)) {
      gMemVtable.free (mem);
      return NULL;
   }

   mem = gMemVtable.realloc (mem, num_bytes);

   if (BSON_UNLIKELY (!mem)) {
      fprintf (stderr, "Failure to re-allocate memory in bson_realloc(). errno: %d.\n", errno);
      abort ();
   }

   return mem;
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_realloc_ctx --
 *
 *       This wraps bson_realloc and provides a compatible api for similar
 *       functions with a context
 *
 * Parameters:
 *       @mem: The memory to realloc, or NULL.
 *       @num_bytes: The size of the new allocation or 0 to free.
 *       @ctx: Ignored
 *
 * Returns:
 *       The new allocation if successful; otherwise abort() is called and
 *       this function never returns.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */


void *
bson_realloc_ctx (void *mem,        /* IN */
                  size_t num_bytes, /* IN */
                  void *ctx)        /* IN */
{
   BSON_UNUSED (ctx);

   return bson_realloc (mem, num_bytes);
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_free --
 *
 *       Frees @mem using the underlying allocator.
 *
 *       Currently, this only calls free() directly, but that is subject to
 *       change.
 *
 * Parameters:
 *       @mem: An allocation to free.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
bson_free (void *mem) /* IN */
{
   gMemVtable.free (mem);
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_zero_free --
 *
 *       Frees @mem using the underlying allocator. @size bytes of @mem will
 *       be zeroed before freeing the memory. This is useful in scenarios
 *       where @mem contains passwords or other sensitive information.
 *
 * Parameters:
 *       @mem: An allocation to free.
 *       @size: The number of bytes in @mem.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
bson_zero_free (void *mem,   /* IN */
                size_t size) /* IN */
{
   if (BSON_LIKELY (mem)) {
      memset (mem, 0, size);
      gMemVtable.free (mem);
   }
}


static void *
_aligned_alloc_as_malloc (size_t alignment, size_t num_bytes)
{
   BSON_UNUSED (alignment);

   return gMemVtable.malloc (num_bytes);
}


/*
 *--------------------------------------------------------------------------
 *
 * bson_mem_set_vtable --
 *
 *       This function will change our allocation vtable.
 *
 *       It is imperative that this is called at the beginning of the
 *       process before any memory has been allocated by the default
 *       allocator.
 *
 * Returns:
 *       None.
 *
 * Side effects:
 *       None.
 *
 *--------------------------------------------------------------------------
 */

void
bson_mem_set_vtable (const bson_mem_vtable_t *vtable)
{
   BSON_ASSERT (vtable);

   if (!vtable->malloc || !vtable->calloc || !vtable->realloc || !vtable->free) {
      fprintf (stderr,
               "Failure to install BSON vtable, "
               "missing functions.\n");
      return;
   }

   gMemVtable = *vtable;

   // Backwards compatibility with code prior to addition of aligned_alloc.
   if (!gMemVtable.aligned_alloc) {
      gMemVtable.aligned_alloc = _aligned_alloc_as_malloc;
   }
}

void
bson_mem_restore_vtable (void)
{
   bson_mem_vtable_t vtable = {.malloc = malloc,
                               .calloc = calloc,
                               .realloc = realloc,
                               .free = free,
                               .aligned_alloc = _aligned_alloc_impl,
                               .padding = {0}};

   bson_mem_set_vtable (&vtable);
}
