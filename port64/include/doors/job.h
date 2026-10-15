/* DOORS OS - Copyright (c) 2026 Joel Damaso <jammidee@gmail.com>, Philippines. */
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
#define ErcCpuFault   1000		/* DOORS: job ended by CPU exception n -> 1000 + n */

struct cpu_regs;
struct regs;

#define ErcBadJobParam 76		/* a string too long for its JCB field */

/* The job control block fields MMURTL kept (JCBRec in MJOB.H). Strings are NUL-terminated here. */
struct job {
	bool used;
	bool alive;
	erc_t exit_erc;			/* why it ended (ExitJob code, or ErcCpuFault + vector) */
	char name[16];
	char cmdline[80];
	char path[70];			/* SetPath: the default path prefix (CLI) */
	char user[30];			/* SetUserName */
	char exit_run[80];		/* SetExitJob: program loaded into the job when it exits */
	char sysin[50], sysout[50];	/* SetSysIn / SetSysOut */
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
erc_t Chain(const char *pFileName, U32 cbFileName, erc_t dExitError);
erc_t SetPath(const char *pPath, U32 dcbPath);
erc_t GetPath(U32 JobNum, char *pPathRet, U32 *pdcbPathRet);
erc_t SetUserName(const char *pUser, U32 dcbUser);
erc_t GetUserName(char *pUserRet, U32 *pdcbUserRet);
erc_t SetExitJob(const char *pRunFile, U32 dcbRunFile);
erc_t GetExitJob(char *pRunRet, U32 *pdcbRunRet);
erc_t SetSysIn(const char *pFile, U32 dcbFile);
erc_t GetSysIn(char *pFileRet, U32 *pdcbFileRet);
erc_t SetSysOut(const char *pFile, U32 dcbFile);
erc_t GetSysOut(char *pFileRet, U32 *pdcbFileRet);

#endif
