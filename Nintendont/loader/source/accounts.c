/* SlippWii: the Slippi accounts on the card, and which one is signed in (accounts.h).
 *
 *   /slippwii/user.json                 the account signed in: the one online play uses
 *   /slippwii/accounts/NAME.json        the accounts saved on this card (Slippi Launcher's user.json)
 *   /slippwii/accounts/NAME.direct.bin  each one's Direct codes, and its Teams codes, as the kernel
 *   /slippwii/accounts/NAME.teams.bin   keeps them (direct-codes.bin, teams-codes.bin: so_exi.c)
 *
 * All on SlippWii's own drive (the SD card; the USB drive when there is no card). The kernel reads
 * the account and the code histories from the game's drive: slippwii.c copies user.json there when
 * a game starts, and signing in or out here puts the histories on every drive, or takes them off.
 *
 * Signing in, out or forgetting never loses a login: the account signed in is saved in accounts/
 * (with the codes its games left) before anything replaces it, and the one file written for
 * signing in is written beside user.json first and then put in its place.
 */
#include <gccore.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "global.h"
#include "font.h"
#include "FPad.h"
#include "menu.h"
#include "ff_utf8.h"
#include "diskio.h"
#include "accounts.h"

extern FATFS *devices[2];
extern u32 Shutdown;	/* menu.c */

#define MAX_ACCOUNTS 32
#define SHOWN 10		/* accounts on the screen at once */
#define FILE_MAX 0x4000		/* a user.json is a few hundred bytes */
#define HISTORY_MAX (5 + 8 * 17)	/* direct-codes.bin: "SLDC", a count, 8 codes (so_exi.c) */

static const char *const dev_names[2] = { "sd", "usb" };
static const char *const histories[2] = { "direct-codes.bin", "teams-codes.bin" };
static const char *const history_kinds[2] = { "direct", "teams" };

typedef struct {
	char file[64];		/* in accounts/; "" for a user.json not saved there yet */
	char name[24], code[16], uid[40];
	int damaged;		/* not an account: shown, but can't be signed in */
} Account;

static Account list[MAX_ACCOUNTS];
static int count;		/* in list */
static int active = -1;		/* who is signed in, in list; -1: nobody */
static int active_state;	/* user.json: 1 an account, 0 none, -1 damaged */
static int loaded;
static char header[64];
static u32 header_color;

/* ---- files ----------------------------------------------------------------- */

static const char *home_dev(void)
{
	return devices[DEV_SD] ? "sd" : "usb";
}

static void path_of(char *out, size_t size, const char *dev, const char *rel)
{
	snprintf(out, size, "%s:/slippwii/%s", dev, rel);
}

/* accounts/ of the home drive: the file's name, and the one beside it of the same stem. */
static void saved_path(char *out, size_t size, const char *file, const char *kind)
{
	char stem[64];
	size_t n = strlen(file);
	if (!kind) {
		snprintf(out, size, "%s:/slippwii/accounts/%s", home_dev(), file);
		return;
	}
	snprintf(stem, sizeof(stem), "%.*s", (int)(n > 5 ? n - 5 : n), file);	/* (less ".json") */
	snprintf(out, size, "%s:/slippwii/accounts/%s.%s.bin", home_dev(), stem, kind);
}

static void wide(const char *s, WCHAR *out, size_t count)
{
	size_t i;
	for (i = 0; i + 1 < count && s[i]; i++)
		out[i] = (unsigned char)s[i];
	out[i] = 0;
}

static void remove_file(const char *path)
{
	WCHAR w[128];
	wide(path, w, 128);
	f_unlink(w);
}

/* The file (malloc'd, a 0 after it), or NULL. */
static u8 *load(const char *path, u32 *size, u32 max)
{
	FIL f;
	UINT got = 0;
	u8 *buf;
	if (f_open_char(&f, path, FA_READ | FA_OPEN_EXISTING) != FR_OK)
		return NULL;
	*size = (u32)f_size(&f);
	buf = *size <= max ? malloc(*size + 1) : NULL;
	if (buf && (f_read(&f, buf, *size, &got) != FR_OK || got != *size)) {
		free(buf);
		buf = NULL;
	}
	f_close(&f);
	if (buf)
		buf[*size] = '\0';
	return buf;
}

