/*
 * Contributed by Stephen M. Webb <stephen.webb@bregmasoft.ca>
 *
 * This file is part of libunwind, a platform-independent unwind library.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "libunwind_i.h"


#ifdef UNW_REMOTE_ONLY
bool
unw_address_is_valid(UNUSED unw_word_t addr, UNUSED size_t len)
{
  Debug(1, "remote-only invoked\n");
  return false;
}

#else /* !UNW_REMOTE_ONLY */

#include <stdatomic.h>


static atomic_flag _unw_address_validator_initialized = ATOMIC_FLAG_INIT;
static int _mem_validate_pipe[2] = {-1, -1};

#ifdef HAVE_PIPE2
static int
_do_pipe2 (int pipefd[2])
{
  return pipe2 (pipefd, O_CLOEXEC | O_NONBLOCK);
}
#else
static void
_set_pipe_flags (int fd)
{
  int fd_flags = fcntl (fd, F_GETFD, 0);
  int status_flags = fcntl (fd, F_GETFL, 0);

  fd_flags |= FD_CLOEXEC;
  fcntl (fd, F_SETFD, fd_flags);

  status_flags |= O_NONBLOCK;
  fcntl (fd, F_SETFL, status_flags);
}

static int
_do_pipe2 (int pipefd[2])
{
  if (pipe (pipefd) < 0)
    {
      return -1;
    }
  _set_pipe_flags(pipefd[0]);
  _set_pipe_flags(pipefd[1]);
  return 0;
}
#endif


static int
_open_pipe (void)
{
  if (_mem_validate_pipe[0] != -1)
    close (_mem_validate_pipe[0]);
  if (_mem_validate_pipe[1] != -1)
    close (_mem_validate_pipe[1]);

  return _do_pipe2 (_mem_validate_pipe);
}


/**
 * Test is a memory address is valid by trying to write from it
 * @param[in]  addr The address to validate
 *
 * @returns true if the memory address is valid (readable), false otherwise.
 *
 * This check works by using the address as a (one-byte) buffer in a
 * write-to-pipe operation.  The write will fail if the memory is not in the
 * process's address space and marked as readable. The read will force the page
 * to be swapped in if it's not already there.
 */
static bool
_write_validate (unw_word_t addr)
{
  int ret = -1;
  ssize_t bytes = 0;

  if (unlikely (!atomic_flag_test_and_set(&_unw_address_validator_initialized)))
    {
      if (_open_pipe () != 0)
        {
          return false;
        }
    }

  do
    {
      char buf;
      bytes = read (_mem_validate_pipe[0], &buf, 1);
    }
  while ( errno == EINTR );

  if (!(bytes > 0 || errno == EAGAIN || errno == EWOULDBLOCK))
    {
      // re-open closed pipe
      if (_open_pipe () != 0)
        {
          return false;
        }
    }

  do
    {
#ifdef HAVE_SYS_SYSCALL_H
      /* use syscall insteadof write() so that ASAN does not complain */
      ret = syscall (SYS_write, _mem_validate_pipe[1], addr, 1);
#else
      ret = write (_mem_validate_pipe[1], (void *)addr, 1);
#endif
    }
  while ( errno == EINTR );

  return ret > 0;
}


/* Cache of already validated addresses */
enum { NLGA = 4 };

#if defined(HAVE___CACHE_PER_THREAD) && HAVE___CACHE_PER_THREAD
// thread-local variant
static _Thread_local unw_word_t last_good_addr[NLGA];
static _Thread_local int lga_victim;


static bool
_is_cached_valid_mem(unw_word_t page_addr)
{
  int i;
  for (i = 0; i < NLGA; i++)
    {
      if (page_addr == last_good_addr[i])
        return true;
    }
  return false;
}


static void
_cache_valid_mem(unw_word_t page_addr)
{
  int i, victim;
  victim = lga_victim;
  for (i = 0; i < NLGA; i++)
    {
      if (last_good_addr[victim] == 0)
        {
          last_good_addr[victim] = page_addr;
          return;
        }
      victim = (victim + 1) % NLGA;
    }

  /* All slots full. Evict the victim. */
  last_good_addr[victim] = page_addr;
  victim = (victim + 1) % NLGA;
  lga_victim = victim;
}

#else
// global, thread safe variant
static _Atomic unw_word_t last_good_addr[NLGA];
static _Atomic int lga_victim;


static bool
_is_cached_valid_mem(unw_word_t page_addr)
{
  int i;
  for (i = 0; i < NLGA; i++)
    {
      if (page_addr == atomic_load(&last_good_addr[i]))
        return true;
    }
  return false;
}


/**
 * Adds a known-valid page address to the cache.
 *
 * This implementation is racy as all get-out but the worst case is that cached
 * address get lost, forcing extra unnecessary validation checks.  All of the
 * atomic operations don't matter because of TOCTOU races.
 */
static void
_cache_valid_mem(unw_word_t page_addr)
{
  unw_word_t zero = 0;
  int victim = atomic_load(&lga_victim);
  for (int i = 0; i < NLGA; i++)
    {
      if (atomic_compare_exchange_strong(&last_good_addr[victim], &zero, page_addr))
        {
          return;
        }
      victim = (victim + 1) % NLGA;
    }

  /* All slots full. Evict the victim. */
  atomic_store(&last_good_addr[victim], page_addr);
  victim = (victim + 1) % NLGA;
  atomic_store(&lga_victim, victim);
}
#endif


/**
 * Validate an address is readable
 * @param[in]  addr The (starting) address of the memory range to validate
 * @param[in]  len  The size of the memory range to validate in bytes
 *
 * Validates the memory range from @p addr to (@p addr + @p len - 1) is
 * readable.  Since the granularity of memory readability is the page, only one
 * byte needs to be validated per page for each page starting at @p addr and
 * encompassing @p len bytes. Only the first address of each page is checked.
 *
 * @returns true if the memory is readable, false otherwise.
 */
bool
unw_address_is_valid(unw_word_t addr, size_t len)
{
  if (len == 0)
    return true;

  /*
   * Find the starting address of the page containing the start of the range.
   */
  unw_word_t start_page_addr = unw_page_start (addr);

  /*
   * Bounds check on bottom of memory: first page is always deemed inaccessible.
   * This is potentially incorrect on an embedded system, especially one running
   * on bare metal with no VMM, but the check has always been here and no one
   * has complained.
   */
  if (start_page_addr == 0)
    return false;

  /*
   * Bounds check on top of memory. Unsigned wraparound could be hazardous.
   */
  if (addr > (UNW_WORD_MAX - len - unw_page_size))
    return false;

  /*
   * Find the starting address of the page containing the end of the range.
   */
  unw_word_t end_page_addr = unw_page_start (addr + (len - 1)) + unw_page_size;

  /*
   * Step through each page and check if the first address in each is readable.
   * The first non-readable page encountered means none of them in the given
   * range can be considered readable.
   */
  for (unw_word_t page_addr = start_page_addr;
       page_addr < end_page_addr;
       page_addr += unw_page_size)
    {
      if (!_is_cached_valid_mem(page_addr))
        {
          if (!_write_validate (page_addr))
            {
              Debug(1, "returning false\n");
              return false;
            }
          _cache_valid_mem(page_addr);
        }
    }

  return true;
}

#endif /* !UNW_REMOTE_ONLY */
