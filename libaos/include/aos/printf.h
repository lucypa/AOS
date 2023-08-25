#pragma once

#include <stdarg.h>

typedef void (*vputchar_t)(char c);
void update_vputchar(vputchar_t vputchar);
int sos_sprintf(char *buf, const char *fmt, ...);
void sos_printf(const char *fmt, ...);
