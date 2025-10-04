/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#include "platform.h"
#include <stdio.h>      /* fprintf, open, fdopen, fread, _fileno, stdin, stdout */
#include <stdlib.h>     /* malloc, free */
#include <assert.h>
#include <errno.h>      /* errno */

#if defined (_MSC_VER)
#  include <sys/stat.h>
#  include <io.h>
#endif

#include "fileio_asyncio.h"
#include "fileio_common.h"

/* **********************************************************************
 *  Sparse write
 ************************************************************************/

/** AIO_fwriteSparse() :
*  @return : storedSkips,
*            argument for next call to AIO_fwriteSparse() or AIO_fwriteSparseEnd() */
static unsigned
AIO_fwriteSparse(FILE* file,
                 const void* buffer, size_t bufferSize,
                 const FIO_prefs_t* const prefs,
                 unsigned storedSkips)
{
    const size_t* const bufferT = (const size_t*)buffer;   /* Buffer is supposed malloc'ed, hence aligned on size_t */
    size_t bufferSizeT = bufferSize / sizeof(size_t);
    const size_t* const bufferTEnd = bufferT + bufferSizeT;
    const size_t* ptrT = bufferT;
    static const size_t segmentSizeT = (32 KB) / sizeof(size_t);   /* check every 32 KB */

    if (prefs->testMode) return 0;  /* do not output anything in test mode */

    if (!prefs->sparseFileSupport) {  /* normal write */
        size_t const sizeCheck = fwrite(buffer, 1, bufferSize, file);
        if (sizeCheck != bufferSize)
            EXM_THROW(70, "Write error : cannot write block : %s",
                      strerror(errno));
        return 0;
    }

    /* avoid int overflow */
    if (storedSkips > 1 GB) {
        if (LONG_SEEK(file, 1 GB, SEEK_CUR) != 0)
        EXM_THROW(91, "1 GB skip error (sparse file support)");
        storedSkips -= 1 GB;
    }

    while (ptrT < bufferTEnd) {
        size_t nb0T;

        /* adjust last segment if < 32 KB */
        size_t seg0SizeT = segmentSizeT;
        if (seg0SizeT > bufferSizeT) seg0SizeT = bufferSizeT;
        bufferSizeT -= seg0SizeT;

        /* count leading zeroes */
        for (nb0T=0; (nb0T < seg0SizeT) && (ptrT[nb0T] == 0); nb0T++) ;
        storedSkips += (unsigned)(nb0T * sizeof(size_t));

        if (nb0T != seg0SizeT) {   /* not all 0s */
            size_t const nbNon0ST = seg0SizeT - nb0T;
            /* skip leading zeros */
            if (LONG_SEEK(file, storedSkips, SEEK_CUR) != 0)
                EXM_THROW(92, "Sparse skip error ; try --no-sparse");
            storedSkips = 0;
            /* write the rest */
            if (fwrite(ptrT + nb0T, sizeof(size_t), nbNon0ST, file) != nbNon0ST)
                EXM_THROW(93, "Write error : cannot write block : %s",
                          strerror(errno));
        }
        ptrT += seg0SizeT;
    }

    {   static size_t const maskT = sizeof(size_t)-1;
        if (bufferSize & maskT) {
            /* size not multiple of sizeof(size_t) : implies end of block */
            const char* const restStart = (const char*)bufferTEnd;
            const char* restPtr = restStart;
            const char* const restEnd = (const char*)buffer + bufferSize;
            assert(restEnd > restStart && restEnd < restStart + sizeof(size_t));
            for ( ; (restPtr < restEnd) && (*restPtr == 0); restPtr++) ;
            storedSkips += (unsigned) (restPtr - restStart);
            if (restPtr != restEnd) {
                /* not all remaining bytes are 0 */
                size_t const restSize = (size_t)(restEnd - restPtr);
                if (LONG_SEEK(file, storedSkips, SEEK_CUR) != 0)
                    EXM_THROW(92, "Sparse skip error ; try --no-sparse");
                if (fwrite(restPtr, 1, restSize, file) != restSize)
                    EXM_THROW(95, "Write error : cannot write end of decoded block : %s",
                              strerror(errno));
                storedSkips = 0;
            }   }   }

    return storedSkips;
}

