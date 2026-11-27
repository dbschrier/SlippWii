/* tools/test_loader_accounts.py: loader/source/accounts.c on a PC, with the stand-ins of
 * loader_stubs.c (built with SW_NO_MAIN). The drives are $SW_SD and $SW_USB, the game's drive
 * $SW_GAME_DEV, the buttons $SW_ANSWERS.
 *
 *   accounts_host header   prints the header line
 *   accounts_host menu     the account screen, then the header
 *   accounts_host start    the question at Melee's start, its answer, then the header */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loader_stubs.h"
#include "accounts.h"

extern FATFS *devices[2];
static FATFS present;

static void print_header(void)
{
	u32 color;
	const char *line = Accounts_HeaderLine(&color);
	printf("HEADER%s %s\n", color == MAROON ? "!" : color == GREEN ? "+" : " ", line);
}

int main(int argc, char **argv)
{
	const char *what = argc > 1 ? argv[1] : "header";
	if (getenv("SW_SD"))
		devices[DEV_SD] = &present;
	if (getenv("SW_USB"))
		devices[DEV_USB] = &present;
	Accounts_Load();
	if (!strcmp(what, "menu"))
		Accounts_Menu();
	else if (!strcmp(what, "start"))
		printf("PICKED %d\n", Accounts_PickAtStart());
	print_header();
	return 0;
}
