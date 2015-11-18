/* Copyright (c) 2015 Google Inc
 * Davide Libenzi <dlibenzi@google.com>
 * See LICENSE for details.
 */

#pragma once

#include <stdio.h>
#include <stdarg.h>

size_t kprof_tracedata_size(void);
size_t kprof_tracedata_read(void *data, size_t size, size_t offset);
void kprof_tracedata_write(const char *pretty_buf, size_t len);
void trace_vprintk(bool btrace, const char *fmt, va_list args);
void trace_printk(bool btrace, const char *fmt, ...);