static void
AIO_fwriteSparseEnd(const FIO_prefs_t* const prefs, FILE* file, unsigned storedSkips)
{
    if (prefs->testMode) assert(storedSkips == 0);
    if (storedSkips>0) {
        assert(prefs->sparseFileSupport > 0);  /* storedSkips>0 implies sparse support is enabled */
        (void)prefs;   /* assert can be disabled, in which case prefs becomes unused */
        if (LONG_SEEK(file, storedSkips-1, SEEK_CUR) != 0)
            EXM_THROW(69, "Final skip error (sparse file support)");
        /* last zero must be explicitly written,
         * so that skipped ones get implicitly translated as zero by FS */
        {   const char lastZeroByte[1] = { 0 };
            if (fwrite(lastZeroByte, 1, 1, file) != 1)
                EXM_THROW(69, "Write error : cannot write last zero : %s", strerror(errno));
        }   }
}


/* **********************************************************************
 *  AsyncIO functionality
 ************************************************************************/

/* AIO_supported:
 * Returns 1 if AsyncIO is supported on the system, 0 otherwise. */
int AIO_supported(void) {
#ifdef ZSTD_MULTITHREAD
    return 1;
#else
    return 0;
#endif
}

/* ***********************************
 *  Generic IoPool implementation
 *************************************/

static IOJob_t *AIO_IOPool_createIoJob(IOPoolCtx_t *ctx, size_t bufferSize) {
    IOJob_t* const job  = (IOJob_t*) malloc(sizeof(IOJob_t));
    void* const buffer = malloc(bufferSize);
    if(!job || !buffer)
        EXM_THROW(101, "Allocation error : not enough memory");
    job->buffer = buffer;
    job->bufferSize = bufferSize;
    job->usedBufferSize = 0;
    job->file = NULL;
    job->ctx = ctx;
    job->offset = 0;
    return job;
}


/* AIO_IOPool_createThreadPool:
 * Creates a thread pool and a mutex for threaded IO pool.
 * Displays warning if asyncio is requested but MT isn't available. */
static void AIO_IOPool_createThreadPool(IOPoolCtx_t* ctx, const FIO_prefs_t* prefs) {
    ctx->threadPool = NULL;
    ctx->threadPoolActive = 0;
    if(prefs->asyncIO) {
        if (ZSTD_pthread_mutex_init(&ctx->ioJobsMutex, NULL))
            EXM_THROW(102,"Failed creating ioJobsMutex mutex");
        /* We want MAX_IO_JOBS-2 queue items because we need to always have 1 free buffer to
         * decompress into and 1 buffer that's actively written to disk and owned by the writing thread. */
        assert(MAX_IO_JOBS >= 2);
        ctx->threadPool = POOL_create(1, MAX_IO_JOBS - 2);
        ctx->threadPoolActive = 1;
        if (!ctx->threadPool)
            EXM_THROW(104, "Failed creating I/O thread pool");
    }
}

/* AIO_IOPool_init:
 * Allocates and sets and a new I/O thread pool including its included availableJobs. */
static void AIO_IOPool_init(IOPoolCtx_t* ctx, const FIO_prefs_t* prefs, POOL_function poolFunction, size_t bufferSize) {
    int i;
    AIO_IOPool_createThreadPool(ctx, prefs);
    ctx->prefs = prefs;
    ctx->poolFunction = poolFunction;
    ctx->totalIoJobs = ctx->threadPool ? MAX_IO_JOBS : 2;
    ctx->availableJobsCount = ctx->totalIoJobs;
    for(i=0; i < ctx->availableJobsCount; i++) {
        ctx->availableJobs[i] = AIO_IOPool_createIoJob(ctx, bufferSize);
    }
    ctx->jobBufferSize = bufferSize;
    ctx->file = NULL;
}


/* AIO_IOPool_threadPoolActive:
 * Check if current operation uses thread pool.
 * Note that in some cases we have a thread pool initialized but choose not to use it. */
static int AIO_IOPool_threadPoolActive(IOPoolCtx_t* ctx) {
    return ctx->threadPool && ctx->threadPoolActive;
}


/* AIO_IOPool_lockJobsMutex:
 * Locks the IO jobs mutex if threading is active */
static void AIO_IOPool_lockJobsMutex(IOPoolCtx_t* ctx) {
    if(AIO_IOPool_threadPoolActive(ctx))
        ZSTD_pthread_mutex_lock(&ctx->ioJobsMutex);
}

