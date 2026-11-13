/* SPDX-License-Identifier: LGPL-2.1-or-later */
/* Copyright (C) 2026 Rigby Foundation */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <pthread.h>
static int (*value)(int);
static void *thread(void *unused)
{
    (void)unused;
    return (void *)(long)(value(5) == 7011 && value(0) == 7016);
}
int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    void *module = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!module) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 2;
    }
    value = (int (*)(int))dlsym(module, "plugin_value");
    if (!value) {
        fprintf(stderr, "dlsym: %s\n", dlerror());
        return 2;
    }
    int good = value(3) == 7011 && value(0) == 7014;
    pthread_t t;
    void *result = 0;
    if (pthread_create(&t, 0, thread, 0))
        return 2;
    pthread_join(t, &result);
    good = good && result == (void *)1 && value(0) == 7014;
    if (dlclose(module))
        good = 0;
    printf("DYNAMIC_RESULT constructor/dlopen/dlsym/TLS/thread %s\n", good ? "PASS" : "FAIL");
    return good ? 0 : 1;
}
