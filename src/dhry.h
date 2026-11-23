/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * dhry.h -- Dhrystone 2.1 benchmark, by Reinhold P. Weicker (public domain,
 * May 1988).  Adapted for sunbench: the interactive main() and self-timing
 * are removed; the measured loop is dhry_loop(), with one-time setup in
 * dhry_init().  The harness times it and reports Dhrystones/second.
 *
 * IMPORTANT: dhry_1.c and dhry_2.c MUST be separate translation units with no
 * cross-file / link-time optimisation, or the compiler folds the benchmark
 * away.  This is inherent to Dhrystone's validity, not a sunbench quirk.
 */
#ifndef SUNBENCH_DHRY_H
#define SUNBENCH_DHRY_H
#include "port.h"

typedef enum { Ident_1, Ident_2, Ident_3, Ident_4, Ident_5 } Enumeration;
typedef int  One_Thirty;
typedef int  One_Fifty;
typedef char Capital_Letter;
typedef int  Boolean;
typedef char Str_30 [31];
typedef int  Arr_1_Dim [50];
typedef int  Arr_2_Dim [50][50];

typedef struct record {
    struct record *Ptr_Comp;
    Enumeration    Discr;
    union {
        struct { Enumeration Enum_Comp; int Int_Comp; char Str_Comp[31]; } var_1;
        struct { Enumeration E_Comp_2; char Str_2_Comp[31]; } var_2;
        struct { char Ch_1_Comp; char Ch_2_Comp; } var_3;
    } variant;
} Rec_Type, *Rec_Pointer;

#define Null 0
#define DTRUE  1
#define DFALSE 0

/* Globals shared between dhry_1.c and dhry_2.c. */
extern Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
extern int         Int_Glob;
extern Boolean     Bool_Glob;
extern char        Ch_1_Glob, Ch_2_Glob;
extern int         Arr_1_Glob[50];
extern int         Arr_2_Glob[50][50];

/* dhry_2.c (kept in a separate object on purpose). */
void        Proc_6 P_((Enumeration Enum_Val_Par, Enumeration *Enum_Ref_Par));
void        Proc_7 P_((One_Fifty Int_1_Par_Val, One_Fifty Int_2_Par_Val,
                       One_Fifty *Int_Par_Ref));
void        Proc_8 P_((Arr_1_Dim Arr_1_Par_Ref, Arr_2_Dim Arr_2_Par_Ref,
                       int Int_1_Par_Val, int Int_2_Par_Val));
/* int, not Capital_Letter: the definition is K&R, so its char parameters
 * arrive default-promoted to int, and a prototype must say so.  gcc lets
 * a char prototype slide; Minix's ACK rejects it (redeclaration).  Open
 * Watcom (the DOS build) is the exception the other way round: it takes
 * the K&R definition's char literally and rejects the int prototype. */
#ifdef __WATCOMC__
Enumeration Func_1 P_((Capital_Letter Ch_1_Par_Val, Capital_Letter Ch_2_Par_Val));
#else
Enumeration Func_1 P_((int Ch_1_Par_Val, int Ch_2_Par_Val));
#endif
Boolean     Func_2 P_((Str_30 Str_1_Par_Ref, Str_30 Str_2_Par_Ref));
Boolean     Func_3 P_((Enumeration Enum_Par_Val));

/* Harness entry points (dhry_1.c). */
void dhry_init P_((void));
void dhry_loop P_((unsigned long runs));

#endif