/* AIO_IOPool_unlockJobsMutex:
 * Unlocks the IO jobs mutex if threading is active */
static void AIO_IOPool_unlockJobsMutex(IOPoolCtx_t* ctx) {
    if(AIO_IOPool_threadPoolActive(ctx))
        ZSTD_pthread_mutex_unlock(&ctx->ioJobsMutex);
}

/* AIO_IOPool_releaseIoJob:
 * Releases an acquired job back to the pool. Doesn't execute the job. */
static void AIO_IOPool_releaseIoJob(IOJob_t* job) {
    IOPoolCtx_t* const ctx = (IOPoolCtx_t *) job->ctx;
    AIO_IOPool_lockJobsMutex(ctx);
    assert(ctx->availableJobsCount < ctx->totalIoJobs);
    ctx->availableJobs[ctx->availableJobsCount++] = job;
    AIO_IOPool_unlockJobsMutex(ctx);
}

/* AIO_IOPool_join:
 * Waits for all tasks in the pool to finish executing. */
static void AIO_IOPool_join(IOPoolCtx_t* ctx) {
    if(AIO_IOPool_threadPoolActive(ctx))
        POOL_joinJobs(ctx->threadPool);
}

/* AIO_IOPool_setThreaded:
 * Allows (de)activating threaded mode, to be used when the expected overhead
 * of threading costs more than the expected gains. */
static void AIO_IOPool_setThreaded(IOPoolCtx_t* ctx, int threaded) {
    assert(threaded == 0 || threaded == 1);
    assert(ctx != NULL);
    if(ctx->threadPoolActive != threaded) {
        AIO_IOPool_join(ctx);
        ctx->threadPoolActive = threaded;
    }
}

/* AIO_IOPool_free:
 * Release a previously allocated IO thread pool. Makes sure all tasks are done and released. */
static void AIO_IOPool_destroy(IOPoolCtx_t* ctx) {
    int i;
    if(ctx->threadPool) {
        /* Make sure we finish all tasks and then free the resources */
        AIO_IOPool_join(ctx);
        /* Make sure we are not leaking availableJobs */
        assert(ctx->availableJobsCount == ctx->totalIoJobs);
        POOL_free(ctx->threadPool);
        ZSTD_pthread_mutex_destroy(&ctx->ioJobsMutex);
    }
    assert(ctx->file == NULL);
    for(i=0; i<ctx->availableJobsCount; i++) {
        IOJob_t* job = (IOJob_t*) ctx->availableJobs[i];
        free(job->buffer);
        free(job);
    }
}

/* AIO_IOPool_acquireJob:
 * Returns an available io job to be used for a future io. */
static IOJob_t* AIO_IOPool_acquireJob(IOPoolCtx_t* ctx) {
    IOJob_t *job;
    assert(ctx->file != NULL || ctx->prefs->testMode);
    AIO_IOPool_lockJobsMutex(ctx);
    assert(ctx->availableJobsCount > 0);
    job = (IOJob_t*) ctx->availableJobs[--ctx->availableJobsCount];
    AIO_IOPool_unlockJobsMutex(ctx);
    job->usedBufferSize = 0;
    job->file = ctx->file;
    job->offset = 0;
    return job;
}


/* AIO_IOPool_setFile:
 * Sets the destination file for future files in the pool.
 * Requires completion of all queued jobs and release of all otherwise acquired jobs. */
static void AIO_IOPool_setFile(IOPoolCtx_t* ctx, FILE* file) {
    assert(ctx!=NULL);
    AIO_IOPool_join(ctx);
    assert(ctx->availableJobsCount == ctx->totalIoJobs);
    ctx->file = file;
}

static FILE* AIO_IOPool_getFile(const IOPoolCtx_t* ctx) {
    return ctx->file;
}

/* AIO_IOPool_enqueueJob:
 * Enqueues an io job for execution.
 * The queued job shouldn't be used directly after queueing it. */
static void AIO_IOPool_enqueueJob(IOJob_t* job) {
    IOPoolCtx_t* const ctx = (IOPoolCtx_t *)job->ctx;
    if(AIO_IOPool_threadPoolActive(ctx))
        POOL_add(ctx->threadPool, ctx->poolFunction, job);
    else
        ctx->poolFunction(job);
}

