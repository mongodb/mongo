/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifdef JS_STRUCTURED_SPEW

#  include "util/StructuredSpewer.h"

#  include "mozilla/Sprintf.h"

#  include "util/GetPidProvider.h"  // getpid()
#  include "util/Text.h"
#  include "vm/JSContext.h"
#  include "vm/JSScript.h"

using namespace js;

const StructuredSpewer::NameArray StructuredSpewer::names_ = {
#  define STRUCTURED_CHANNEL(name) #  name,
    STRUCTURED_CHANNEL_LIST(STRUCTURED_CHANNEL)
#  undef STRUCTURED_CHANNEL
};

// Choose a sensible default spew directory.
//
// The preference here is to use the current working directory,
// except on Android.
#  ifndef DEFAULT_SPEW_DIRECTORY
#    if defined(_WIN32)
#      define DEFAULT_SPEW_DIRECTORY "."
#    elif defined(__ANDROID__)
#      define DEFAULT_SPEW_DIRECTORY "/data/local/tmp"
#    else
#      define DEFAULT_SPEW_DIRECTORY "."
#    endif
#  endif

bool StructuredSpewer::ensureInitializationAttempted() {
  if (!outputInitializationAttempted_) {
    char filename[2048] = {0};
    // For ease of use with Treeherder
    if (getenv("SPEW_UPLOAD") && getenv("MOZ_UPLOAD_DIR")) {
      SprintfLiteral(filename, "%s/spew_output", getenv("MOZ_UPLOAD_DIR"));
    } else if (getenv("SPEW_FILE")) {
      SprintfLiteral(filename, "%s", getenv("SPEW_FILE"));
    } else {
      SprintfLiteral(filename, "%s/spew_output", DEFAULT_SPEW_DIRECTORY);
    }
    tryToInitializeOutput(filename);
    // We can't use the intialization state of the Fprinter, as it is not
    // marked as initialized in a case where we cannot open the output, so
    // we track the attempt separately.
    outputInitializationAttempted_ = true;
  }

  return json_.isSome();
}

void StructuredSpewer::tryToInitializeOutput(const char* path) {
  static mozilla::Atomic<uint32_t, mozilla::ReleaseAcquire> threadCounter;

  char suffix_path[2048] = {0};
  SprintfLiteral(suffix_path, "%s.%d.%u", path, getpid(), threadCounter++);

  if (!output_.init(suffix_path)) {
    // Returning here before we've emplaced the JSONPrinter
    // means this is effectively disabled, but fail earlier
    // we also disable the channel.
    selectedChannel_.disableAllChannels();
    return;
  }

  // These logs are structured as a JSON array.
  json_.emplace(output_);
  json_->beginList();
}

// Treat pattern like a glob, and return true if pattern exists
// in the script's name or filename or line number.
//
// This is the most basic matching I can imagine
static bool MatchJSScript(JSScript* script, const char* pattern) {
  if (!pattern) {
    return false;
  }

  char signature[2048] = {0};
  SprintfLiteral(signature, "%s:%u:%u", script->filename(), script->lineno(),
                 script->column());

  // Trivial containment match.
  char* result = strstr(signature, pattern);

  return result != nullptr;
}

bool StructuredSpewer::enabled(JSScript* script) {
  if (spewingEnabled_ == 0) {
    return false;
  }

  static const char* pattern = getenv("SPEW_FILTER");
  if (!pattern || MatchJSScript(script, pattern)) {
    return true;
  }
  return false;
}

bool StructuredSpewer::enabled(JSContext* cx, const JSScript* script,
                               SpewChannel channel) const {
  if (script && !script->spewEnabled()) {
    return false;
  }
  return cx->spewer().enabled(channel);
}

// Attempt to setup a common header for objects based on script/channel.
//
// Returns true if the spewer is prepared for more input
void StructuredSpewer::startObject(JSContext* cx, const JSScript* script,
                                   SpewChannel channel) {
  MOZ_ASSERT(json_.isSome());

  JSONPrinter& json = json_.ref();

  json.beginObject();
  json.property("channel", getName(channel));
  if (script) {
    json.beginObjectProperty("location");
    json.property("filename", script->filename());
    json.property("line", script->lineno());
    json.property("column", script->column());
    json.endObject();
  }
}

