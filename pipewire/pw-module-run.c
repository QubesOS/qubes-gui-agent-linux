/* SPDX-FileCopyrightText: Copyright © 2021-2023 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2021-2023 Demi Marie Obenour */
/* SPDX-License-Identifier: MIT */
/* C11 headers */
#include <stddef.h>
#include <stdlib.h>

/* glibc headers */
#include <err.h>

#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>

int main(int argc, char **argv)
{
    pw_init(&argc, &argv);
    if (argc < 3 || (argc & 1) == 0)
        errx(1, "Usage: module-loader MODULE ARGS [MODULE ARGS...]\nTotal number of arguments must be even.");
    struct pw_main_loop *loop = pw_main_loop_new(NULL);
    if (!loop)
        err(1, "pw_main_loop_new");
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);
    if (!context)
        err(1, "pw_context_new");
    struct pw_impl_module **modules = calloc(argc >> 1, sizeof(*modules));
    if (!modules)
        err(1, "calloc(%d, %zu)", argc, sizeof(*modules));
    for (int i = 1; i < argc - 1; i += 2) {
        if ((modules[i / 2] = pw_context_load_module(context, argv[i], argv[i + 1], NULL)) == NULL)
            err(1, "pw_context_load_module");
    }
    int e = pw_main_loop_run(loop);
    if (e < 0) {
        errno = -e;
        err(1, "pw_main_loop_run()");
    }
    pw_context_destroy(context);
    pw_main_loop_destroy(loop);

    return 0;
}