/* ***********************************
 *  WritePool implementation
 *************************************/

/* AIO_WritePool_acquireJob:
 * Returns an available write job to be used for a future write. */
IOJob_t* AIO_WritePool_acquireJob(WritePoolCtx_t* ctx) {
    return AIO_IOPool_acquireJob(&ctx->base);
}

/* AIO_WritePool_enqueueAndReacquireWriteJob:
 * Queues a write job for execution and acquires a new one.
 * After execution `job`'s pointed value would change to the newly acquired job.
 * Make sure to set `usedBufferSize` to the wanted length before call.
 * The queued job shouldn't be used directly after queueing it. */
void AIO_WritePool_enqueueAndReacquireWriteJob(IOJob_t **job) {
    AIO_IOPool_enqueueJob(*job);
    *job = AIO_IOPool_acquireJob((IOPoolCtx_t *)(*job)->ctx);
}

/* AIO_WritePool_sparseWriteEnd:
 * Ends sparse writes to the current file.
 * Blocks on completion of all current write jobs before executing. */
void AIO_WritePool_sparseWriteEnd(WritePoolCtx_t* ctx) {
    assert(ctx != NULL);
    AIO_IOPool_join(&ctx->base);
    AIO_fwriteSparseEnd(ctx->base.prefs, ctx->base.file, ctx->storedSkips);
    ctx->storedSkips = 0;
}

/* AIO_WritePool_setFile:
 * Sets the destination file for future writes in the pool.
 * Requires completion of all queues write jobs and release of all otherwise acquired jobs.
 * Also requires ending of sparse write if a previous file was used in sparse mode. */
void AIO_WritePool_setFile(WritePoolCtx_t* ctx, FILE* file) {
    AIO_IOPool_setFile(&ctx->base, file);
    assert(ctx->storedSkips == 0);
}

/* AIO_WritePool_getFile:
 * Returns the file the writePool is currently set to write to. */
FILE* AIO_WritePool_getFile(const WritePoolCtx_t* ctx) {
    return AIO_IOPool_getFile(&ctx->base);
}

/* AIO_WritePool_releaseIoJob:
 * Releases an acquired job back to the pool. Doesn't execute the job. */
void AIO_WritePool_releaseIoJob(IOJob_t* job) {
    AIO_IOPool_releaseIoJob(job);
}

/* AIO_WritePool_closeFile:
 * Ends sparse write and closes the writePool's current file and sets the file to NULL.
 * Requires completion of all queues write jobs and release of all otherwise acquired jobs.  */
int AIO_WritePool_closeFile(WritePoolCtx_t* ctx) {
    FILE* const dstFile = ctx->base.file;
    assert(dstFile!=NULL || ctx->base.prefs->testMode!=0);
    AIO_WritePool_sparseWriteEnd(ctx);
    AIO_IOPool_setFile(&ctx->base, NULL);
    return fclose(dstFile);
}

/* AIO_WritePool_executeWriteJob:
 * Executes a write job synchronously. Can be used as a function for a thread pool. */
static void AIO_WritePool_executeWriteJob(void* opaque){
    IOJob_t* const job = (IOJob_t*) opaque;
    WritePoolCtx_t* const ctx = (WritePoolCtx_t*) job->ctx;
    ctx->storedSkips = AIO_fwriteSparse(job->file, job->buffer, job->usedBufferSize, ctx->base.prefs, ctx->storedSkips);
    AIO_IOPool_releaseIoJob(job);
}

/* AIO_WritePool_create:
 * Allocates and sets and a new write pool including its included jobs. */
WritePoolCtx_t* AIO_WritePool_create(const FIO_prefs_t* prefs, size_t bufferSize) {
    WritePoolCtx_t* const ctx = (WritePoolCtx_t*) malloc(sizeof(WritePoolCtx_t));
    if(!ctx) EXM_THROW(100, "Allocation error : not enough memory");
    AIO_IOPool_init(&ctx->base, prefs, AIO_WritePool_executeWriteJob, bufferSize);
    ctx->storedSkips = 0;
    return ctx;
}

/* AIO_WritePool_free:
 * Frees and releases a writePool and its resources. Closes destination file if needs to. */
