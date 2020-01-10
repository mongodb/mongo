/* This program should crash and produce coredump */

#include "compiler.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#endif

#if defined(__linux__)
void write_maps(char *fname)
{
    char buf[512], path[128];
    char exec;
    uintmax_t addr;
    FILE *maps = fopen("/proc/self/maps", "r");
    FILE *out = fopen(fname, "w");

    if (!maps || !out)
        exit(EXIT_FAILURE);

    while (fgets(buf, sizeof(buf), maps))
    {
        if (sscanf(buf, "%jx-%*x %*c%*c%c%*c %*x %*s %*d /%126[^\n]", &addr, &exec, path+1) != 3)
            continue;

        if (exec != 'x')
            continue;

        path[0] = '/';
        fprintf(out, "0x%jx:%s ", addr, path);
    }
    fprintf(out, "\n");

    fclose(out);
    fclose(maps);
}
#elif defined(__FreeBSD__)
void
write_maps(char *fname)
{
    FILE *out;
    char *buf, *bp, *eb;
    struct kinfo_vmentry *kv;
    int mib[4], error;
    size_t len;

    out = fopen(fname, "w");
    if (out == NULL)
        exit(EXIT_FAILURE);

    len = 0;
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_VMMAP;
    mib[3] = getpid();
    error = sysctl(mib, 4, NULL, &len, NULL, 0);
    if (error == -1)
	exit(EXIT_FAILURE);
    len = len * 4 / 3;
    buf = malloc(len);
    if (buf == NULL)
	exit(EXIT_FAILURE);
    error = sysctl(mib, 4, buf, &len, NULL, 0);
    if (error == -1)
	    exit(EXIT_FAILURE);

    for (bp = buf, eb = buf + len; bp < eb; bp += kv->kve_structsize) {
        kv = (struct kinfo_vmentry *)(uintptr_t)bp;
	if (kv->kve_type == KVME_TYPE_VNODE &&
	  (kv->kve_protection & KVME_PROT_EXEC) != 0) {
	    fprintf(out, "0x%jx:%s ", kv->kve_start, kv->kve_path);
	}
    }

    fprintf(out, "\n");
    fclose(out);
    free(buf);
}
#else
#error Port me
#endif

#ifdef __GNUC__
#ifndef __clang__
// Gcc >= 8 became too good at inlining aliase c into b when using -O2 or -O3,
// so force -O1 in all cases, otherwise a frame will be missing in the tests.
#pragma GCC optimize "-O1"
#endif
int c(int x) NOINLINE ALIAS(b);
#define compiler_barrier() __asm__ __volatile__ ("");
#else
int c(int x);
#define compiler_barrier()
#endif

int NOINLINE a(void)
{
  *(volatile int *)32 = 1;
  return 1;
}

int NOINLINE b(int x)
{
  int r;

  compiler_barrier();
  
  if (x)
    r = a();
  else
    r = c(1);
  return r + 1;
}

int
main (int argc, char **argv)
{
  if (argc > 1)
      write_maps(argv[1]);
  b(0);
  return 0;
}

