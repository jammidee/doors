/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
/* Jobs: loading programs, ending jobs (MMURTL JOBC.C LoadNewJob/ExitJob/KillJob,
   JOBCODE.ASM GetJobNum/SetJobName...). See include/doors/job.h.

   Ending a job (ExitJob, KillJob, or a CPU exception in ring 3) tears it down in this order:
     1. its other tasks are killed (taken off the ready queue or the exchange they wait at);
     2. requests it made become orphans (services lose their aliases of its memory at once);
        requests it was serving are failed back to their clients (ErcNoSuchSvc);
     3. its services are unregistered and all its exchanges freed (queued requests are failed,
        waiting tasks woken, timers cancelled);
     4. its address space is destroyed (aliases of other jobs' pages are not freed);
     5. its screen and key buffer are dropped (the monitor is shown if it was on screen);
     6. "Job number N terminated. Error: E" is reported, as MMURTL's Monitor did. */

#include <doors/console.h>
#include <doors/cpu.h>
#include <doors/fs.h>
#include <doors/job.h>
#include <doors/mem.h>
#include <doors/msg.h>
#include <doors/multiboot2.h>
#include <doors/string.h>
#include <doors/task.h>
#include <doors/video.h>
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

/* Read a whole program file (path "C:\..."). The buffer comes from AllocOSPage. */
static erc_t read_file(const char *path, U32 len, U8 **buf, U64 *size, U32 *npages)
{
	U32 h;
	U64 got;
	erc_t erc;

	if ((erc = OpenFile(path, len, ModeRead, 1, &h)))
		return erc;
	if ((erc = GetFileSize(h, size)) == ErcOk) {
		if (!*size || *size > 64UL * 1024 * 1024) {
			erc = ErcBadRunFile;
		} else {
			*npages = (U32)(PAGE_ROUND_UP(*size) / PAGE_SIZE);
			erc = AllocOSPage(*npages, buf);
			if (!erc) {
				erc = ReadBytes(h, *buf, *size, &got);
				if (erc == ErcEOF && got == *size)
					erc = ErcOk;
				if (erc)
					DeAllocPage(*buf, *npages);
			}
		}
	}
	CloseFile(h);
	return erc;
}

/* A program to load. MMURTL loaded a .RUN file by path. DOORS loads an ELF64 program from a boot
   module whose name matches (no drive or directory given), or else from a file; a file name without
   a drive is relative to the job's path ("C:\BIN\HELLO.ELF", "hello", "HELLO.ELF"). */
struct program {
	const U8 *img;
	U64 size;
	U32 npages;			/* != 0: img came from AllocOSPage (a file) */
	const char *name, *args;
	U32 nlen;
};

static erc_t find_program(const char *path, U32 len, struct program *p)
{
	const struct boot_module *m;
	U8 *buf;
	U32 i;
	erc_t erc;

	memset(p, 0, sizeof(*p));
	p->name = path;
	for (i = 0; i < len; i++)
		if (path[i] == ':' || path[i] == '\\' || path[i] == '/')
			p->name = path + i + 1;
	m = p->name == path ? boot_module(path, len) : NULL;
	if (m) {
		p->img = phys_to_virt(m->start);
		p->size = m->end - m->start;
		p->name = m->name;
		p->nlen = (U32)strlen(m->name);
		p->args = m->args;
		return ErcOk;
	}
	/* a file; a name without drive is relative to the caller's path (SetPath), as in MMURTL */
	if ((erc = read_file(path, len, &buf, &p->size, &p->npages)))
		return p->name == path && erc == ErcBadFileSpec ? ErcNoSuchFile : erc;	/* no path set */
	p->img = buf;
	p->nlen = (U32)(path + len - p->name);
	p->args = "";
	return ErcOk;
}

static void free_program(struct program *p)
{
	if (p->npages)
		DeAllocPage((U8 *)p->img, p->npages);
}

/* Load a program into job j: a new address space, the code, the stack, the first task.
   The job's old address space must be gone. */
