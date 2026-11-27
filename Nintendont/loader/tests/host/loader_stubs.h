/* Stand-ins for the loader's libogc, FatFS and screen functions, so
 * loader/source/slippwii.c and accounts.c build and run on a PC (tools/test_loader_slippwii.py,
 * tools/test_loader_accounts.py). "sd:/x" and "usb:/x" are folders ($SW_SD, $SW_USB); a real disc
 * is an image file ($SW_DRIVE); the screen is printed; the buttons come from $SW_ANSWERS (A, B, X,
 * Y, U for up, D for down). */
#ifndef LOADER_STUBS_H
#define LOADER_STUBS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef volatile uint32_t vu32;
typedef unsigned int UINT;
typedef unsigned char BYTE;

#define ALIGNED(x) __attribute__((aligned(x)))
#define ALIGN_FORWARD(x, align) ((typeof(x))((((uint32_t)(x)) + (align) - 1) & (~((align) - 1))))
#define memalign(a, n) malloc(n)

#define BLACK 0x000000FF
#define MAROON 0x800000FF
#define GREEN 0x00C108FF
#define GRAY 0x808080FF
#define DARK_BLUE 0x00173bFF
#define ARROW_RIGHT ">"
#define DEFAULT_SIZE 16
#define MENU_POS_X 25
#define MENU_POS_Y 34
#define DEV_SD 0
#define DEV_USB 1
#define NIN_CFG_NETWORK (1 << 13)

typedef struct {
	char GamePath[255];
	unsigned int Config;
	unsigned int GameID;
} NIN_CFG;
extern NIN_CFG *ncfg;

typedef enum { FR_OK = 0, FR_DISK_ERR, FR_NO_FILE } FRESULT;
#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_OPEN_EXISTING 0x00
#define FA_CREATE_ALWAYS 0x08
typedef struct {
	FILE *fp;
	u64 size;
} FIL;
typedef struct {
	int dummy;
} FATFS;
typedef uint16_t WCHAR;
#define AM_DIR 0x10
typedef struct {
	void *dir;
} FF_DIR;
#define DIR FF_DIR	/* (not the PC's own DIR) */
typedef struct {
	BYTE fattrib;
	WCHAR fname[256];
} FILINFO;

FRESULT f_open_char(FIL *fp, const char *path, BYTE mode);
FRESULT f_read(FIL *fp, void *buf, UINT n, UINT *got);
FRESULT f_write(FIL *fp, const void *buf, UINT n, UINT *wrote);
FRESULT f_lseek(FIL *fp, u64 offset);
FRESULT f_close(FIL *fp);
FRESULT f_mkdir_char(const char *path);
FRESULT f_opendir_char(DIR *dp, const char *path);
FRESULT f_readdir(DIR *dp, FILINFO *info);
FRESULT f_closedir(DIR *dp);
FRESULT f_unlink(const WCHAR *path);
FRESULT f_rename(const WCHAR *from, const WCHAR *to);
#define f_size(fp) ((fp)->size)

const char *GetRootDevice(void);
int IsSupportedFileExt(const char *path);
void ReadRealDisc(u8 *buf, u64 offset, u32 len, u32 cmd);
void PrintFormat(u8 size, u32 color, int x, int y, const char *fmt, ...);
void PrintBuildInfo(void);
void PrintButtonActions(const char *home, const char *a, const char *b, const char *x);
void GRRLIB_Render(void);
void ClearScreen(void);
void VIDEO_WaitVSync(void);
void FPAD_Update(void);
int FPAD_OK(int lock);
int FPAD_Cancel(int lock);
int FPAD_X(int lock);
int FPAD_Y(int lock);
int FPAD_Up(int lock);
int FPAD_Down(int lock);
void LoaderShutdown(void);
void ExitToLoader(int ret);
void FlushDevices(void);

#endif
