/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* The host tests' check macro: a failing check prints its place and stops the test. */
#ifndef DBY_TORCH_TEST_CHECK_H
#define DBY_TORCH_TEST_CHECK_H
#include <stdio.h>
#include <stdlib.h>

static unsigned checks;
#define CHECK(x) do {if (!(x)) {fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #x); exit(1);} ++checks;} while (0)

/* The whole logged write sequence from `from` on, as "47=00 49=00 43=17 ...": the register the
 * arbiter was told to write and the byte it was told to write, in the order they were sent. A
 * comparison that fails prints both, which is the answer to "in what order does this port touch the
 * flash block" without any hardware attached. */
#define SEQ(from, want) do {                                                            \
        char seq_[2048];                                                                \
        m_seq(&m, (from), seq_, sizeof seq_);                                           \
        if (strcmp(seq_, want) != 0) {                                                  \
            fprintf(stderr, "%s:%d: write sequence\n  got:  %s\n  want: %s\n",         \
                    __FILE__, __LINE__, seq_, want);                                     \
            exit(1);                                                                    \
        }                                                                               \
        ++checks;                                                                       \
    } while (0)

#endif