static void unload(u8 *buf, u32 size)
{
	if (buf)
		memset(buf, 0, size);	/* (a login: not left in memory) */
	free(buf);
}

static int write_new(const char *path, const void *buf, u32 size)
{
	FIL f;
	UINT wrote = 0;
	FRESULT res;
	if (f_open_char(&f, path, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK)
		return -1;
	res = f_write(&f, buf, size, &wrote);
	if (f_close(&f) != FR_OK || res != FR_OK || wrote != size)
		return -1;
	return 0;
}

/* Written beside the file as NAME.new, then put in its place: a power cut leaves the old file or the
 * new one (or, between the two steps, NAME.new alone: Accounts_Load takes it for user.json). */
static int save(const char *path, const void *buf, u32 size)
{
	char tmp[128];
	WCHAR wa[128], wb[128];
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (write_new(tmp, buf, size) < 0) {
		remove_file(tmp);
		return -1;
	}
	remove_file(path);
	wide(tmp, wa, 128);
	wide(path, wb, 128);
	return f_rename(wa, wb) == FR_OK ? 0 : -1;
}

/* 0, 1 when there is no `from`, -1 when it could not be written. */
static int copy_file(const char *from, const char *to, u32 max)
{
	u32 n;
	u8 *buf = load(from, &n, max);
	int res;
	if (!buf)
		return 1;
	res = save(to, buf, n);
	unload(buf, n);
	return res;
}

/* ---- an account file --------------------------------------------------------- */

/* A JSON string field's value, printable ASCII only (others shown as '?'). */
static int json_string(const char *json, const char *key, char *out, size_t size)
{
	char pattern[40];
	const char *p;
	size_t n = 0;
	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (!p)
		return 0;
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
		p++;
	if (*p++ != '"')
		return 0;
	while (*p && *p != '"' && n + 1 < size) {
		unsigned char c = (unsigned char)*p++;
		if (c == '\\' && *p)
			c = (unsigned char)*p++;
		if (c >= 0x80) {
			while ((*p & 0xC0) == 0x80)
				p++;	/* the rest of a UTF-8 character */
			c = '?';
		} else if (c < 0x20) {
			c = '?';
		}
		out[n++] = (char)c;
	}
	out[n] = '\0';
	return n > 0;
}

int Accounts_ReadFile(const char *path, char *name, char *code, char *uid)
{
	char key[8], c[16], u[40], nm[24];
	u32 n;
	char *json = (char *)load(path, &n, FILE_MAX);
	int ok;
	if (!json)
		return 0;
	ok = json_string(json, "uid", u, sizeof(u)) && json_string(json, "playKey", key, sizeof(key)) &&
	     json_string(json, "connectCode", c, sizeof(c));
	if (ok && !json_string(json, "displayName", nm, sizeof(nm)))
		strcpy(nm, "?");
	unload((u8 *)json, n);
	memset(key, 0, sizeof(key));
	if (!ok)
		return -1;
	if (name)
		strcpy(name, nm);
	if (code)
		strcpy(code, c);
	if (uid)
		strcpy(uid, u);
	return 1;
}

/* ---- what is on the card --------------------------------------------------- */

static int ends_with_json(const char *s)
{
	size_t n = strlen(s);
	return n > 5 && strcasecmp(s + n - 5, ".json") == 0;
}

/* A file's name from the directory, if it is plain ASCII (the ones this writes are). */
static int ascii_name(const FILINFO *info, char *out, size_t size)
{
	size_t i;
	for (i = 0; i + 1 < size && info->fname[i]; i++) {
		if (info->fname[i] >= 0x80)
			return 0;
		out[i] = (char)info->fname[i];
	}
	out[i] = '\0';
	return !info->fname[i];
}

static int find_uid(const char *uid)
{
	int i;
	for (i = 0; i < count; i++)
		if (!list[i].damaged && strcmp(list[i].uid, uid) == 0)
			return i;
	return -1;
}

static int saved_count(void)
{
	int i, n = 0;
	for (i = 0; i < count; i++)
		n += !list[i].damaged && list[i].file[0];
	return n;
}

static void make_header(void)
{
	char line[64];
	const Account *a = active >= 0 ? &list[active] : NULL;
	header_color = a ? GREEN : MAROON;
	if (a) {
		/* (28 characters: the settings page's "1 / 2" is beside it) */
		snprintf(line, sizeof(line), "Account : %s (%s)", a->name, a->code);
		if (strlen(line) > 28)
			snprintf(line, sizeof(line), "Account : %s", a->code);
	} else if (active_state < 0) {
		snprintf(line, sizeof(line), "Account : user.json damaged");
	} else if (saved_count()) {
		snprintf(line, sizeof(line), "Account : none (%d saved)", saved_count());
	} else {
		snprintf(line, sizeof(line), "Account : none (online off)");
	}
	snprintf(header, sizeof(header), "%s", line);
	loaded = 1;
}

/* Reads user.json and accounts/ into the list, by name. */
static void scan(void)
{
	char path[128], name[64], uid[40];
	static FILINFO info;
	Account act, tmp;
	DIR dir;
	int i, j;

	count = 0;
	active = -1;
	memset(&act, 0, sizeof(act));
	path_of(path, sizeof(path), home_dev(), "user.json");
	active_state = Accounts_ReadFile(path, act.name, act.code, act.uid);

	snprintf(path, sizeof(path), "%s:/slippwii/accounts", home_dev());
	if (f_opendir_char(&dir, path) == FR_OK) {
		while (count < MAX_ACCOUNTS && f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
			Account *a = &list[count];
			if ((info.fattrib & AM_DIR) || !ascii_name(&info, name, sizeof(name)) || !ends_with_json(name))
				continue;
			memset(a, 0, sizeof(*a));
			strcpy(a->file, name);
			saved_path(path, sizeof(path), name, NULL);
			a->damaged = Accounts_ReadFile(path, a->name, a->code, uid) != 1;
			if (a->damaged) {
				snprintf(a->name, sizeof(a->name), "%s", name);
			} else {
				if (find_uid(uid) >= 0)
					continue;	/* (the same account twice: shown once) */
				strcpy(a->uid, uid);
			}
			count++;
		}
		f_closedir(&dir);
	}
	if (active_state == 1 && find_uid(act.uid) < 0 && count < MAX_ACCOUNTS)
		list[count++] = act;	/* signed in, not saved in accounts/ yet */

	for (i = 1; i < count; i++)	/* by name */
		for (j = i; j > 0 && strcasecmp(list[j - 1].name, list[j].name) > 0; j--) {
			tmp = list[j];
			list[j] = list[j - 1];
			list[j - 1] = tmp;
		}
	if (active_state == 1)
		active = find_uid(act.uid);
	make_header();
}

/* A name in accounts/ for an account: its connect code ("ABCD#123" -> "ABCD-123.json"). */
static void new_file_name(const Account *a, char *out, size_t size)
{
	char stem[24], path[128];
	size_t i;
	int n;
	for (i = 0; i + 1 < sizeof(stem) && a->code[i]; i++) {
		char c = a->code[i];
		stem[i] = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ? c : '-';
	}
	stem[i] = '\0';
	for (n = 1; n < 100; n++) {
		if (n == 1)
			snprintf(out, size, "%s.json", stem);
		else
			snprintf(out, size, "%s-%d.json", stem, n);
		saved_path(path, sizeof(path), out, NULL);
		if (Accounts_ReadFile(path, NULL, NULL, NULL) == 0)
			return;	/* (no such file) */
	}
}

/* Which drive the kernel plays from: where the games' code histories are. */
static const char *game_dev(void)
{
	const char *dev = GetRootDevice();
	if (!dev)
		return home_dev();
	return devices[strcmp(dev, "sd") == 0 ? DEV_SD : DEV_USB] ? dev : home_dev();
}

/* The account signed in, saved in accounts/ (first, if it was only user.json) with the codes its
 * games left. 0, or -1: then nothing is to change. */
static int keep_active(void)
{
	char from[128], to[128];
	Account *a;
	int k;
	if (active < 0)
		return 0;
	a = &list[active];
	if (!a->file[0]) {
		char file[64];
		snprintf(from, sizeof(from), "%s:/slippwii/accounts", home_dev());
		f_mkdir_char(from);
		new_file_name(a, file, sizeof(file));
		path_of(from, sizeof(from), home_dev(), "user.json");
		saved_path(to, sizeof(to), file, NULL);
		if (copy_file(from, to, FILE_MAX) != 0)
			return -1;
		strcpy(a->file, file);
	}
	for (k = 0; k < 2; k++) {
		path_of(from, sizeof(from), game_dev(), histories[k]);
		saved_path(to, sizeof(to), a->file, history_kinds[k]);
		if (copy_file(from, to, HISTORY_MAX) == 1 && strcmp(game_dev(), home_dev()) != 0) {
			path_of(from, sizeof(from), home_dev(), histories[k]);
			copy_file(from, to, HISTORY_MAX);
		}
	}
	return 0;
}

/* The code histories on every drive: the account's (none if it has none, or `a` is NULL). */
static void put_histories(const Account *a)
{
	char from[128], to[128];
	int d, k;
	for (d = 0; d < 2; d++) {
		if (!devices[d])
			continue;
		for (k = 0; k < 2; k++) {
			path_of(to, sizeof(to), dev_names[d], histories[k]);
			if (a && a->file[0]) {
				saved_path(from, sizeof(from), a->file, history_kinds[k]);
				if (copy_file(from, to, HISTORY_MAX) == 0)
					continue;
			}
			remove_file(to);
		}
	}
}

/* user.json off every drive (when it is `uid`'s; any, for NULL). */
static void remove_signed_in(const char *uid)
{
	char path[128], u[40];
	int d;
	for (d = 0; d < 2; d++) {
		if (!devices[d])
			continue;
		path_of(path, sizeof(path), dev_names[d], "user.json");
		if (!uid || (Accounts_ReadFile(path, NULL, NULL, u) == 1 && strcmp(u, uid) == 0))
			remove_file(path);
	}
}

/* Slippi Launcher's backups of earlier accounts (user.json.old-DATE, made by the copy script before
 * it kept accounts here) to accounts/, when they are accounts not saved there. */
static void adopt_old_files(void)
{
	char path[128], from[128], to[128], name[64], uid[40], file[64];
	static FILINFO info;
	Account a;
	DIR dir;
	snprintf(path, sizeof(path), "%s:/slippwii", home_dev());
	if (f_opendir_char(&dir, path) != FR_OK)
		return;
	while (f_readdir(&dir, &info) == FR_OK && info.fname[0]) {
		if ((info.fattrib & AM_DIR) || !ascii_name(&info, name, sizeof(name)) || strncmp(name, "user.json.old-", 14) != 0)
			continue;
		path_of(from, sizeof(from), home_dev(), name);
		memset(&a, 0, sizeof(a));
		if (Accounts_ReadFile(from, a.name, a.code, uid) != 1 || find_uid(uid) >= 0)
			continue;
		snprintf(to, sizeof(to), "%s:/slippwii/accounts", home_dev());
		f_mkdir_char(to);
		new_file_name(&a, file, sizeof(file));
		saved_path(to, sizeof(to), file, NULL);
		if (copy_file(from, to, FILE_MAX) == 0) {
			remove_file(from);
			scan();	/* (so a second backup of the same account is seen as saved) */
		}
	}
	f_closedir(&dir);
}

void Accounts_Load(void)
{
	char path[128], tmp[128];
	WCHAR wa[128], wb[128];
	path_of(path, sizeof(path), home_dev(), "user.json");
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	if (Accounts_ReadFile(path, NULL, NULL, NULL) == 0 && Accounts_ReadFile(tmp, NULL, NULL, NULL) == 1) {
		wide(tmp, wa, 128);	/* a sign-in that a power cut stopped half way */
		wide(path, wb, 128);
		f_rename(wa, wb);
	}
	scan();
}

const char *Accounts_HeaderLine(u32 *color)
{
	*color = header_color;
	return loaded ? header : "";
}

/* ---- signing in and out ------------------------------------------------------------ */

static char message[64];
static u32 message_color;

static void tell(u32 color, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(message, sizeof(message), fmt, ap);
	va_end(ap);
	message_color = color;
}

static void sign_in(int i)
{
	char from[128], to[128];
	Account a = list[i];
	if (a.damaged) {
		tell(MAROON, "%s is not a Slippi account.", a.file);
		return;
	}
	if (i == active) {
		tell(BLACK, "%s (%s) is signed in.", a.name, a.code);
		return;
	}
	if (keep_active() < 0) {
		tell(MAROON, "Could not save the account signed in.");
		return;
	}
	saved_path(from, sizeof(from), a.file, NULL);
	path_of(to, sizeof(to), home_dev(), "user.json");
	if (copy_file(from, to, FILE_MAX) != 0) {
		tell(MAROON, "Could not write to the %s.", devices[DEV_SD] ? "SD card" : "USB drive");
		scan();
		return;
	}
	if (strcmp(home_dev(), "sd") == 0 && devices[DEV_USB]) {
		path_of(to, sizeof(to), "usb", "user.json");
		remove_file(to);	/* (slippwii.c puts the new one there when a game starts from it) */
	}
	put_histories(&a);
	scan();
	tell(GREEN, "Signed in as %s (%s).", a.name, a.code);
}

static void sign_out(void)
{
	Account a;
	if (active < 0) {
		tell(BLACK, "Nobody is signed in.");
		return;
	}
	a = list[active];
	if (keep_active() < 0) {
		tell(MAROON, "Could not save %s first: still signed in.", a.name);
		return;
	}
	remove_signed_in(NULL);
	put_histories(NULL);
	scan();
	tell(BLACK, "Signed out. %s is still saved here.", a.name);
}

static void forget(int i)
{
	char path[128];
	Account a = list[i];
	int k;
	if (!a.damaged) {
		if (i == active)
			put_histories(NULL);
		remove_signed_in(a.uid);
	}
	if (a.file[0]) {
		saved_path(path, sizeof(path), a.file, NULL);
		remove_file(path);
		for (k = 0; k < 2; k++) {
			saved_path(path, sizeof(path), a.file, history_kinds[k]);
			remove_file(path);
		}
	}
	scan();
	tell(BLACK, "%s was removed from this Wii.", a.damaged ? a.file : a.name);
}

/* ---- the screen ---------------------------------------------------------------------- */

static void draw_list(const char *title, const char *line, int sel, int top)
{
	int i;
	PrintFormat(DEFAULT_SIZE, DARK_BLUE, MENU_POS_X, MENU_POS_Y + 20 * 3 + 10, "%s", title);
	if (line)
		PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 5, "%s", line);
	for (i = top; i < count && i < top + SHOWN; i++) {
		const Account *a = &list[i];
		u32 color = a->damaged ? GRAY : i == active ? GREEN : BLACK;
		PrintFormat(DEFAULT_SIZE, color, MENU_POS_X, MENU_POS_Y + 20 * (6 + i - top), "%s %-16.16s %-9s %s",
			i == sel ? ARROW_RIGHT : " ", a->name, a->damaged ? "" : a->code,
			a->damaged ? "damaged" : i == active ? "signed in" : "");
	}
	if (top > 0)
		PrintFormat(DEFAULT_SIZE, GRAY, MENU_POS_X + 410, MENU_POS_Y + 20 * 6, "(more above)");
	if (top + SHOWN < count)
		PrintFormat(DEFAULT_SIZE, GRAY, MENU_POS_X + 410, MENU_POS_Y + 20 * (5 + SHOWN), "(more below)");
	if (message[0])
		PrintFormat(DEFAULT_SIZE, message_color, MENU_POS_X, MENU_POS_Y + 20 * 17, "%s", message);
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 19, "To add an account: copy its user.json into");
	PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 20, "slippwii/accounts/ on the SD card.");
}

