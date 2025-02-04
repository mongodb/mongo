/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#pragma once

#include <aws/core/Core_EXPORTS.h>
#include <aws/core/VersionConfig.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/monitoring/MonitoringManager.h>
#include <aws/core/utils/crypto/Factories.h>
#include <aws/core/utils/logging/CRTLogSystem.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <aws/core/utils/memory/MemorySystemInterface.h>
#include <aws/crt/io/Bootstrap.h>
#include <aws/crt/io/TlsOptions.h>

namespace Aws {
static const char* DEFAULT_LOG_PREFIX = "aws_sdk_";

/**
 * SDK wide options for logging
 */
struct LoggingOptions {
  LoggingOptions() = default;

  /**
   * Defaults to Off, if this is set to something else, then logging will be turned on and logLevel will be passed to the logger
   */
  Aws::Utils::Logging::LogLevel logLevel = Aws::Utils::Logging::LogLevel::Off;

  /**
   * Defaults to aws_sdk_. This will only be used if the default logger is used.
   */
  const char* defaultLogPrefix = DEFAULT_LOG_PREFIX;

  /**
   * Defaults to empty, if logLevel has been set and this field is empty, then the default log system will be used.
   * otherwise, we will call this closure to create a logger
   */
  std::function<std::shared_ptr<Aws::Utils::Logging::LogSystemInterface>()> logger_create_fn;

  /**
   * Defaults to empty, if logLevel has been set and this field is empty, then the default CRT log system will be used.
   * The default CRT log system will redirect all logs from common runtime libraries (CRT) to C++ SDK with the same log level and
   * formatting.
   */
  std::function<std::shared_ptr<Aws::Utils::Logging::CRTLogSystemInterface>()> crt_logger_create_fn;
};

/**
 * SDK wide options for memory management
 */
struct MemoryManagementOptions {
  MemoryManagementOptions() = default;

  /**
   * Defaults to nullptr. If custom memory management is being used and this hasn't been set then the default memory
   * manager will be used. If this has been set and custom memory management has been turned on, then this will be installed
   * at startup time.
   */
  Aws::Utils::Memory::MemorySystemInterface* memoryManager = nullptr;
};

/**
 * SDK wide options for I/O: client bootstrap and TLS connection options
 */
struct IoOptions {
  std::function<std::shared_ptr<Aws::Crt::Io::ClientBootstrap>()> clientBootstrap_create_fn;
  std::function<std::shared_ptr<Aws::Crt::Io::TlsConnectionOptions>()> tlsConnectionOptions_create_fn;
};

/**
 * SDK wide options for http
 */
struct HttpOptions {
  HttpOptions() = default;

