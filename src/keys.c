/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/* keys.c -- see keys.h. */
#include "port.h"
#include "keys.h"
#include "tui.h"        /* sb_tui_alt_leave on Ctrl-C */

#ifdef HAVE_TERMIOS

#include <termios.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>

static struct termios g_saved;
static int g_raw = 0;

int sb_keys_available()
{
    return 1;
}

void sb_keys_raw_off()
{
    if (g_raw) {
        (void)tcsetattr(0, TCSANOW, &g_saved);
        g_raw = 0;
    }
}

/* Restore the terminal if the user hits Ctrl-C mid-menu, then die. */
static void on_intr(sig)
    int sig;
{
    sb_keys_raw_off();
    sb_tui_alt_leave();
    (void)signal(sig, SIG_DFL);
    _exit(1);
}

int sb_keys_raw_on()
{
    struct termios t;

    if (!isatty(0))
        return -1;
    if (tcgetattr(0, &g_saved) != 0)
        return -1;
    t = g_saved;
    t.c_lflag &= ~(ICANON | ECHO);
    t.c_cc[VMIN]  = 1;      /* one byte at a time, blocking */
    t.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSANOW, &t) != 0)
        return -1;
    g_raw = 1;
    (void)signal(SIGINT, on_intr);
    return 0;
}

int sb_key_get()
{
    unsigned char c, c2, c3;
    struct termios raw, esc;
    int n;

    if (read(0, &c, 1) != 1)
        return SBK_QUIT;
    if (c == '\r' || c == '\n') return SBK_ENTER;
    if (c == 'q' || c == 'Q')   return SBK_QUIT;
    if (c == 'k')               return SBK_UP;    /* vi-style bonus */
    if (c == 'j')               return SBK_DOWN;

    if (c == 27) {   /* ESC: could be a lone key or an arrow sequence */
        /* Briefly allow a non-blocking follow-up read so lone ESC = quit. */
        if (tcgetattr(0, &raw) != 0)
            return SBK_QUIT;
        esc = raw;
        esc.c_cc[VMIN]  = 0;
        esc.c_cc[VTIME] = 1;   /* 0.1s */
        (void)tcsetattr(0, TCSANOW, &esc);
        n = read(0, &c2, 1);
        if (n == 1 && c2 == '[') {
            (void)read(0, &c3, 1);
            (void)tcsetattr(0, TCSANOW, &raw);
            switch (c3) {
            case 'A': return SBK_UP;
            case 'B': return SBK_DOWN;
            case 'C': return SBK_RIGHT;
            case 'D': return SBK_LEFT;
            }
            return SBK_OTHER;
        }
        (void)tcsetattr(0, TCSANOW, &raw);
        return SBK_QUIT;   /* lone ESC */
    }
    return SBK_OTHER;
}

#else  /* no termios -> menu disabled */

int  sb_keys_available() { return 0; }
int  sb_keys_raw_on()    { return -1; }
void sb_keys_raw_off()   { }
int  sb_key_get()        { return SBK_QUIT; }

#endif
