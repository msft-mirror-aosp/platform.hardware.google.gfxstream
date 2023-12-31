// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdlib.h>

#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <cstring>

#if defined(__Fuchsia__)
#include <lib/syslog/global.h>
#else
#include <libgen.h>
#endif

#include "cutils/log.h"
#include "cutils/properties.h"

extern "C" {

#if !defined(__Fuchsia__)
static void linux_log_prefix(const char *prefix, const char *file, int line, const char *format,
                             va_list ap, ...)
{
  char buf[50];
  char *dup = strdup(file);
  if (!dup)
    return;

  snprintf(buf, sizeof(buf), "[%s(%d)]", basename(dup), line);
  fprintf(stderr, "%s\n", buf);
  vfprintf(stderr, format, ap);

  free(dup);
}
#endif

int property_get(const char* key, char* value, const char* default_value) {
  return 0;
}

int __android_log_print(int priority, const char* tag, const char* file,
                        int line, const char* format, ...) {
  const char* local_tag = tag;
  if (!local_tag) {
    local_tag = "<NO_TAG>";
  }

  va_list ap;
  va_start(ap, format);
#if defined(__Fuchsia__)
  switch (priority) {
    case ANDROID_LOG_VERBOSE:
    case ANDROID_LOG_DEBUG:
      FX_LOGVF(DEBUG, local_tag, file, line, format, ap);
      break;
    case ANDROID_LOG_WARN:
      FX_LOGVF(WARNING, local_tag, file, line, format, ap);
      break;
    case ANDROID_LOG_ERROR:
      FX_LOGVF(ERROR, local_tag, file, line, format, ap);
      break;
    case ANDROID_LOG_FATAL:
      FX_LOGVF(FATAL, local_tag, file, line, format, ap);
      break;
    case ANDROID_LOG_INFO:
    default:
      FX_LOGVF(INFO, local_tag, file, line, format, ap);
      break;
  }
#else
  linux_log_prefix(local_tag, file, line, format, ap);
#endif

  return 1;
}

void __android_log_assert(const char* condition, const char* tag,
                          const char* file, int line, const char* format, ...) {
  const char* local_tag = tag;
  if (!local_tag) {
    local_tag = "<NO_TAG>";
  }
  va_list ap;
  va_start(ap, format);
#if defined(__Fuchsia__)
  FX_LOGVF(ERROR, local_tag, file, line, format, ap);
#else
  linux_log_prefix(local_tag, file, line, format, ap);
#endif

  va_end(ap);

  abort();
}

int sync_wait(int fd, int timeout) { return -1; }
}
