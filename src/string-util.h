#ifndef WAYNAV_STRING_UTIL_H
#define WAYNAV_STRING_UTIL_H

#include <stdbool.h>
#include <string.h>
#include <strings.h>

static inline bool streq(const char *a, const char *b) {
    return strcmp(a, b) == 0;
}

static inline bool strcaseeq(const char *a, const char *b) {
    return strcasecmp(a, b) == 0;
}

#endif
