// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace common
{

/**
 * Thread instrumentation interface.
 * When the opentelemetry-cpp library executes internal threads,
 * the application linking with the opentelemetry-cpp library
 * may want to have some control on how the thread executes.
 *
 * There are many different use cases for this,
 * listing a few to illustrate.
 *
 * (1) The application may need to initialize thread local storage,
 * in an application specific way, because application code that
 * relies on thread local storage will be executed in the thread code path.
 * For example, custom samplers for traces, or custom observable instruments
 * for metrics, may expect specific thread local storage keys to exist.
 *
 * (2) The application may want opentelemetry-cpp threads to be observable
 * in a given way when exposed to the operating system.
 * For example, a linux application may want to give a name to
 * opentelemetry-cpp threads,
 * using [pthread_setname_np(3)](https://man7.org/linux/man-pages/man3/pthread_setname_np.3.html),
 * to help troubleshooting.
 *
 * (3) The application may want specific opentelemetry-cpp threads to use
 * application defined specific named network.
 * For example, a linux application may want to use
 * [setns(2)](https://man7.org/linux/man-pages/man2/setns.2.html)
 * on a per exporter basis, so that different exporters uses different networks.
 *
 * (4) The application may want to bind specific opentelemetry-cpp threads
 * to specific CPUs, for performance reasons.
 * For example, a linux application may want to use
 * [sched_setaffinity(2)](https://man7.org/linux/man-pages/man2/sched_setaffinity.2.html)
 * on a per thread basis.
 *
 * Providing dedicated opentelemetry-cpp interfaces in the SDK or exporters,
 * to support these use cases, is not practical, because the code involved
 * is highly platform dependent and use case dependent.
 *
 * Instead, the opentelemetry-cpp library provide hooks for applications
 * to implement their own, arbitrary, platform specific, logic.
 * This is done by implementing the ThreadInstrumentation interface
 * in the application, and providing a given ThreadInstrumentation object
 * when initializing the SDK or exporters.
 *
 * The opentelemetry-cpp library calls the following extension points,
 * when a ThreadInstrumentation is provided.
 *
 * Upon thread creation and termination, the methods OnStart() and OnEnd()
 * are invoked, respectively.
 *
 * When a thread is to block and wait, for example on a timer,
 * the methods BeforeWait() and AfterWait() are invoked.
 *
 * When a thread is to perform a chunk of work,
 * for example to process all the available data in an exporter,
 * the methods BeforeLoad() and AfterLoad() are invoked.
 */
class ThreadInstrumentation
{
public:
  ThreadInstrumentation()          = default;
  virtual ~ThreadInstrumentation() = default;

/*
 * This feature is experimental, protected by a _PREVIEW flag.
 */
#ifdef ENABLE_THREAD_INSTRUMENTATION_PREVIEW
  virtual void OnStart() {}
  virtual void OnEnd() {}
  virtual void BeforeWait() {}
  virtual void AfterWait() {}
  virtual void BeforeLoad() {}
  virtual void AfterLoad() {}
#endif /* ENABLE_THREAD_INSTRUMENTATION_PREVIEW */
};

}  // namespace common
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
