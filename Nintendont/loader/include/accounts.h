/* SlippWii: the Slippi accounts saved on the card, and which one is signed in (accounts.c). */
#ifndef ACCOUNTS_H
#define ACCOUNTS_H

#include <stddef.h>
#include <gctypes.h>

/* A Slippi account file (Slippi Launcher's user.json): 1 with the fields filled in (`name` "?" when
 * it has none), 0 when there is no such file, -1 when it is not an account. Any pointer may be NULL;
 * name holds 24 bytes, code 16, uid 40. */
int Accounts_ReadFile(const char *path, char *name, char *code, char *uid);

/* Reads who is signed in, for the header (main.c, once the drives are mounted). */
void Accounts_Load(void);

/* The header's line ("Account : ..."), and its colour; "" before Accounts_Load. */
const char *Accounts_HeaderLine(u32 *color);

/* The account screen (Slippi Settings > Slippi Account): sign in, sign out, forget. */
void Accounts_Menu(void);

/* At Melee's start with nobody signed in (slippwii.c): when accounts are saved, asks who plays.
 * 1: one is signed in, 0: none is saved, -1: the player chose none (B). */
int Accounts_PickAtStart(void);

#endif
