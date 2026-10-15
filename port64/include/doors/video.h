/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Virtual text video (MMURTL VIDEO.ASM, MVID.H) and the keyboard service (KEYBOARD.ASM, MKBD.H).

   Every job has its own 80x25 screen. The job that owns the video ("video owner") is the one you
   see; the others keep drawing into their own screens. The keyboard always follows the video
   owner: keys go to the job on screen. Ctrl-Alt-PgDn shows the next job, Ctrl-Alt-Del kills the
   job on screen (not the OS).

   Job 1 (the OS and its monitor) uses row 0 as a status line (title, clock, ticks); kernel
   messages scroll in rows 1-24 of job 1's screen. */
#ifndef DOORS_VIDEO_H
#define DOORS_VIDEO_H

#include <doors/erc.h>
#include <doors/types.h>

#define VID_COLS 80
#define VID_ROWS 25
#define NORM_ATTR 0x07			/* MMURTL default: white (7) on black */

#define ErcVidNum      300		/* bad video (job) number */
#define ErcVidParam    301		/* a parameter out of range */
#define ErcEditParam   300		/* bad parameter to EditLine (MMURTL reuses 300) */
#define ErcNoKeyAvail  700		/* ReadKbd(.., 0): no key waiting */

/* MMURTL key codes from ReadKbd: bits 0-7 key, 8-15 shift state, 16-23 locks, bit 24 numeric pad */
#define KBD_CTRL   0x0300
#define KBD_SHIFT  0x0C00
#define KBD_ALT    0x3000
#define KBD_NUMPAD 0x01000000

/* ---- kernel side ---- */
void vid_init(void);
void vid_remap(void);			/* after vmm_init(): reach VGA memory via the direct map */
void vid_set_title(const char *title);	/* job 1's status line, left part */
void vid_status_tick(U64 ticks);	/* timer IRQ: clock and tick counter on job 1's status line */
void vid_clock_base(U32 unix_time);	/* RTC time at boot, so the IRQ never reads the CMOS */
void vid_kputc(char c);			/* kernel console character on job 1's screen */
void vid_kattr(U8 attr);		/* attribute for kernel console text (panic: white on red) */
void vid_job_new(U32 job);		/* blank screen for a new job */
void vid_job_gone(U32 job);		/* the video owner ended: show job 1 again */
void vid_force_owner(U32 job);		/* panic: show job 1 no matter what */
U32  vid_owner(void);
void vid_next_owner(void);		/* Ctrl-Alt-PgDn */

void kbd_init(void);			/* hardware: IRQ 1 */
void kbd_service_start(void);		/* the keyboard task: delivers keys to the video owner */
void kbd_job_new(U32 job);
void kbd_job_gone(U32 job);
/* Test hooks from M2/M5: plain ASCII ('\n' for Enter) to an exchange, or a polling buffer. They are
   used before the keyboard service runs (kbd_set_exch(0) turns the exchange hook off). */
void kbd_set_exch(U32 exch);
int  kbd_getc(void);
void kbd_inject(U32 code);		/* self-test: a translated key, as if typed */

/* ---- MMURTL calls (MVID.H, MKBD.H), same names and parameter order ---- */
erc_t SetVidOwner(U32 dJobNum);
erc_t GetVidOwner(U32 *pdVidNumRet);
erc_t SetNormVid(U32 dAttr);
erc_t GetNormVid(U32 *pdNormVidRet);
erc_t ClrScr(void);
erc_t TTYOut(const char *pTextOut, U64 ddTextOut, U32 ddAttrib);
erc_t PutVidChars(U32 ddCol, U32 ddLine, const char *pChars, U32 sChars, U32 ddAttrib);
erc_t PutVidAttrs(U32 ddCol, U32 ddLine, U32 sChars, U32 dAttr);
erc_t GetVidChar(U32 ddCol, U32 ddLine, U8 *pCharRet, U8 *pAttrRet);
erc_t SetXY(U32 NewX, U32 NewY);
erc_t GetXY(U32 *pXRet, U32 *pYRet);
erc_t ScrollVid(U32 ddULCol, U32 ddULline, U32 nddCols, U32 nddLines, U32 ddfUp);
erc_t Beep(void);
erc_t Tone(U32 dHz, U32 dTicks10ms);
erc_t ReadKbd(U32 *pKeyCodeRet, U32 fWait);

#endif
