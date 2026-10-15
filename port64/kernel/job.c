/* Jobs: loading programs, ending jobs (MMURTL JOBC.C LoadNewJob/ExitJob/KillJob,
   JOBCODE.ASM GetJobNum/SetJobName...). See include/doors/job.h.

   Ending a job (ExitJob, KillJob, or a CPU exception in ring 3) tears it down in this order:
     1. its other tasks are killed (taken off the ready queue or the exchange they wait at);
     2. requests it made become orphans (services lose their aliases of its memory at once);
        requests it was serving are failed back to their clients (ErcNoSuchSvc);
     3. its services are unregistered and all its exchanges freed (queued requests are failed,
        waiting tasks woken, timers cancelled);
     4. its address space is destroyed (aliases of other jobs' pages are not freed);
     5. "Job number N terminated. Error: E" is reported, as MMURTL's Monitor did. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/job.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/multiboot2.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/x86.h>

static struct job jobs[MAX_JOBS];

void job_init(void)
{
	jobs[1].used = jobs[1].alive = true;
	memcpy(jobs[1].name, "DOORS OS", 9);
}

const struct job *job_get(U32 job)
{
	return job < MAX_JOBS && jobs[job].used ? &jobs[job] : NULL;
}

/* ---- ELF64 loader ---- */

struct elf64_ehdr {
	U8  ident[16];
	U16 type, machine;
	U32 version;
	U64 entry, phoff, shoff;
	U32 flags;
	U16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} PACKED;

struct elf64_phdr {
	U32 type, flags;
	U64 offset, vaddr, paddr, filesz, memsz, align;
} PACKED;

#define PT_LOAD 1
#define PF_X 1
#define PF_W 2

static erc_t elf_load(struct addr_space *as, const U8 *img, U64 size, U64 *entry)
{
	const struct elf64_ehdr *eh = (const struct elf64_ehdr *)img;
	U32 i;

	if (size < sizeof(*eh) || memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != 2 ||
	    eh->ident[5] != 1 || eh->type != 2 || eh->machine != 62 ||
	    eh->phentsize != sizeof(struct elf64_phdr) ||
	    eh->phoff + (U64)eh->phnum * sizeof(struct elf64_phdr) > size)
		return ErcBadRunFile;			/* not a static x86-64 executable */

	for (i = 0; i < eh->phnum; i++) {
		const struct elf64_phdr *ph = (const struct elf64_phdr *)(img + eh->phoff) + i;
		U64 va, end, flags;

		if (ph->type != PT_LOAD || !ph->memsz)
			continue;
		end = ph->vaddr + ph->memsz;
		if (ph->vaddr < USER_BASE || end > USER_STACK_TOP - (USER_STACK_PAGES + 1) * PAGE_SIZE ||
		    end < ph->vaddr || ph->filesz > ph->memsz || ph->offset + ph->filesz > size)
			return ErcBadRunFile;
		flags = PTE_U | ((ph->flags & PF_W) ? PTE_W : 0) | ((ph->flags & PF_X) ? 0 : PTE_NX);

		for (va = ph->vaddr & PAGE_MASK; va < end; va += PAGE_SIZE) {
			U64 pa, pte, lo, hi;

			if (vmm_translate(as, va, &pa, &pte)) {	/* page shared with the previous segment */
				pa &= PAGE_MASK;
				flags |= pte & PTE_W;
				if (!(pte & PTE_NX))
					flags &= ~PTE_NX;
			} else {
				pa = pmm_alloc();
				if (!pa)
					return ErcNoMem;
			}
			if (vmm_map(as, va, pa, flags))
				return ErcNoMem;

			/* copy the file bytes that fall into this page (the rest stays zero: .bss) */
			lo = va > ph->vaddr ? va : ph->vaddr;
			hi = va + PAGE_SIZE < ph->vaddr + ph->filesz ? va + PAGE_SIZE : ph->vaddr + ph->filesz;
			if (lo < hi)
				memcpy((U8 *)phys_to_virt(pa) + (lo - va), img + ph->offset + (lo - ph->vaddr),
				       hi - lo);
		}
	}
	if (eh->entry < USER_BASE || !vmm_translate(as, eh->entry, NULL, NULL))
		return ErcBadRunFile;
	*entry = eh->entry;
	return ErcOk;
}

static erc_t map_user_stack(struct addr_space *as)
{
	U64 va;

	for (va = USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE; va < USER_STACK_TOP; va += PAGE_SIZE) {
		U64 pa = pmm_alloc();

		if (!pa || vmm_map(as, va, pa, PTE_U | PTE_W | PTE_NX)) {
			if (pa)
				pmm_free(pa);
			return ErcNoMem;
		}
	}
	return ErcOk;		/* the page below stays unmapped: a guard */
}

/* ---- MMURTL calls ---- */

