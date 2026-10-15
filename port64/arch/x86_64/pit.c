/* 8254 PIT channel 0 as the system tick (MMURTL TMRCODE.ASM: TimerTick, incremented every
   10 ms). The tick count is shown at the top right of the screen, like MMURTL's Monitor. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/task.h>
#include <doors/x86.h>

#define PIT_HZ   1193182u
#define PIT_CH0  0x40
#define PIT_CMD  0x43

static volatile U64 ticks;

static void timer_irq(struct regs *r)
{
	(void)r;
	ticks++;
	if (ticks % 10 == 0)
		vga_status_tick(ticks);
	sched_tick();		/* wake sleepers, time slicing; may switch tasks */
}

void pit_init(U32 hz)
{
	U32 div = (PIT_HZ + hz / 2) / hz;

	outb(PIT_CMD, 0x36);		/* channel 0, lo/hi byte, mode 3 (square wave) */
	outb(PIT_CH0, div & 0xFF);
	outb(PIT_CH0, (div >> 8) & 0xFF);
	irq_register(IRQ_TIMER, timer_irq);
}

U64 timer_ticks(void)
{
	return ticks;
}

void timer_sleep(U64 n)
{
	U64 end = ticks + n;

	while (ticks < end)
		hlt();			/* interrupts must be enabled */
}
