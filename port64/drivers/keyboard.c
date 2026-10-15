/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* PS/2 keyboard (IRQ 1): scan code set 1 translated to MMURTL key codes exactly as
   KEYBOARD.ASM XLateRawKBD does (same tables): key in bits 0-7, shift state in 8-15, lock state
   in 16-23, bit 24 set for keys from the numeric pad. Every finished key goes as a message to
   the keyboard service task (kernel/kbdsvc.c). Keyboard LEDs are not driven (MMURTL SetKbdLEDS).

   Before the service runs, the M2/M5 self-tests use two hooks that see plain ASCII: a polling
   buffer (kbd_getc) and an exchange (kbd_set_exch). */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/msg.h>
#include <doors/video.h>
#include <doors/x86.h>

#define KBD_DATA 0x60

/* KbdTable: scan code (or the 60h-6Ch substitutes for E0 keys) -> key; high bit = numeric pad */
static const U8 kbd_table[128] = {
	0x00, 0x1B, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0x08, 0x09,	/* 00 */
	'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0x0D, 0x00, 'a', 's',	/* 10 */
	'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0x00, '\\', 'z', 'x', 'c', 'v',	/* 20 */
	'b', 'n', 'm', ',', '.', '/', 0x00, '*', 0x00, ' ', 0x00, 0x0F, 0x10, 0x11, 0x12, 0x13,	/* 30 */
	0x14, 0x15, 0x16, 0x17, 0x18, 0x00, 0x00, 0x86, 0x81, 0x85, 0xAD, 0x83, 0x9F, 0x84, 0xAB, 0x8B,/* 40 */
	0x82, 0x8C, 0x8E, 0xFF, 0x1C, 0x00, 0x00, 0x19, 0x1A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,/* 50 */
	0x0E, 0x0B, 0x02, 0x0C, 0x03, 0x00, 0x04, 0x06, 0x01, 0x05, 0x7F, 0xAF, 0x8D, 0x00, 0x00, 0x00,/* 60 */
};

/* KbdTable2: the byte after an E0 prefix -> a unique single code */
static const U8 kbd_table2[][2] = {
	{ 0x52, 0x60 }, { 0x4F, 0x61 }, { 0x50, 0x62 }, { 0x51, 0x63 },	/* Ins End Down PgDn */
	{ 0x4B, 0x64 }, { 0x4D, 0x66 }, { 0x47, 0x67 }, { 0x48, 0x68 },	/* Left Right Home Up */
	{ 0x49, 0x69 }, { 0x53, 0x6A }, { 0x35, 0x6B }, { 0x1C, 0x6C },	/* PgUp Del Num/ NumEnter */
	{ 0x38, 0x70 }, { 0x1D, 0x71 }, { 0xB8, 0xF0 }, { 0x9D, 0xF1 },	/* right Alt/Ctrl down, up */
};

/* KbdTableS: shift (and Caps/Num Lock) translation of a key */
static const U8 kbd_table_s[128] = {
	0x00, '8', '2', '4', '6', '9', '7', 0x07, 0x08, 0x09, 0x0A, '1', '3', 0x0D, '0', 0x0F,	/* 00 */
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x18, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, '5',/* 10 */
	' ', '!', '"', '#', '$', '%', '&', '"', '(', ')', '*', '+', '<', '_', '>', '?',	/* 20 */
	')', '!', '@', '#', '$', '%', '^', '&', '*', '(', ':', ':', '<', '+', '>', '?',	/* 30 */
	'@', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',	/* 40 */
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '{', '|', '}', '^', '_',	/* 50 */
	'~', 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K', 'L', 'M', 'N', 'O',	/* 60 */
	'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z', '{', '|', '}', '~', '.',	/* 70 */
};

/* KbdState bits (shift state byte) and KbdLock bits, as in KEYBOARD.ASM */
#define CTRL_L  0x01
#define CTRL_R  0x02
#define SHIFT_L 0x04
#define SHIFT_R 0x08
#define ALT_L   0x10
#define ALT_R   0x20
#define SCROLL_LOCK 0x01
#define NUM_LOCK    0x02
#define CAPS_LOCK   0x04