  /**
   * Defaults to empty, if this is set, then the result of your closure will be installed and used instead of the system defaults
   */
  std::function<std::shared_ptr<Aws::Http::HttpClientFactory>()> httpClientFactory_create_fn;
  /**
   * libCurl infects everything with its global state. If it is being used then we automatically initialize and clean it up.
   * If this is a problem for you, set this to false. If you manually initialize libcurl please add the option CURL_GLOBAL_ALL to your init
   * call.
   */
  bool initAndCleanupCurl = true;
  /**
   * Installs a global SIGPIPE handler that logs the error and prevents it from terminating the current process.
   * This can be used on operating systems on which CURL is being used. In some situations CURL cannot avoid
   * triggering a SIGPIPE.
   * For more information see: https://curl.haxx.se/libcurl/c/CURLOPT_NOSIGNAL.html
   * NOTE: CURLOPT_NOSIGNAL is already being set.
   */
  bool installSigPipeHandler = false;
  /**
   * Disable legacy URL encoding that leaves `$&,:@=` unescaped for legacy purposes.
   */
  bool compliantRfc3986Encoding = false;
  /**
   * When constructing Path segments in a URI preserve path separators instead of collapsing
   * slashes. This is useful for aligning with other SDKs and tools on key path for S3 objects
   * as currently the C++ SDK sanitizes the path.
   *
   * TODO: In the next major release, this will become the default to align better with other SDKs.
   */
  bool preservePathSeparators = false;
};

/**
 * SDK wide options for crypto
 */
struct CryptoOptions {
  CryptoOptions() = default;

  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::HashFactory>()> md5Factory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::HashFactory>()> sha1Factory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::HashFactory>()> sha256Factory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::HMACFactory>()> sha256HMACFactory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::SymmetricCipherFactory>()> aes_CBCFactory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::SymmetricCipherFactory>()> aes_CTRFactory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::SymmetricCipherFactory>()> aes_GCMFactory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::SymmetricCipherFactory>()> aes_KeyWrapFactory_create_fn;
  /**
   * If set, this closure will be used to create and install the factory.
   */
  std::function<std::shared_ptr<Aws::Utils::Crypto::SecureRandomFactory>()> secureRandomFactory_create_fn;
  /**
   * OpenSSL infects everything with its global state. If it is being used then we automatically initialize and clean it up.
   * If this is a problem for you, set this to false. Be aware that if you don't use our init and cleanup and you are using
   * crypto functionality, you are responsible for installing thread locking, and loading strings and error messages.
   */
  bool initAndCleanupOpenSSL = true;
};

/**
 * MonitoringOptions is used to set up monitoring functionalities globally and(or) for users to customize monitoring listeners.
 */
struct MonitoringOptions {
  /**
   * These factory functions will be used to try to create customized monitoring listener factories, then be used to create monitoring
   * listener instances. Based on functions and factory's implementation, it may fail to create an instance. If a function failed to create
   * factory or a created factory failed to create an instance, SDK just ignore it. By default, SDK will try to create a default Client Side
   * Monitoring Listener.
   */
  std::vector<Aws::Monitoring::MonitoringFactoryCreateFunction> customizedMonitoringFactory_create_fn;
};

/**
 * You may notice that instead of taking pointers directly to your factories, we take a closure. This is because
 * if you have installed custom memory management, the allocation for your factories needs to happen after
 * the memory system has been initialized and shutdown needs to happen prior to the memory management being shutdown.
 *
 * Common Recipes:
 *
 * Just use defaults:
 *
 * SDKOptions options;
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 *
 * Turn logging on using the default logger:
 *
 * SDKOptions options;
 * options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 *
 * Install custom memory manager:
 *
 * MyMemoryManager memoryManager;
 *
 * SDKOptions options;
 * options.memoryManagementOptions.memoryManager = &memoryManager;
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 *
 * Override default http client factory
 *
 * SDKOptions options;
 * options.httpOptions.httpClientFactory_create_fn = [](){ return Aws::MakeShared<MyCustomHttpClientFactory>("ALLOC_TAG", arg1); };
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 */
struct SDKOptions {
  SDKOptions() = default;
  /**
   * SDK wide options for I/O: client bootstrap and TLS connection options
   */
  IoOptions ioOptions = {};
  /**
   * SDK wide options for logging
   */
  LoggingOptions loggingOptions = {};
  /**
   * SDK wide options for memory management
   */
  MemoryManagementOptions memoryManagementOptions = {};
  /**
   * SDK wide options for http
   */
  HttpOptions httpOptions = {};
  /**
   * SDK wide options for crypto
   */
  CryptoOptions cryptoOptions = {};

  /**
   * Options used to set up customized monitoring implementations
   * Put your monitoring factory in a closure (a create factory function) and put all closures in a vector.
   * Basic usage can be found in aws-cpp-sdk-core-tests/monitoring/MonitoringTest.cpp
   */
  MonitoringOptions monitoringOptions = {};

  struct SDKVersion {
    unsigned char major = AWS_SDK_VERSION_MAJOR;
    unsigned char minor = AWS_SDK_VERSION_MINOR;
    unsigned short patch = AWS_SDK_VERSION_PATCH;
  } sdkVersion = {};
};

/*
 * Initialize SDK wide state for the SDK. This method must be called before doing anything else with this library.
 *
 * Common Recipes:
 *
 * Just use defaults:
 *
 * SDKOptions options;
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 *
 * Turn logging on using the default logger:
 *
 * SDKOptions options;
 * options.loggingOptions.logLevel = Aws::Utils::Logging::LogLevel::Info;
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 *
 * Install custom memory manager:
 *
 * MyMemoryManager memoryManager;
 *
 * SDKOptions options;
 * options.memoryManagementOptions.memoryManager = &memoryManager;
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 *
 * Override default http client factory
 *
 * SDKOptions options;
 * options.httpOptions.httpClientFactory_create_fn = [](){ return Aws::MakeShared<MyCustomHttpClientFactory>("ALLOC_TAG", arg1); };
 * Aws::InitAPI(options);
 * .....
 * Aws::ShutdownAPI(options);
 */
AWS_CORE_API void InitAPI(const SDKOptions& options);

/**
 * Shutdown SDK wide state for the SDK. This method must be called when you are finished using the SDK.
 * Notes:
 * 1) Please call this from the same thread from which InitAPI() has been called (use a dedicated thread
 *    if necessary). This avoids problems in initializing the dependent Common RunTime C libraries.
 * 2) Do not call any other SDK methods after calling ShutdownAPI.
 */
AWS_CORE_API void ShutdownAPI(const SDKOptions& options);
}  // namespace Aws