void AIO_WritePool_free(WritePoolCtx_t* ctx) {
    /* Make sure we finish all tasks and then free the resources */
    if(AIO_WritePool_getFile(ctx))
        AIO_WritePool_closeFile(ctx);
    AIO_IOPool_destroy(&ctx->base);
    assert(ctx->storedSkips==0);
    free(ctx);
}

/* AIO_WritePool_setAsync:
 * Allows (de)activating async mode, to be used when the expected overhead
 * of asyncio costs more than the expected gains. */
void AIO_WritePool_setAsync(WritePoolCtx_t* ctx, int async) {
    AIO_IOPool_setThreaded(&ctx->base, async);
}


/* ***********************************
 *  ReadPool implementation
 *************************************/
static void AIO_ReadPool_releaseAllCompletedJobs(ReadPoolCtx_t* ctx) {
    int i;
    for(i=0; i<ctx->completedJobsCount; i++) {
        IOJob_t* job = (IOJob_t*) ctx->completedJobs[i];
        AIO_IOPool_releaseIoJob(job);
    }
    ctx->completedJobsCount = 0;
}

static void AIO_ReadPool_addJobToCompleted(IOJob_t* job) {
    ReadPoolCtx_t* const ctx = (ReadPoolCtx_t *)job->ctx;
    AIO_IOPool_lockJobsMutex(&ctx->base);
    assert(ctx->completedJobsCount < MAX_IO_JOBS);
    ctx->completedJobs[ctx->completedJobsCount++] = job;
    if(AIO_IOPool_threadPoolActive(&ctx->base)) {
        ZSTD_pthread_cond_signal(&ctx->jobCompletedCond);
    }
    AIO_IOPool_unlockJobsMutex(&ctx->base);
}

/* AIO_ReadPool_findNextWaitingOffsetCompletedJob_locked:
 * Looks through the completed jobs for a job matching the waitingOnOffset and returns it,
 * if job wasn't found returns NULL.
 * IMPORTANT: assumes ioJobsMutex is locked. */
static IOJob_t* AIO_ReadPool_findNextWaitingOffsetCompletedJob_locked(ReadPoolCtx_t* ctx) {
    IOJob_t *job = NULL;
    int i;
    /* This implementation goes through all completed jobs and looks for the one matching the next offset.
     * While not strictly needed for a single threaded reader implementation (as in such a case we could expect
     * reads to be completed in order) this implementation was chosen as it better fits other asyncio
     * interfaces (such as io_uring) that do not provide promises regarding order of completion. */
    for (i=0; i<ctx->completedJobsCount; i++) {
        job = (IOJob_t *) ctx->completedJobs[i];
        if (job->offset == ctx->waitingOnOffset) {
            ctx->completedJobs[i] = ctx->completedJobs[--ctx->completedJobsCount];
            return job;
        }
    }
    return NULL;
}

/* AIO_ReadPool_numReadsInFlight:
 * Returns the number of IO read jobs currently in flight. */
static size_t AIO_ReadPool_numReadsInFlight(ReadPoolCtx_t* ctx) {
    const size_t jobsHeld = (ctx->currentJobHeld==NULL ? 0 : 1);
    return ctx->base.totalIoJobs - (ctx->base.availableJobsCount + ctx->completedJobsCount + jobsHeld);
}

/* AIO_ReadPool_getNextCompletedJob:
 * Returns a completed IOJob_t for the next read in line based on waitingOnOffset and advances waitingOnOffset.
 * Would block. */
static IOJob_t* AIO_ReadPool_getNextCompletedJob(ReadPoolCtx_t* ctx) {
    IOJob_t *job = NULL;
    AIO_IOPool_lockJobsMutex(&ctx->base);

    job = AIO_ReadPool_findNextWaitingOffsetCompletedJob_locked(ctx);

    /* As long as we didn't find the job matching the next read, and we have some reads in flight continue waiting */
    while (!job && (AIO_ReadPool_numReadsInFlight(ctx) > 0)) {
        assert(ctx->base.threadPool != NULL); /* we shouldn't be here if we work in sync mode */
        ZSTD_pthread_cond_wait(&ctx->jobCompletedCond, &ctx->base.ioJobsMutex);
        job = AIO_ReadPool_findNextWaitingOffsetCompletedJob_locked(ctx);
    }

    if(job) {
        assert(job->offset == ctx->waitingOnOffset);
        ctx->waitingOnOffset += job->usedBufferSize;
    }

    AIO_IOPool_unlockJobsMutex(&ctx->base);
    return job;
}