static erc_t build(U32 j, const struct program *p)
{
	struct addr_space *as;
	struct task *t;
	U64 entry;
	erc_t erc;
	char tname[16];

	as = vmm_space_create(j);
	if (!as)
		return ErcNoMem;
	erc = elf_load(as, p->img, p->size, &entry);
	if (!erc)
		erc = map_user_stack(as);
	if (erc) {
		vmm_space_destroy(as);
		return erc;
	}
	memset(jobs[j].name, 0, sizeof(jobs[j].name));
	memcpy(jobs[j].name, p->name, p->nlen < 15 ? p->nlen : 15);
	memset(tname, 0, sizeof(tname));
	memcpy(tname, p->name, p->nlen < 15 ? p->nlen : 15);
	jobs[j].alive = true;
	jobs[j].loads++;
	t = task_create_user(tname, j, entry, USER_STACK_TOP, DEFAULT_PRIORITY, &erc);
	if (!t) {
		vmm_space_destroy(as);
		jobs[j].alive = false;
		return erc;
	}
	return ErcOk;
}

erc_t LoadNewJob(const char *pFileName, U32 cbFileName, U32 *pJobNumRet)
{
	struct program p;
	U64 f;
	erc_t erc;
	U32 j;

	if ((erc = find_program(pFileName, cbFileName, &p)))
		return erc;
	f = irq_save();
	for (j = FIRST_USER_JOB; j < MAX_JOBS && jobs[j].used; j++)
		;
	if (j == MAX_JOBS) {
		irq_restore(f);
		free_program(&p);
		return ErcNoMoreJCBs;
	}
	memset(&jobs[j], 0, sizeof(jobs[j]));
	jobs[j].used = true;		/* claimed */
	irq_restore(f);
	vid_job_new(j);			/* a blank screen of its own, and an empty key buffer */
	kbd_job_new(j);
	memcpy(jobs[j].cmdline, p.args, strlen(p.args) < 79 ? strlen(p.args) : 79);
	erc = build(j, &p);
	free_program(&p);
	if (erc) {
		jobs[j].used = false;
		return erc;
	}
	if (pJobNumRet)
		*pJobNumRet = j;
	return ErcOk;
}

/* Steps 1-4 of the teardown (top of this file). Interrupts are off. */
static void teardown(U32 job)
{
	struct addr_space *as = job_space(job);

	sched_kill_job(job);
	rqb_job_gone(job);
	svc_job_gone(job);
	fsys_job_gone(job);
	exch_free_job(job);
	if (current->job == job)
		vmm_space_switch(&kernel_space);	/* we are about to free the space we run in */
	if (as)
		vmm_space_destroy(as);
}

/* Tear a job down. Returns normally unless the caller belongs to it. */
static void job_end(U32 job, erc_t erc)
{
	bool self = current->job == job;
	U64 f = irq_save();

	teardown(job);
	jobs[job].alive = false;
	jobs[job].exit_erc = erc;
	vid_job_gone(job);			/* if it was on screen, the monitor (job 1) is shown */
	kbd_job_gone(job);
	kprintf("Job number %u terminated. Error: %u\n", job, erc);
	if (self)
		task_exit();			/* interrupts stay off; never returns */
	irq_restore(f);
}

/* Chain and ExitJob with an exit run file: the calling job's program is replaced by p. The job
   keeps its number, screen, keys, command line, path, user name and exit run file (MMURTL keeps the
   JCB); its tasks, exchanges, services, open files and memory go, as at the end of a job. Called by
   a task of that job; never returns. */
static NORETURN void replace(struct program *p, erc_t erc)
{
	U32 job = current->job;
	U64 f = irq_save();

	(void)f;
	teardown(job);
	jobs[job].exit_erc = erc;
	if (build(job, p)) {
		jobs[job].alive = false;
		vid_job_gone(job);
		kbd_job_gone(job);
		kprintf("Job number %u terminated. Error: %u\n", job, erc);
	}
	free_program(p);
	task_exit();
}

