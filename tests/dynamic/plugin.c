/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdint.h>
static _Thread_local int local = 11;
static int ready;
__attribute__((constructor)) static void initialize(void)
{
    ready = 7;
}
int plugin_value(int add)
{
    int old = local;
    local += add;
    return ready * 1000 + old;
}
