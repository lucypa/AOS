#include <stdint.h>

/* This library is taken directly from musllibc. 
   It is NOT thread safe. */
void *sos_malloc(unsigned nbytes);
void *sos_calloc(unsigned nelem, unsigned size);
void sos_free(void *p);