/* A, B or nothing, after the first frames (so the press that opened a screen does nothing there). */
static int frames;
static int pressed_ok(void) { return frames > 10 && FPAD_OK(0); }
static int pressed_back(void) { return frames > 10 && FPAD_Cancel(0); }

static void wait_frame(void)
{
	VIDEO_WaitVSync();
	FPAD_Update();
	if (Shutdown)
		LoaderShutdown();
	frames++;
}

static int confirm_forget(const Account *a)
{
	int redraw = 1;
	frames = 0;
	while (1) {
		wait_frame();
		if (redraw) {
			PrintBuildInfo();
			PrintButtonActions(NULL, "Forget", "Keep", NULL);
			PrintFormat(DEFAULT_SIZE, DARK_BLUE, MENU_POS_X, MENU_POS_Y + 20 * 3 + 10, "Slippi Account");
			if (a->damaged) {
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 5, "Delete %s?", a->file);
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 6, "It is not a Slippi account.");
			} else {
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 5, "Forget %s (%s)?", a->name, a->code);
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 6, "Its login is removed from this Wii's SD card");
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 7, "and USB drive. To sign in with it again,");
				PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X, MENU_POS_Y + 20 * 8, "copy its user.json from a PC.");
			}
			GRRLIB_Render();
			ClearScreen();
			redraw = 0;
		}
		if (pressed_back())
			return 0;
		if (pressed_ok())
			return 1;
	}
}

