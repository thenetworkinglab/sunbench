/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * keys.h -- minimal raw-mode keyboard for the interactive menu.
 *
 * termios cbreak mode (no canonical line editing, no echo), with the
 * original settings saved and restored -- including on SIGINT -- so we
 * never leave the console wedged.  Arrow keys (ESC [ A/B/C/D) are decoded
 * to logical codes.  If termios isn't available the menu is simply
 * disabled (sb_keys_available() returns 0).
 */
#ifndef SUNBENCH_KEYS_H
#define SUNBENCH_KEYS_H
#include "port.h"

#define SBK_NONE  0
#define SBK_UP    1
#define SBK_DOWN  2
#define SBK_LEFT  3
#define SBK_RIGHT 4
#define SBK_ENTER 5
#define SBK_QUIT  6
#define SBK_OTHER 7

int  sb_keys_available P_((void));   /* 1 if raw-mode keys are supported */
int  sb_keys_raw_on P_((void));      /* enter cbreak; 0 ok, -1 fail */
void sb_keys_raw_off P_((void));     /* restore original tty settings */
int  sb_key_get P_((void));          /* blocking read -> SBK_* */

#endif