/* MMURTL ExitJob: if an exit run file is set (SetExitJob), it is loaded into this job (that is how
   the CLI comes back after a program it ran); otherwise the job ends. */
void ExitJob(erc_t dError)
{
	struct job *jb = &jobs[current->job];
	struct program p;

	if (current->job < FIRST_USER_JOB)
		panic("ExitJob from the OS job");
	if (jb->exit_run[0]) {
		erc_t erc = find_program(jb->exit_run, (U32)strlen(jb->exit_run), &p);

		if (!erc)
			replace(&p, dError);
		kprintf("Job %u: exit run file %s: error %u\n", current->job, jb->exit_run, erc);
	}
	job_end(current->job, dError);
	panic("ExitJob returned");
}

/* MMURTL Chain: run another program in this job (it inherits the command line, path, screen...)
   and pass dExitError on. Returns only if the program can't be loaded. */
erc_t Chain(const char *pFileName, U32 cbFileName, erc_t dExitError)
{
	struct program p;
	erc_t erc;

	if (current->job < FIRST_USER_JOB)
		return ErcBadJobNum;
	if ((erc = find_program(pFileName, cbFileName, &p)))
		return erc;
	replace(&p, dExitError);
}

/* KillJob always ends the job: its exit run file is not loaded. */
erc_t KillJob(U32 JobNum)
{
	if (JobNum < FIRST_USER_JOB || JobNum >= MAX_JOBS || !jobs[JobNum].alive)
		return ErcBadJobNum;
	job_end(JobNum, ErcOpCancel);	/* MMURTL: a killed job ends with ErcOpCancel (4) */
	return ErcOk;
}

/* A CPU exception in ring 3 ends the job (MMURTL entered the debugger). The exit run file is not
   loaded: the fault arrives in an exception handler, where loading a file can't wait for the disk. */
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

/* The other JCB strings (MMURTL JOBC.C): too long gives ErcBadJobParam, length 0 clears. */
static erc_t set_str(char *field, U32 size, const char *s, U32 n)
{
	if (n > size - 1)
		return ErcBadJobParam;
	memset(field, 0, size);
	memcpy(field, s, n);
	return ErcOk;
}

static erc_t get_str(const char *field, char *ret, U32 *pn)
{
	U32 n = (U32)strlen(field);

	memcpy(ret, field, n);
	*pn = n;
	return ErcOk;
}

#define ME (&jobs[current->job])

erc_t SetPath(const char *p, U32 n)		{ return set_str(ME->path, sizeof(ME->path), p, n); }
erc_t SetUserName(const char *p, U32 n)		{ return set_str(ME->user, sizeof(ME->user), p, n); }
erc_t SetExitJob(const char *p, U32 n)		{ return set_str(ME->exit_run, sizeof(ME->exit_run), p, n); }
erc_t SetSysIn(const char *p, U32 n)		{ return set_str(ME->sysin, sizeof(ME->sysin), p, n); }
erc_t SetSysOut(const char *p, U32 n)		{ return set_str(ME->sysout, sizeof(ME->sysout), p, n); }
erc_t GetUserName(char *p, U32 *n)		{ return get_str(ME->user, p, n); }
erc_t GetExitJob(char *p, U32 *n)		{ return get_str(ME->exit_run, p, n); }
erc_t GetSysIn(char *p, U32 *n)			{ return get_str(ME->sysin, p, n); }
erc_t GetSysOut(char *p, U32 *n)		{ return get_str(ME->sysout, p, n); }

/* GetPath takes a job number (MMURTL: so the CLI can show another job's path). */
erc_t GetPath(U32 JobNum, char *pPathRet, U32 *pdcbPathRet)
{
	if (JobNum >= MAX_JOBS || !jobs[JobNum].used)
		return ErcBadJobNum;
	return get_str(jobs[JobNum].path, pPathRet, pdcbPathRet);
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
		else {
			kprintf("INITIAL.JOB: loaded %.*s as job %u\n", (int)n, s, job);
			SetVidOwner(job);	/* as MMURTL's Monitor: the new job gets the screen */
		}
	}
}
