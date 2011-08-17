#ifndef _LAMBDACHINE_CONFIG_H
#define _LAMBDACHINE_CONFIG_H

#define LC_HAS_JIT      1

#undef NDEBUG
#define DEBUG

#ifdef NDEBUG
#define LC_DEBUG_LEVEL  0
#else
#define LC_DEBUG_LEVEL  1
#endif

#endif
