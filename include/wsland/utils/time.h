#ifndef WSLAND_UTILS_TIME_H
#define WSLAND_UTILS_TIME_H

#include <time.h>
#include <stdint.h>

int64_t timespec_to_msec(const struct timespec *a);

#endif