erc_t LoadNewJob(const char *pFileName, U32 cbFileName, U32 *pJobNumRet)
{
	const struct boot_module *m = boot_module(pFileName, cbFileName);
	struct addr_space *as;
	struct task *t;
	U64 entry, f;
	erc_t erc;
	U32 j;

	if (!m)
		return ErcNoSuchFile;
	f = irq_save();
	for (j = FIRST_USER_JOB; j < MAX_JOBS && jobs[j].used; j++)
		;
	if (j == MAX_JOBS) {
		irq_restore(f);
		return ErcNoMoreJCBs;
	}
	memset(&jobs[j], 0, sizeof(jobs[j]));
	jobs[j].used = true;		/* claimed */
	irq_restore(f);

	as = vmm_space_create(j);
	if (!as) {
		jobs[j].used = false;
		return ErcNoMem;
	}
	erc = elf_load(as, phys_to_virt(m->start), m->end - m->start, &entry);
	if (!erc)
		erc = map_user_stack(as);
	if (erc) {
		vmm_space_destroy(as);
		jobs[j].used = false;
		return erc;
	}
	memcpy(jobs[j].name, m->name, strlen(m->name) < 15 ? strlen(m->name) : 15);
	memcpy(jobs[j].cmdline, m->args, strlen(m->args) < 79 ? strlen(m->args) : 79);
	jobs[j].alive = true;

	t = task_create_user(m->name, j, entry, USER_STACK_TOP, DEFAULT_PRIORITY, &erc);
	if (!t) {
		vmm_space_destroy(as);
		jobs[j].used = jobs[j].alive = false;
		return erc;
	}
	if (pJobNumRet)
		*pJobNumRet = j;
	return ErcOk;
}

/* Tear a job down (see the top of this file). Returns normally unless the caller belongs to it. */
static void job_end(U32 job, erc_t erc)
{
	struct addr_space *as = job_space(job);
	bool self = current->job == job;
	U64 f = irq_save();

	sched_kill_job(job);
	rqb_job_gone(job);
	svc_job_gone(job);
	exch_free_job(job);
	if (self)
		vmm_space_switch(&kernel_space);	/* we are about to free the space we run in */
	if (as)
		vmm_space_destroy(as);
	jobs[job].alive = false;
	jobs[job].exit_erc = erc;
	kprintf("Job number %u terminated. Error: %u\n", job, erc);
	if (self)
		task_exit();			/* interrupts stay off; never returns */
	irq_restore(f);
}

void ExitJob(erc_t dError)
{
	if (current->job < FIRST_USER_JOB)
		panic("ExitJob from the OS job");
	job_end(current->job, dError);
	panic("ExitJob returned");
}

erc_t KillJob(U32 JobNum)
{
	if (JobNum < FIRST_USER_JOB || JobNum >= MAX_JOBS || !jobs[JobNum].alive)
		return ErcBadJobNum;
	job_end(JobNum, ErcOpCancel);	/* MMURTL: a killed job ends with ErcOpCancel (4) */
	return ErcOk;
}

void job_fault(struct regs *r, U32 vector)
{
	(void)r;
	job_end(current->job, ErcCpuFault + vector);
	panic("job_fault returned");
}

erc_t GetJobNum(U32 *pJobNumRet)
{
	*pJobNumRet = current->job;
	return ErcOk;
}

erc_t SetJobName(const char *pName, U32 dcbName)
{
	struct job *j = &jobs[current->job];

	if (dcbName > sizeof(j->name) - 1)
		dcbName = sizeof(j->name) - 1;
	memset(j->name, 0, sizeof(j->name));
	memcpy(j->name, pName, dcbName);
	return ErcOk;
}

erc_t SetCmdLine(const char *pCmd, U32 dcbCmd)
{
	struct job *j = &jobs[current->job];

	if (dcbCmd > sizeof(j->cmdline) - 1)
		dcbCmd = sizeof(j->cmdline) - 1;
	memset(j->cmdline, 0, sizeof(j->cmdline));
	memcpy(j->cmdline, pCmd, dcbCmd);
	return ErcOk;
}

erc_t GetCmdLine(char *pCmdRet, U32 *pdcbCmdRet)
{
	struct job *j = &jobs[current->job];
	U32 n = (U32)strlen(j->cmdline);

	memcpy(pCmdRet, j->cmdline, n);
	*pdcbCmdRet = n;
	return ErcOk;
}

/* INITIAL.JOB (MMURTL: \MMSYS\INITIAL.JOB): one program name per line, ';' starts a comment.
   Here it is a boot module of that name. */
void job_load_initial(void)
{
	const struct boot_module *m = boot_module("INITIAL.JOB", 11);
	const char *p, *end;

	if (!m)
		return;
	p = phys_to_virt(m->start);
	end = p + (m->end - m->start);
	while (p < end) {
		const char *s = p;
		U32 n = 0, job;
		erc_t erc;

		while (p < end && *p != '\n')
			p++;
		while (s + n < p && s[n] != ';' && s[n] != ' ' && s[n] != '\t' && s[n] != '\r')
			n++;
		p++;
		if (!n)
			continue;
		erc = LoadNewJob(s, n, &job);
		if (erc)
			kprintf("INITIAL.JOB: %.*s: error %u\n", (int)n, s, erc);
		else
			kprintf("INITIAL.JOB: loaded %.*s as job %u\n", (int)n, s, job);
	}
}
