#ifndef REMEDY_RECEIPT_CHECK_H
#define REMEDY_RECEIPT_CHECK_H

#include <stdio.h>
#include <stdlib.h>

static inline void remedy_receipt_fail(
    const char* expression,
    const char* file,
    int line) {
    fprintf(stderr, "%s:%d: receipt check failed: %s\n", file, line, expression);
    abort();
}

#define REMEDY_RECEIPT_CHECK(expression) \
    ((expression) ? (void)0 : remedy_receipt_fail(#expression, __FILE__, __LINE__))

#endif
