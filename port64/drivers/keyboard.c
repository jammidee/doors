/* PS/2 keyboard (IRQ 1), scan code set 1 -> ASCII, with Shift and Caps Lock.
   A first step towards MMURTL's keyboard service (KEYBOARD.ASM); key codes for function and
   cursor keys, Ctrl/Alt combinations and the per-job keyboard owner come with the CLI (M9). */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/msg.h>
#include <doors/x86.h>

static volatile U32 notify_exch;

#define KBD_DATA 0x60

static const char normal[128] = {
	0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
	'\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
	0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
	0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
	'*', 0, ' ',
};

static const char shifted[128] = {
	0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
	'\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
	0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
	0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
	'*', 0, ' ',
};

#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36
#define SC_CAPS   0x3A

static bool shift, caps;

/* Single-producer (IRQ) / single-consumer ring buffer. */
static volatile char buf[64];
static volatile U32 head, tail;

static void kbd_irq(struct regs *r)
{
	U8 sc = inb(KBD_DATA);
	bool release = sc & 0x80;
	char c;

	(void)r;
	sc &= 0x7F;
	if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
		shift = !release;
		return;
	}
	if (release)
		return;
	if (sc == SC_CAPS) {
		caps = !caps;
		return;
	}

	c = shift ? shifted[sc] : normal[sc];
	if (caps && c >= 'a' && c <= 'z')
		c -= 32;
	else if (caps && c >= 'A' && c <= 'Z')
		c += 32;
	if (!c)
		return;

	if (notify_exch) {		/* a task waits on an exchange: message it (MMURTL style) */
		ISendMsg(notify_exch, (U8)c, 0);
		return;
	}
	if (((head + 1) % sizeof(buf)) != tail) {	/* drop the key if the buffer is full */
		buf[head] = c;
		head = (head + 1) % sizeof(buf);
	}
}

/* Deliver keys as messages (key, 0) to this exchange instead of the polling buffer; 0 = off. */
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