/* static */
void StructuredSpewer::spew(JSContext* cx, SpewChannel channel, const char* fmt,
                            ...) {
  // Because we don't have a script here, use the singleton's
  // filter to determine if the channel is active.
  if (!cx->spewer().enabled(channel)) {
    return;
  }

  if (!cx->spewer().ensureInitializationAttempted()) {
    return;
  }

  va_list ap;
  va_start(ap, fmt);

  MOZ_ASSERT(cx->spewer().json_.isSome());

  JSONPrinter& json = cx->spewer().json_.ref();

  json.beginObject();
  json.property("channel", getName(channel));
  json.formatProperty("message", fmt, ap);
  json.endObject();

  va_end(ap);
}

// Currently uses the exact spew flag representation as text.
void StructuredSpewer::parseSpewFlags(const char* flags) {
#  define CHECK_CHANNEL(name)                            \
    if (ContainsFlag(flags, #name)) {                    \
      selectedChannel_.enableChannel(SpewChannel::name); \
      break;                                             \
    }

  do {
    STRUCTURED_CHANNEL_LIST(CHECK_CHANNEL)
  } while (false);

#  undef CHECK_CHANNEL

  if (ContainsFlag(flags, "AtStartup")) {
    enableSpewing();
  }

  if (ContainsFlag(flags, "help")) {
    printf(
        "\n"
        "usage: SPEW=option,option,... where options can be:\n"
        "\n"
        "  help                     Dump this help message\n"
        "  channel                  Enable the selected channel from below, "
        "if\n"
        "                           more than one channel is specified, then "
        "the\n"
        "                           channel will be set whichever specified "
        "filter\n"
        "                           comes first in STRUCTURED_CHANNEL_LIST."
        "  AtStartup                Enable spewing at browser startup instead\n"
        "                           of when gecko profiling starts."
        "\n"
        " Channels: \n"
        "\n"
        // List Channels
        "  BaselineICStats          Dump the IC Entry counters during Ion "
        "analysis\n"
        "  ScriptStats              Dump statistics collected by tracelogger "
        "that\n"
        "                           is aggregated by script. Requires\n"
        "                           JS_TRACE_LOGGING=1\n"
        "  CacheIRHealthReport      Dump the CacheIR information and "
        "associated "
        "rating\n"
        // End Channel list
        "\n\n"
        "By default output goes to a file called spew_output.$PID.$THREAD\n"
        "\n"
        "Further control of the spewer can be accomplished with the below\n"
        "environment variables:\n"
        "\n"
        "   SPEW_FILE: Selects the file to write to. An absolute path.\n"
        "\n"
        "   SPEW_FILTER: A string which is matched against 'signature'\n"
        "        constructed from a JSScript, currently connsisting of \n"
        "        filename:line:col.\n"
        "\n"
        "        A JSScript matches the filter string is found in the\n"
        "        signature\n"
        "\n"
        "   SPEW_UPLOAD: If this variable is set as well as MOZ_UPLOAD_DIR,\n"
        "        output goes to $MOZ_UPLOAD_DIR/spew_output* to ease usage\n"
        "        with Treeherder.\n"

    );
    exit(0);
  }
}

AutoStructuredSpewer::AutoStructuredSpewer(JSContext* cx, SpewChannel channel,
                                           JSScript* script)
    : printer_(mozilla::Nothing()) {
  if (!cx->spewer().enabled(cx, script, channel)) {
    return;
  }

  if (!cx->spewer().ensureInitializationAttempted()) {
    return;
  }

  cx->spewer().startObject(cx, script, channel);
  printer_.emplace(&cx->spewer().json_.ref());
}

AutoSpewChannel::AutoSpewChannel(JSContext* cx, SpewChannel channel,
                                 JSScript* script)
    : cx_(cx) {
  if (!cx->spewer().enabled(cx, script, channel)) {
    wasChannelAutoSet = cx->spewer().selectedChannel_.enableChannel(channel);
  }
}

AutoSpewChannel::~AutoSpewChannel() {
  if (wasChannelAutoSet) {
    cx_->spewer().selectedChannel_.disableAllChannels();
  }
}

#endif
