/* See loader_stubs.h. main() runs SlippWii_Prepare once and prints what
 * it left: the config's networking bit and the account line. (Built with
 * SW_NO_MAIN, the stand-ins only: tests/host/accounts_main.c has its own.) */
#include <dirent.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "loader_stubs.h"
#include "slippwii.h"

static NIN_CFG cfg;
NIN_CFG *ncfg = &cfg;
FATFS *devices[2];
#ifndef SW_NO_MAIN
static FATFS present;
#endif
u32 Shutdown;
static const char *answers;
static int last_y = -1;

static void real_path(char *out, size_t size, const char *path)
{
	const char *root = NULL;
	if (strncmp(path, "sd:", 3) == 0) {
		root = getenv("SW_SD");
		path += 3;
	} else if (strncmp(path, "usb:", 4) == 0) {
		root = getenv("SW_USB");
		path += 4;
	}
	snprintf(out, size, "%s%s", root ? root : "", path);
}

FRESULT f_open_char(FIL *fp, const char *path, BYTE mode)
{
	char p[1024];
	real_path(p, sizeof(p), path);
	fp->fp = fopen(p, !(mode & FA_WRITE) ? "rb" : (mode & FA_CREATE_ALWAYS) ? "wb" : "r+b");
	if (!fp->fp)
		return FR_NO_FILE;
	fseek(fp->fp, 0, SEEK_END);
	fp->size = (u64)ftell(fp->fp);
	fseek(fp->fp, 0, SEEK_SET);
	return FR_OK;
}

FRESULT f_read(FIL *fp, void *buf, UINT n, UINT *got)
{
	*got = (UINT)fread(buf, 1, n, fp->fp);
	return FR_OK;
}

FRESULT f_write(FIL *fp, const void *buf, UINT n, UINT *wrote)
{
	*wrote = (UINT)fwrite(buf, 1, n, fp->fp);
	return *wrote == n ? FR_OK : FR_DISK_ERR;
}

FRESULT f_lseek(FIL *fp, u64 offset)
{
	return fseek(fp->fp, (long)offset, SEEK_SET) == 0 ? FR_OK : FR_DISK_ERR;
}

FRESULT f_close(FIL *fp)
{
	fclose(fp->fp);
	return FR_OK;
}

FRESULT f_mkdir_char(const char *path)
{
	char p[1024];
	real_path(p, sizeof(p), path);
#ifdef _WIN32
	_mkdir(p);
#else
	mkdir(p, 0777);
#endif
	return FR_OK;
}

FRESULT f_opendir_char(DIR *dp, const char *path)
{
	char p[1024];
	real_path(p, sizeof(p), path);
	dp->dir = opendir(p);
	return dp->dir ? FR_OK : FR_NO_FILE;
}