/* AIO_ReadPool_executeReadJob:
 * Executes a read job synchronously. Can be used as a function for a thread pool. */
static void AIO_ReadPool_executeReadJob(void* opaque){
    IOJob_t* const job = (IOJob_t*) opaque;
    ReadPoolCtx_t* const ctx = (ReadPoolCtx_t *)job->ctx;
    if(ctx->reachedEof) {
        job->usedBufferSize = 0;
        AIO_ReadPool_addJobToCompleted(job);
        return;
    }
    job->usedBufferSize = fread(job->buffer, 1, job->bufferSize, job->file);
    if(job->usedBufferSize < job->bufferSize) {
        if(ferror(job->file)) {
            EXM_THROW(37, "Read error");
        } else if(feof(job->file)) {
            ctx->reachedEof = 1;
        } else {
            EXM_THROW(37, "Unexpected short read");
        }
    }
    AIO_ReadPool_addJobToCompleted(job);
}

static void AIO_ReadPool_enqueueRead(ReadPoolCtx_t* ctx) {
    IOJob_t* const job = AIO_IOPool_acquireJob(&ctx->base);
    job->offset = ctx->nextReadOffset;
    ctx->nextReadOffset += job->bufferSize;
    AIO_IOPool_enqueueJob(job);
}

static void AIO_ReadPool_startReading(ReadPoolCtx_t* ctx) {
    int i;
    for (i = 0; i < ctx->base.availableJobsCount; i++) {
        AIO_ReadPool_enqueueRead(ctx);
    }
}

/* AIO_ReadPool_setFile:
 * Sets the source file for future read in the pool. Initiates reading immediately if file is not NULL.
 * Waits for all current enqueued tasks to complete if a previous file was set. */
void AIO_ReadPool_setFile(ReadPoolCtx_t* ctx, FILE* file) {
    assert(ctx!=NULL);
    AIO_IOPool_join(&ctx->base);
    AIO_ReadPool_releaseAllCompletedJobs(ctx);
    if (ctx->currentJobHeld) {
        AIO_IOPool_releaseIoJob((IOJob_t *)ctx->currentJobHeld);
        ctx->currentJobHeld = NULL;
    }
    AIO_IOPool_setFile(&ctx->base, file);
    ctx->nextReadOffset = 0;
    ctx->waitingOnOffset = 0;
    ctx->srcBuffer = ctx->coalesceBuffer;
    ctx->srcBufferLoaded = 0;
    ctx->reachedEof = 0;
    if(file != NULL)
        AIO_ReadPool_startReading(ctx);
}

/* AIO_ReadPool_create:
 * Allocates and sets and a new readPool including its included jobs.
 * bufferSize should be set to the maximal buffer we want to read at a time, will also be used
 * as our basic read size. */
ReadPoolCtx_t* AIO_ReadPool_create(const FIO_prefs_t* prefs, size_t bufferSize) {
    ReadPoolCtx_t* const ctx = (ReadPoolCtx_t*) malloc(sizeof(ReadPoolCtx_t));
    if(!ctx) EXM_THROW(100, "Allocation error : not enough memory");
    AIO_IOPool_init(&ctx->base, prefs, AIO_ReadPool_executeReadJob, bufferSize);

    ctx->coalesceBuffer = (U8*) malloc(bufferSize * 2);
    ctx->srcBuffer = ctx->coalesceBuffer;
    ctx->srcBufferLoaded = 0;
    ctx->completedJobsCount = 0;
    ctx->currentJobHeld = NULL;

    if(ctx->base.threadPool)
        if (ZSTD_pthread_cond_init(&ctx->jobCompletedCond, NULL))
            EXM_THROW(103,"Failed creating jobCompletedCond cond");

    return ctx;
}

/* AIO_ReadPool_free:
 * Frees and releases a readPool and its resources. Closes source file. */
void AIO_ReadPool_free(ReadPoolCtx_t* ctx) {
    if(AIO_ReadPool_getFile(ctx))
        AIO_ReadPool_closeFile(ctx);
    if(ctx->base.threadPool)
        ZSTD_pthread_cond_destroy(&ctx->jobCompletedCond);
    AIO_IOPool_destroy(&ctx->base);
    free(ctx->coalesceBuffer);
    free(ctx);
}