static U8 state, locks;
static bool prefix;
static volatile U32 service_exch, notify_exch;

/* Polling buffer for kbd_getc (M2 self-test). */
static volatile char buf[64];
static volatile U32 head, tail;

/* XLateRawKBD for one byte: returns the finished key code, or 0 (prefix, shift key, key up). */
static U32 translate(U8 sc)
{
	U32 i;
	U8 k, numpad;

	if (prefix) {
		prefix = false;
		for (i = 0; i < sizeof(kbd_table2) / sizeof(kbd_table2[0]); i++)
			if (kbd_table2[i][0] == sc)
				break;
		if (i == sizeof(kbd_table2) / sizeof(kbd_table2[0]))
			return 0;			/* no translation: ignore it */
		sc = kbd_table2[i][1];
	} else if (sc == 0xE0) {
		prefix = true;
		return 0;
	}
	switch (sc) {
	case 0x45: locks ^= NUM_LOCK; return 0;
	case 0x3A: locks ^= CAPS_LOCK; return 0;
	case 0x46: locks ^= SCROLL_LOCK; return 0;
	case 0x2A: state |= SHIFT_L; return 0;
	case 0x36: state |= SHIFT_R; return 0;
	case 0xAA: state &= ~SHIFT_L; return 0;
	case 0xB6: state &= ~SHIFT_R; return 0;
	case 0x1D: state |= CTRL_L; return 0;
	case 0x71: state |= CTRL_R; return 0;
	case 0x9D: state &= ~CTRL_L; return 0;
	case 0xF1: state &= ~CTRL_R; return 0;
	case 0x38: state |= ALT_L; return 0;
	case 0x70: state |= ALT_R; return 0;
	case 0xB8: state &= ~ALT_L; return 0;
	case 0xF0: state &= ~ALT_R; return 0;
	}
	if ((sc & 0x80) || !sc)
		return 0;				/* key up */
	k = kbd_table[sc & 0x7F];
	if (!k)
		return 0;
	numpad = (k & 0x80) ? 1 : 0;
	if (((state & (SHIFT_L | SHIFT_R)) && k >= 0x21 && k <= 0x7E) ||
	    ((k & 0x80) && (locks & NUM_LOCK) && k != 0xAD) ||
	    ((locks & CAPS_LOCK) && k >= 'a' && k <= 'z'))
		k = kbd_table_s[k & 0x7F];
	return (U32)numpad << 24 | (U32)locks << 16 | (U32)state << 8 | (k & 0x7F);
}

static void hooks(U32 code)
{
	char c = (char)(code & 0xFF);

	if (c == 0x0D)
		c = '\n';
	if (c != '\n' && c != '\b' && (c < ' ' || c > '~'))
		return;
	if (notify_exch) {
		ISendMsg(notify_exch, (U8)c, 0);
		return;
	}
	if (((head + 1) % sizeof(buf)) != tail) {	/* drop the key if the buffer is full */
		buf[head] = c;
		head = (head + 1) % sizeof(buf);
	}
}

void kbd_inject(U32 code)
{
	U64 f = irq_save();

	if (service_exch && !notify_exch)
		ISendMsg(service_exch, code, 0);
	else
		hooks(code);
	irq_restore(f);
}

static void kbd_irq(struct regs *r)
{
	U32 code = translate(inb(KBD_DATA));

	(void)r;
	if (code)
		kbd_inject(code);
}

/* Called by the keyboard service once it waits on its exchange. */
void kbd_set_service(U32 exch)
{
	service_exch = exch;
}

/* M5 test hook: keys as (ASCII, 0) messages to this exchange instead of the service; 0 = off. */
void kbd_set_exch(U32 exch)
{
	notify_exch = exch;
}

void kbd_init(void)
{
	while (inb(0x64) & 1)		/* flush anything the BIOS/GRUB left behind */
		inb(KBD_DATA);
	irq_register(IRQ_KEYBOARD, kbd_irq);
}

int kbd_getc(void)
{
	char c;

	if (head == tail)
		return -1;
	c = buf[tail];
	tail = (tail + 1) % sizeof(buf);
	return (unsigned char)c;
}