void Accounts_Menu(void)
{
	int sel, top = 0, redraw = 1;
	scan();
	adopt_old_files();
	sel = active >= 0 ? active : 0;
	message[0] = '\0';
	frames = 0;
	while (1) {
		wait_frame();
		if (redraw) {
			if (sel >= count)
				sel = count > 0 ? count - 1 : 0;
			if (sel < top)
				top = sel;
			if (sel >= top + SHOWN)
				top = sel - SHOWN + 1;
			PrintBuildInfo();
			PrintButtonActions(NULL, "Sign in", "Back", "Sign out");
			PrintFormat(DEFAULT_SIZE, BLACK, MENU_POS_X + 410, MENU_POS_Y + 20 * 4, "Y : Forget");
			draw_list("Slippi Account", count ? NULL : "No accounts saved on this card yet.", sel, top);
			GRRLIB_Render();
			ClearScreen();
			redraw = 0;
		}
		if (frames <= 10)
			continue;
		redraw = 1;
		if (FPAD_Cancel(0))
			break;
		else if (FPAD_OK(0) && count)
			sign_in(sel);
		else if (FPAD_X(0))
			sign_out();
		else if (FPAD_Y(0) && count) {
			if (confirm_forget(&list[sel]))
				forget(sel);
			else
				message[0] = '\0';
			frames = 11;
		} else if (FPAD_Up(0) && sel > 0)
			sel--;
		else if (FPAD_Down(0) && sel + 1 < count)
			sel++;
		else
			redraw = 0;
	}
	ClearScreen();
}

int Accounts_PickAtStart(void)
{
	int sel = 0, top = 0, redraw = 1;
	scan();
	adopt_old_files();
	if (active >= 0)
		return 1;
	if (!saved_count())
		return 0;
	message[0] = '\0';
	frames = 0;
	while (1) {
		wait_frame();
		if (redraw) {
			if (sel < top)
				top = sel;
			if (sel >= top + SHOWN)
				top = sel - SHOWN + 1;
			PrintBuildInfo();
			PrintButtonActions(NULL, "Sign in", "No account", NULL);
			draw_list("Who is playing?", "No account is signed in. Pick one for online play:", sel, top);
			GRRLIB_Render();
			ClearScreen();
			redraw = 0;
		}
		if (frames <= 10)
			continue;
		redraw = 1;
		if (FPAD_Cancel(0)) {
			ClearScreen();
			return -1;
		} else if (FPAD_OK(0)) {
			sign_in(sel);
			if (active >= 0)
				break;
		} else if (FPAD_Up(0) && sel > 0)
			sel--;
		else if (FPAD_Down(0) && sel + 1 < count)
			sel++;
		else
			redraw = 0;
	}
	ClearScreen();
	return 1;
}
