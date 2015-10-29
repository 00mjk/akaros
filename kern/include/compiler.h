#pragma once

#ifdef __GNUC__

#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#else /* #ifdef __GNUC__ */

#define likely(x) (x)
#define unlikely(x) (x)

#endif /* #ifdef __GNUC__ */
