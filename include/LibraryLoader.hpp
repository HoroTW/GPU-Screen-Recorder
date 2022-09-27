#pragma once

#include <dlfcn.h>
#include <stdio.h>

typedef struct {
    void **func;
    const char *name;
} dlsym_assign;

static void* dlsym_print_fail(void *handle, const char *name, bool required) {
    dlerror();
    void *sym = dlsym(handle, name);
    char *err_str = dlerror();

    if(!sym)
        fprintf(stderr, "%s: dlsym(handle, \"%s\") failed, error: %s\n", required ? "error" : "warning", name, err_str ? err_str : "(null)");

    return sym;
}

/* |dlsyms| should be null terminated */
static bool dlsym_load_list(void *handle, const dlsym_assign *dlsyms) {
    bool success = true;
    for(int i = 0; dlsyms[i].func; ++i) {
        *dlsyms[i].func = dlsym_print_fail(handle, dlsyms[i].name, true);
        if(!*dlsyms[i].func)
            success = false;
    }
    return success;
}

/* |dlsyms| should be null terminated */
static void dlsym_load_list_optional(void *handle, const dlsym_assign *dlsyms) {
    for(int i = 0; dlsyms[i].func; ++i) {
        *dlsyms[i].func = dlsym_print_fail(handle, dlsyms[i].name, false);
    }
}