/* Copyright (C) 2026 thenetworkinglab
 * SPDX-License-Identifier: GPL-3.0-or-later  (see LICENSE) */
/*
 * dhry_1.c -- Dhrystone 2.1, first module (Weicker, public domain).
 * The measured loop is dhry_loop(); dhry_init() sets up the global state
 * once.  See dhry.h for the separate-compilation requirement.
 */
#include "port.h"
#include "dhry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Rec_Pointer Ptr_Glob, Next_Ptr_Glob;
int         Int_Glob;
Boolean     Bool_Glob;
char        Ch_1_Glob, Ch_2_Glob;
int         Arr_1_Glob[50];
int         Arr_2_Glob[50][50];

#define structassign(d, s) memcpy(&(d), &(s), sizeof(Rec_Type))

static void Proc_1 P_((Rec_Pointer Ptr_Val_Par));
static void Proc_2 P_((One_Fifty *Int_Par_Ref));
static void Proc_3 P_((Rec_Pointer *Ptr_Ref_Par));
static void Proc_4 P_((void));
static void Proc_5 P_((void));

static void Proc_1(Ptr_Val_Par)
    Rec_Pointer Ptr_Val_Par;
{
    Rec_Pointer Next_Record = Ptr_Val_Par->Ptr_Comp;
    structassign(*Ptr_Val_Par->Ptr_Comp, *Ptr_Glob);
    Ptr_Val_Par->variant.var_1.Int_Comp = 5;
    Next_Record->variant.var_1.Int_Comp = Ptr_Val_Par->variant.var_1.Int_Comp;
    Next_Record->Ptr_Comp = Ptr_Val_Par->Ptr_Comp;
    Proc_3(&Next_Record->Ptr_Comp);
    if (Next_Record->Discr == Ident_1) {
        Next_Record->variant.var_1.Int_Comp = 6;
        Proc_6(Ptr_Val_Par->variant.var_1.Enum_Comp,
               &Next_Record->variant.var_1.Enum_Comp);
        Next_Record->Ptr_Comp = Ptr_Glob->Ptr_Comp;
        Proc_7(Next_Record->variant.var_1.Int_Comp, 10,
               &Next_Record->variant.var_1.Int_Comp);
    } else
        structassign(*Ptr_Val_Par, *Ptr_Val_Par->Ptr_Comp);
}

static void Proc_2(Int_Par_Ref)
    One_Fifty *Int_Par_Ref;
{
    One_Fifty   Int_Loc;
    Enumeration Enum_Loc;
    Enum_Loc = Ident_2;
    Int_Loc = *Int_Par_Ref + 10;
    do
        if (Ch_1_Glob == 'A') {
            Int_Loc -= 1;
            *Int_Par_Ref = Int_Loc - Int_Glob;
            Enum_Loc = Ident_1;
        }
    while (Enum_Loc != Ident_1);
}

static void Proc_3(Ptr_Ref_Par)
    Rec_Pointer *Ptr_Ref_Par;
{
    if (Ptr_Glob != Null)
        *Ptr_Ref_Par = Ptr_Glob->Ptr_Comp;
    Proc_7(10, Int_Glob, &Ptr_Glob->variant.var_1.Int_Comp);
}

static void Proc_4()
{
    Boolean Bool_Loc;
    Bool_Loc = Ch_1_Glob == 'A';
    Bool_Glob = Bool_Loc | Bool_Glob;
    Ch_2_Glob = 'B';
}

static void Proc_5()
{
    Ch_1_Glob = 'A';
    Bool_Glob = DFALSE;
}

void dhry_init()
{
    Next_Ptr_Glob = (Rec_Pointer) malloc(sizeof(Rec_Type));
    Ptr_Glob = (Rec_Pointer) malloc(sizeof(Rec_Type));
    Ptr_Glob->Ptr_Comp = Next_Ptr_Glob;
    Ptr_Glob->Discr = Ident_1;
    Ptr_Glob->variant.var_1.Enum_Comp = Ident_3;
    Ptr_Glob->variant.var_1.Int_Comp = 40;
    strcpy(Ptr_Glob->variant.var_1.Str_Comp,
           "DHRYSTONE PROGRAM, SOME STRING");
    Arr_2_Glob[8][7] = 10;
}

void dhry_loop(runs)
    unsigned long runs;
{
    One_Fifty     Int_1_Loc, Int_2_Loc, Int_3_Loc;
    char          Ch_Index;
    Enumeration   Enum_Loc;
    Str_30        Str_1_Loc, Str_2_Loc;
    unsigned long Run_Index;

    Int_3_Loc = 0;
    strcpy(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING");
    for (Run_Index = 1; Run_Index <= runs; ++Run_Index) {
        Proc_5();
        Proc_4();
        Int_1_Loc = 2;
        Int_2_Loc = 3;
        strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
        Enum_Loc = Ident_2;
        Bool_Glob = !Func_2(Str_1_Loc, Str_2_Loc);
        while (Int_1_Loc < Int_2_Loc) {
            Int_3_Loc = 5 * Int_1_Loc - Int_2_Loc;
            Proc_7(Int_1_Loc, Int_2_Loc, &Int_3_Loc);
            Int_1_Loc += 1;
        }
        Proc_8(Arr_1_Glob, Arr_2_Glob, Int_1_Loc, Int_3_Loc);
        Proc_1(Ptr_Glob);
        for (Ch_Index = 'A'; Ch_Index <= Ch_2_Glob; ++Ch_Index)
            if (Enum_Loc == Func_1(Ch_Index, 'C')) {
                Proc_6(Ident_1, &Enum_Loc);
                strcpy(Str_2_Loc, "DHRYSTONE PROGRAM, 3'RD STRING");
                Int_2_Loc = (One_Fifty) Run_Index;
                Int_Glob = (int) Run_Index;
            }
        Int_2_Loc = Int_2_Loc * Int_1_Loc;
        Int_1_Loc = Int_2_Loc / Int_3_Loc;
        Int_2_Loc = 7 * (Int_2_Loc - Int_3_Loc) - Int_1_Loc;
        Proc_2(&Int_1_Loc);
    }
}
