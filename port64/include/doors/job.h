/* DOORS jobs: a job is a program with its own address space, its tasks, exchanges and services
   (MMURTL JOBC.C / JOBCODE.ASM job control blocks). Job 1 is the OS/Monitor, job 2 is kept for
   the debugger (as in MMURTL), loaded programs start at job 3.

   Until the file system exists (M7), LoadNewJob finds programs among the GRUB boot modules by
   name; a program is a static ELF64 executable linked at USER_BASE (port64/user/). */
#ifndef DOORS_JOB_H
#define DOORS_JOB_H

#include <doors/erc.h>
#include <doors/types.h>

#define MAX_JOBS     64
#define FIRST_USER_JOB 3
#define ErcBadRunFile 74		/* not a run file (here: not a valid ELF64 program) */
#define ErcNoSuchFile 203		/* no such file (here: no such boot module) */
#define ErcCpuFault   1000		/* DOORS: job ended by CPU exception n -> 1000 + n */

struct cpu_regs;
struct regs;

struct job {
	bool used;
	bool alive;
	erc_t exit_erc;			/* why it ended (ExitJob code, or ErcCpuFault + vector) */
	char name[16];
	char cmdline[80];
};

void job_init(void);
const struct job *job_get(U32 job);
void job_load_initial(void);			/* INITIAL.JOB module: start the programs it lists */
NORETURN void job_fault(struct regs *r, U32 vector);	/* user-mode CPU exception */

/* ---- MMURTL calls (MJOB.H), same names and parameter order ---- */
erc_t LoadNewJob(const char *pFileName, U32 cbFileName, U32 *pJobNumRet);
NORETURN void ExitJob(erc_t dError);
erc_t KillJob(U32 JobNum);
erc_t GetJobNum(U32 *pJobNumRet);
erc_t SetJobName(const char *pName, U32 dcbName);
erc_t SetCmdLine(const char *pCmd, U32 dcbCmd);
erc_t GetCmdLine(char *pCmdRet, U32 *pdcbCmdRet);

#endif