/* (sorted by nothing, as FAT's own order isn't: tests mustn't count on one) */
FRESULT f_readdir(DIR *dp, FILINFO *info)
{
	struct dirent *e;
	size_t i;
	do
		e = readdir(dp->dir);
	while (e && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")));
	memset(info, 0, sizeof(*info));
	if (!e)
		return FR_OK;
	for (i = 0; i + 1 < 256 && e->d_name[i]; i++)
		info->fname[i] = (unsigned char)e->d_name[i];
	info->fattrib = e->d_type == DT_DIR ? AM_DIR : 0;
	return FR_OK;
}

FRESULT f_closedir(DIR *dp)
{
	closedir(dp->dir);
	return FR_OK;
}

static void narrow(const WCHAR *w, char *out, size_t size)
{
	size_t i;
	for (i = 0; i + 1 < size && w[i]; i++)
		out[i] = (char)w[i];
	out[i] = '\0';
}

FRESULT f_unlink(const WCHAR *path)
{
	char a[1024], p[1024];
	narrow(path, a, sizeof(a));
	real_path(p, sizeof(p), a);
	return remove(p) == 0 ? FR_OK : FR_NO_FILE;
}

/* As FatFs: no replacing a file that is there. */
FRESULT f_rename(const WCHAR *from, const WCHAR *to)
{
	char a[1024], b[1024], pa[1024], pb[1024];
	struct stat st;
	narrow(from, a, sizeof(a));
	narrow(to, b, sizeof(b));
	real_path(pa, sizeof(pa), a);
	real_path(pb, sizeof(pb), b);
	if (stat(pb, &st) == 0)
		return FR_DISK_ERR;
	return rename(pa, pb) == 0 ? FR_OK : FR_DISK_ERR;
}

const char *GetRootDevice(void)
{
	return getenv("SW_GAME_DEV");
}

int IsSupportedFileExt(const char *path)
{
	size_t n = strlen(path);
	return (n > 4 && (!strcmp(path + n - 4, ".iso") || !strcmp(path + n - 4, ".gcm"))) ||
	       (n > 5 && !strcmp(path + n - 5, ".ciso"));
}

void ReadRealDisc(u8 *buf, u64 offset, u32 len, u32 cmd)
{
	FILE *f = fopen(getenv("SW_DRIVE"), "rb");
	(void)cmd;
	memset(buf, 0, len);
	if (f) {
		fseek(f, (long)offset, SEEK_SET);
		if (fread(buf, 1, len, f) != len)
			;
		fclose(f);
	}
}

void PrintFormat(u8 size, u32 color, int x, int y, const char *fmt, ...)
{
	va_list ap;
	(void)size;
	(void)x;
	if (y <= last_y)
		printf("--\n");
	last_y = y;
	printf("SCREEN%s ", color == MAROON ? "!" : color == GREEN ? "+" : " ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	printf("\n");
}

void PrintBuildInfo(void) {}
void PrintButtonActions(const char *home, const char *a, const char *b, const char *x)
{
	(void)home;
	(void)x;
	if (a || b)
		printf("BUTTONS A=%s B=%s\n", a ? a : "-", b ? b : "-");
}
void GRRLIB_Render(void) { last_y = -1; }
void ClearScreen(void) {}
void VIDEO_WaitVSync(void) {}
void FPAD_Update(void) {}

/* The next scripted answer: 'A' or 'B'; none left is a test error. */
static int frames;
static int pressed(char want)
{
	if (++frames < 40)
		return 0;
	if (!answers)
		answers = getenv("SW_ANSWERS");
	if (!answers || !*answers) {
		printf("ERROR: a question with no answer left\n");
		exit(4);
	}
	if (*answers != want)
		return 0;
	printf("PRESS %c\n", want);
	answers++;
	frames = 0;
	return 1;
}
int FPAD_OK(int lock) { (void)lock; return pressed('A'); }
int FPAD_Cancel(int lock) { (void)lock; return pressed('B'); }
int FPAD_X(int lock) { (void)lock; return pressed('X'); }
int FPAD_Y(int lock) { (void)lock; return pressed('Y'); }
int FPAD_Up(int lock) { (void)lock; return pressed('U'); }
int FPAD_Down(int lock) { (void)lock; return pressed('D'); }
void LoaderShutdown(void) { exit(5); }
void ExitToLoader(int ret)
{
	printf("EXIT %d\n", ret);
	exit(3);
}
void FlushDevices(void) {}

#ifndef SW_NO_MAIN
int main(int argc, char **argv)
{
	unsigned cmd = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 0;
	strncpy(cfg.GamePath, argc > 1 ? argv[1] : "", sizeof(cfg.GamePath) - 1);
	memcpy(&cfg.GameID, "GALE", 4);
	cfg.Config = NIN_CFG_NETWORK;
	answers = getenv("SW_ANSWERS");
	if (getenv("SW_SD"))
		devices[DEV_SD] = &present;
	if (getenv("SW_USB"))
		devices[DEV_USB] = &present;
	SlippWii_Prepare(cmd, 0);
	printf("RESULT network=%d account=%s\n", (cfg.Config & NIN_CFG_NETWORK) ? 1 : 0, SlippWii_AccountLine);
	return 0;
}
#endif
