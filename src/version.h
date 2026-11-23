/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * version.h -- single source of truth for the sunbench version.
 *
 * CalVer: YYYY.MM.DD-N  (month and day zero-padded; N counts that day's
 * releases from 1)
 *   2026.11.22-1  = first release of 22 November 2026
 *   2026.11.22-2  = second release the same day
 *   2026.12.05-1  = first release of 5 December 2026
 * Releases before 22 November 2026 used YYYY.MDD.PATCH (2026.719.0 = 19
 * July, first that day), which is ambiguous between January and
 * November/December (2026.111.0) -- hence the change.
 *
 * Keep this in sync with VERSION in the Makefile (used for package names).
 */
#ifndef SUNBENCH_VERSION_H
#define SUNBENCH_VERSION_H

#define SB_VERSION "2026.11.22-4"

#endif
