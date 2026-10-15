/* Kernel console: every character goes to both the VGA screen and COM1.
   kprintf supports %c %s %d %i %u %x %X %p %%, the l/ll/z length modifiers,
   a field width and the '0' and '-' flags. */

#include <stdarg.h>
#include <doors/console.h>
#include <doors/kernel.h>
#include <doors/x86.h>

void console_putc(char c)
{
	serial_putc(c);
	vga_putc(c);
}

void console_write(const char *s)
{
	while (*s)
		console_putc(*s++);
}

struct out {
	int n;
};

static void put(struct out *o, char c)
{
	console_putc(c);
	o->n++;
}

static void put_field(struct out *o, const char *s, int len, int width, bool left, char pad)
{
	int i;

	if (!left)
		for (i = len; i < width; i++)
			put(o, pad);
	for (i = 0; i < len; i++)
		put(o, s[i]);
	if (left)
		for (i = len; i < width; i++)
			put(o, ' ');
}

static int utoa(U64 v, unsigned base, bool upper, char *buf)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	char tmp[24];
	int n = 0, i;

	do {
		tmp[n++] = digits[v % base];
		v /= base;
	} while (v);
	for (i = 0; i < n; i++)
		buf[i] = tmp[n - 1 - i];
	return n;
}

static int vkprintf(const char *fmt, va_list ap)
{
	struct out o = { 0 };
	char buf[24];

	for (; *fmt; fmt++) {
		bool left = false, neg = false;
		char pad = ' ';
		int width = 0, lng = 0, len;
		U64 u;
		S64 s;
		const char *str;

		if (*fmt != '%') {
			put(&o, *fmt);
			continue;
		}
		fmt++;
		for (;; fmt++) {
			if (*fmt == '-')
				left = true;
			else if (*fmt == '0')
				pad = '0';
			else
				break;
		}
		while (*fmt >= '0' && *fmt <= '9')
			width = width * 10 + (*fmt++ - '0');
		while (*fmt == 'l' || *fmt == 'z') {
			lng++;
			fmt++;
		}

		switch (*fmt) {
		case 'c':
			buf[0] = (char)va_arg(ap, int);
			put_field(&o, buf, 1, width, left, ' ');
			break;
		case 's':
			str = va_arg(ap, const char *);
			if (!str)
				str = "(null)";
			for (len = 0; str[len]; len++)
				;
			put_field(&o, str, len, width, left, ' ');
			break;
		case 'd':
		case 'i':
			s = lng ? va_arg(ap, S64) : va_arg(ap, int);
			if (s < 0) {
				neg = true;
				u = (U64)(-(s + 1)) + 1;
			} else {
				u = (U64)s;
			}
			len = utoa(u, 10, false, buf + 1);
			if (neg) {
				if (pad == '0') {
					put(&o, '-');
					width--;
					put_field(&o, buf + 1, len, width, left, pad);
				} else {
					buf[0] = '-';
					put_field(&o, buf, len + 1, width, left, pad);
				}
			} else {
				put_field(&o, buf + 1, len, width, left, pad);
			}
			break;
		case 'u':
			u = lng ? va_arg(ap, U64) : va_arg(ap, unsigned);
			len = utoa(u, 10, false, buf);
			put_field(&o, buf, len, width, left, pad);
			break;
		case 'x':
		case 'X':
			u = lng ? va_arg(ap, U64) : va_arg(ap, unsigned);
			len = utoa(u, 16, *fmt == 'X', buf);
			put_field(&o, buf, len, width, left, pad);
			break;
		case 'p':
			u = (U64)(uintptr_t)va_arg(ap, void *);
			put(&o, '0');
			put(&o, 'x');
			len = utoa(u, 16, false, buf);
			put_field(&o, buf, len, 16, false, '0');
			break;
		case '%':
			put(&o, '%');
			break;
		case '\0':
			return o.n;
		default:
			put(&o, '%');
			put(&o, *fmt);
		}
	}
	return o.n;
}

int kprintf(const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vkprintf(fmt, ap);
	va_end(ap);
	return n;
}

void panic(const char *fmt, ...)
{
	va_list ap;

	vga_set_color(WHITE, RED);
	console_write("\nDOORS PANIC: ");
	va_start(ap, fmt);
	vkprintf(fmt, ap);
	va_end(ap);
	console_write("\n");
	if (test_mode)
		qemu_exit(QEMU_EXIT_FAIL);	/* end `make test` now instead of at its timeout */
	halt_forever();
}