/* AIO_ReadPool_consumeBytes:
 * Consumes byes from srcBuffer's beginning and updates srcBufferLoaded accordingly. */
void AIO_ReadPool_consumeBytes(ReadPoolCtx_t* ctx, size_t n) {
    assert(n <= ctx->srcBufferLoaded);
    ctx->srcBufferLoaded -= n;
    ctx->srcBuffer += n;
}

/* AIO_ReadPool_releaseCurrentlyHeldAndGetNext:
 * Release the current held job and get the next one, returns NULL if no next job available. */
static IOJob_t* AIO_ReadPool_releaseCurrentHeldAndGetNext(ReadPoolCtx_t* ctx) {
    if (ctx->currentJobHeld) {
        AIO_IOPool_releaseIoJob((IOJob_t *)ctx->currentJobHeld);
        ctx->currentJobHeld = NULL;
        AIO_ReadPool_enqueueRead(ctx);
    }
    ctx->currentJobHeld = AIO_ReadPool_getNextCompletedJob(ctx);
    return (IOJob_t*) ctx->currentJobHeld;
}

/* AIO_ReadPool_fillBuffer:
 * Tries to fill the buffer with at least n or jobBufferSize bytes (whichever is smaller).
 * Returns if srcBuffer has at least the expected number of bytes loaded or if we've reached the end of the file.
 * Return value is the number of bytes added to the buffer.
 * Note that srcBuffer might have up to 2 times jobBufferSize bytes. */
size_t AIO_ReadPool_fillBuffer(ReadPoolCtx_t* ctx, size_t n) {
    IOJob_t *job;
    int useCoalesce = 0;
    if(n > ctx->base.jobBufferSize)
        n = ctx->base.jobBufferSize;

    /* We are good, don't read anything */
    if (ctx->srcBufferLoaded >= n)
        return 0;

    /* We still have bytes loaded, but not enough to satisfy caller. We need to get the next job
     * and coalesce the remaining bytes with the next job's buffer */
    if (ctx->srcBufferLoaded > 0) {
        useCoalesce = 1;
        memcpy(ctx->coalesceBuffer, ctx->srcBuffer, ctx->srcBufferLoaded);
        ctx->srcBuffer = ctx->coalesceBuffer;
    }

    /* Read the next chunk */
    job = AIO_ReadPool_releaseCurrentHeldAndGetNext(ctx);
    if(!job)
        return 0;
    if(useCoalesce) {
        assert(ctx->srcBufferLoaded + job->usedBufferSize <= 2*ctx->base.jobBufferSize);
        memcpy(ctx->coalesceBuffer + ctx->srcBufferLoaded, job->buffer, job->usedBufferSize);
        ctx->srcBufferLoaded += job->usedBufferSize;
    }
    else {
        ctx->srcBuffer = (U8 *) job->buffer;
        ctx->srcBufferLoaded = job->usedBufferSize;
    }
    return job->usedBufferSize;
}

/* AIO_ReadPool_consumeAndRefill:
 * Consumes the current buffer and refills it with bufferSize bytes. */
size_t AIO_ReadPool_consumeAndRefill(ReadPoolCtx_t* ctx) {
    AIO_ReadPool_consumeBytes(ctx, ctx->srcBufferLoaded);
    return AIO_ReadPool_fillBuffer(ctx, ctx->base.jobBufferSize);
}

/* AIO_ReadPool_getFile:
 * Returns the current file set for the read pool. */
FILE* AIO_ReadPool_getFile(const ReadPoolCtx_t* ctx) {
    return AIO_IOPool_getFile(&ctx->base);
}

/* AIO_ReadPool_closeFile:
 * Closes the current set file. Waits for all current enqueued tasks to complete and resets state. */
int AIO_ReadPool_closeFile(ReadPoolCtx_t* ctx) {
    FILE* const file = AIO_ReadPool_getFile(ctx);
    AIO_ReadPool_setFile(ctx, NULL);
    return fclose(file);
}

/* AIO_ReadPool_setAsync:
 * Allows (de)activating async mode, to be used when the expected overhead
 * of asyncio costs more than the expected gains. */
void AIO_ReadPool_setAsync(ReadPoolCtx_t* ctx, int async) {
    AIO_IOPool_setThreaded(&ctx->base, async);
}
