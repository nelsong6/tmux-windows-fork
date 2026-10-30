/*
 * Process management for Windows.
 * Tracks child process handles and notifies the event loop when they exit.
 * Replaces SIGCHLD / waitpid() functionality.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"
#include <dbghelp.h>

#pragma comment(lib, "dbghelp.lib")

/* External: defined in win32-compat.c */
void win32_child_exited(pid_t pid, int status);

#define MAX_WATCHED 64

struct watched_process {
	HANDLE   hProcess;
	pid_t    pid;
	int      active;
};

static struct watched_process watched[MAX_WATCHED];
static int nwatched = 0;
static CRITICAL_SECTION watch_lock;
static HANDLE watcher_thread = NULL;
static HANDLE watcher_event = NULL; /* signaled when watch list changes */
static volatile int watcher_shutdown = 0;

static DWORD WINAPI
process_watcher_thread(LPVOID arg)
{
	HANDLE handles[MAX_WATCHED + 1];
	DWORD count, result;
	int i;

	(void)arg;

	for (;;) {
		if (watcher_shutdown)
			break;

		EnterCriticalSection(&watch_lock);
		count = 0;
		handles[count++] = watcher_event; /* always first */
		for (i = 0; i < nwatched; i++) {
			if (watched[i].active && count < MAX_WATCHED + 1)
				handles[count++] = watched[i].hProcess;
		}
		LeaveCriticalSection(&watch_lock);

		result = WaitForMultipleObjects(count, handles, FALSE, 1000);

		if (result == WAIT_OBJECT_0) {
			/* Watcher event signaled: list changed, re-loop. */
			ResetEvent(watcher_event);
			continue;
		}

		if (result == WAIT_TIMEOUT)
			continue;

		if (result >= WAIT_OBJECT_0 + 1 &&
		    result < WAIT_OBJECT_0 + count) {
			/* A child process exited. */
			DWORD idx = result - WAIT_OBJECT_0 - 1;
			DWORD exit_code = 0;
			pid_t pid;

			EnterCriticalSection(&watch_lock);
			/* Map back to watched array index. */
			DWORD wi = 0;
			for (i = 0; i < nwatched; i++) {
				if (watched[i].active) {
					if (wi == idx) {
						GetExitCodeProcess(watched[i].hProcess, &exit_code);
						pid = watched[i].pid;
						CloseHandle(watched[i].hProcess);
						watched[i].active = 0;

						/* Compact. */
						if (i == nwatched - 1)
							nwatched--;

						LeaveCriticalSection(&watch_lock);

						/* Encode exit code like Unix: (code << 8). */
						win32_child_exited(pid, (int)(exit_code << 8));

						/* Notify signal pipe (SIGCHLD). */
						win32_signal_notify(SIGCHLD);
						goto next;
					}
					wi++;
				}
			}
			LeaveCriticalSection(&watch_lock);
		}
next:;
	}
	return 0;
}

void
win32_process_init(void)
{
	InitializeCriticalSection(&watch_lock);
	watcher_event = CreateEvent(NULL, TRUE, FALSE, NULL);
	watcher_shutdown = 0;
	nwatched = 0;
	watcher_thread = CreateThread(NULL, 0, process_watcher_thread, NULL, 0, NULL);
}

void
win32_process_cleanup(void)
{
	int i;

	watcher_shutdown = 1;
	if (watcher_event)
		SetEvent(watcher_event);
	if (watcher_thread) {
		WaitForSingleObject(watcher_thread, 5000);
		CloseHandle(watcher_thread);
		watcher_thread = NULL;
	}
	if (watcher_event) {
		CloseHandle(watcher_event);
		watcher_event = NULL;
	}

	EnterCriticalSection(&watch_lock);
	for (i = 0; i < nwatched; i++) {
		if (watched[i].active)
			CloseHandle(watched[i].hProcess);
	}
	nwatched = 0;
	LeaveCriticalSection(&watch_lock);
	DeleteCriticalSection(&watch_lock);
}

void
win32_process_watch(HANDLE hProcess, pid_t pid)
{
	HANDLE dup;

	/* Duplicate the handle so we own it. */
	if (!DuplicateHandle(GetCurrentProcess(), hProcess,
	    GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
		return;

	EnterCriticalSection(&watch_lock);
	if (nwatched < MAX_WATCHED) {
		watched[nwatched].hProcess = dup;
		watched[nwatched].pid = pid;
		watched[nwatched].active = 1;
		nwatched++;
	} else {
		CloseHandle(dup);
	}
	LeaveCriticalSection(&watch_lock);

	/* Signal the watcher thread to re-read the list. */
	if (watcher_event)
		SetEvent(watcher_event);
}

/*
 * A child's stdio is a pair of pipes, copied to and from the server's socket by
 * two threads. A program can't use a socket as its stdout (cmd.exe and pwsh
 * both exited 1 with no output), and the CRT can't take one either.
 */
struct spawn_bridge {
	HANDLE		 out;	/* read end of the child's stdout/stderr */
	HANDLE		 in;	/* write end of the child's stdin */
	SOCKET		 sock;
	volatile LONG	 refs;
};

static void
spawn_bridge_release(struct spawn_bridge *b)
{
	if (InterlockedDecrement(&b->refs) != 0)
		return;
	closesocket(b->sock);
	free(b);
}

/* Child output to the server; at EOF the socket closes and the job sees EOF. */
static DWORD WINAPI
spawn_bridge_out(LPVOID arg)
{
	struct spawn_bridge	*b = arg;
	char			 buf[4096];
	DWORD			 n;

	while (ReadFile(b->out, buf, sizeof buf, &n, NULL) && n != 0) {
		if (send(b->sock, buf, (int)n, 0) <= 0)
			break;
	}
	CloseHandle(b->out);
	shutdown(b->sock, SD_SEND);
	spawn_bridge_release(b);
	return (0);
}

/* Server input to the child (pipe-pane -O). */
static DWORD WINAPI
spawn_bridge_in(LPVOID arg)
{
	struct spawn_bridge	*b = arg;
	char			 buf[4096];
	DWORD			 written;
	int			 n;

	while ((n = recv(b->sock, buf, sizeof buf, 0)) > 0) {
		if (!WriteFile(b->in, buf, (DWORD)n, &written, NULL))
			break;
	}
	CloseHandle(b->in);
	spawn_bridge_release(b);
	return (0);
}

struct write_close {
	int	 fd;
	size_t	 len;
	char	 data[];
};

static DWORD WINAPI
write_close_thread(LPVOID arg)
{
	struct write_close	*wc = arg;
	size_t			 off = 0;
	int			 n;

	while (off < wc->len) {
		n = _write(wc->fd, wc->data + off, (unsigned int)(wc->len - off));
		if (n <= 0)
			break;
		off += n;
	}
	_close(wc->fd);
	free(wc);
	return (0);
}

/*
 * Write data to a pipe and close it, on a thread of its own: a write to a pipe
 * blocks once the pipe is full, until the reader catches up, and the reader is
 * a child the server must keep serving meanwhile. The fd belongs to the thread.
 */
void
win32_write_close(int fd, const void *data, size_t len)
{
	struct write_close	*wc;
	HANDLE			 thread;

	wc = malloc(sizeof *wc + len);
	if (wc == NULL) {
		_close(fd);
		return;
	}
	wc->fd = fd;
	wc->len = len;
	memcpy(wc->data, data, len);

	thread = CreateThread(NULL, 0, write_close_thread, wc, 0, NULL);
	if (thread == NULL) {
		_close(fd);
		free(wc);
		return;
	}
	CloseHandle(thread);
}

/*
 * Spawn a non-PTY process with its stdin, stdout and stderr connected to the
 * given socket. The socket then belongs to the bridge, which closes it when
 * the child is done; the caller must not close it. Returns the child PID on
 * success, -1 on failure (the caller still owns the socket).
 */
pid_t
win32_process_spawn(const char *cmd, const char *cwd, int outfd)
{
	STARTUPINFOEXA si;
	PROCESS_INFORMATION pi;
	SECURITY_ATTRIBUTES sa;
	HANDLE outr = NULL, outw = NULL, inr = NULL, inw = NULL, threads[2];
	HANDLE inherit[2];
	struct spawn_bridge *b;
	SIZE_T attrsize = 0;
	char *cmddup;
	BOOL ok;

	memset(&sa, 0, sizeof sa);
	sa.nLength = sizeof sa;
	sa.bInheritHandle = TRUE;

	if (!CreatePipe(&outr, &outw, &sa, 0))
		return (-1);
	if (!CreatePipe(&inr, &inw, &sa, 0)) {
		CloseHandle(outr);
		CloseHandle(outw);
		return (-1);
	}
	SetHandleInformation(outr, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(inw, HANDLE_FLAG_INHERIT, 0);

	/* The child inherits its two pipe ends and nothing else of the server's. */
	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = inr;
	si.StartupInfo.hStdOutput = outw;
	si.StartupInfo.hStdError = outw;
	inherit[0] = inr;
	inherit[1] = outw;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attrsize);
	si.lpAttributeList = malloc(attrsize);
	ok = si.lpAttributeList != NULL &&
	    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0,
	    &attrsize) &&
	    UpdateProcThreadAttribute(si.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL,
	    NULL);

	memset(&pi, 0, sizeof pi);
	cmddup = _strdup(cmd);
	if (ok) {
		ok = CreateProcessA(NULL, cmddup, NULL, NULL, TRUE,
		    CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, NULL,
		    (cwd != NULL && *cwd != '\0') ? cwd : NULL,
		    &si.StartupInfo, &pi);
	}
	free(cmddup);
	if (si.lpAttributeList != NULL) {
		DeleteProcThreadAttributeList(si.lpAttributeList);
		free(si.lpAttributeList);
	}
	CloseHandle(outw);
	CloseHandle(inr);
	if (!ok) {
		CloseHandle(outr);
		CloseHandle(inw);
		return (-1);
	}
	CloseHandle(pi.hThread);

	b = calloc(1, sizeof *b);
	b->out = outr;
	b->in = inw;
	b->sock = (SOCKET)outfd;
	b->refs = 2;
	threads[0] = CreateThread(NULL, 0, spawn_bridge_out, b, 0, NULL);
	threads[1] = CreateThread(NULL, 0, spawn_bridge_in, b, 0, NULL);
	CloseHandle(threads[0]);
	CloseHandle(threads[1]);

	/* Watch the process for exit. */
	win32_process_watch(pi.hProcess, (pid_t)pi.dwProcessId);
	CloseHandle(pi.hProcess);

	return ((pid_t)pi.dwProcessId);
}

/*
 * On a crash, write the faulting call stack to stderr and the log, with
 * function names and lines when the PDB is beside the exe (a RelWithDebInfo
 * or Debug build). Without it the server died silently, with no debugger
 * installed to say where; a server started detached has no stderr at all.
 */
void log_debug(const char *, ...);

static LONG WINAPI
win32_crash_handler(EXCEPTION_POINTERS *ep)
{
	HANDLE			 proc = GetCurrentProcess(), thread = GetCurrentThread();
	CONTEXT			 ctx = *ep->ContextRecord;
	STACKFRAME64		 frame;
	char			 symbuf[sizeof(SYMBOL_INFO) + 256];
	SYMBOL_INFO		*sym = (SYMBOL_INFO *)symbuf;
	IMAGEHLP_LINE64		 line;
	DWORD64			 disp64;
	DWORD			 disp;
	char			 out[1024];
	size_t			 len;
	int			 i;

	snprintf(out, sizeof out, "tmux crashed: exception 0x%08lx at %p",
	    ep->ExceptionRecord->ExceptionCode,
	    ep->ExceptionRecord->ExceptionAddress);
	fprintf(stderr, "%s\n", out);
	log_debug("%s", out);
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
	SymInitialize(proc, NULL, TRUE);

	memset(&frame, 0, sizeof frame);
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrPC.Mode = AddrModeFlat;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrFrame.Mode = AddrModeFlat;
	frame.AddrStack.Offset = ctx.Rsp;
	frame.AddrStack.Mode = AddrModeFlat;
	for (i = 0; i < 32; i++) {
		if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, thread, &frame,
		    &ctx, NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL))
			break;
		memset(symbuf, 0, sizeof symbuf);
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		memset(&line, 0, sizeof line);
		line.SizeOfStruct = sizeof line;
		len = snprintf(out, sizeof out, "  #%d %s", i,
		    SymFromAddr(proc, frame.AddrPC.Offset, &disp64, sym) ?
		    sym->Name : "?");
		if (len < sizeof out &&
		    SymGetLineFromAddr64(proc, frame.AddrPC.Offset, &disp, &line))
			snprintf(out + len, sizeof out - len, " %s:%lu",
			    line.FileName, line.LineNumber);
		fprintf(stderr, "%s\n", out);
		log_debug("%s", out);
	}
	fflush(NULL);
	return (EXCEPTION_CONTINUE_SEARCH);
}

void
win32_crash_init(void)
{
	SetUnhandledExceptionFilter(win32_crash_handler);
}

/* Defined in cfg.c. */
extern char **cfg_files;
extern unsigned int cfg_nfiles;
extern int cfg_quiet;
int log_get_level(void);

/*
 * Launch a detached tmux server process.
 * Runs: tmux.exe [-v...] -D -S "<label>" [-f "<file>"]...
 * The -D flag tells tmux to run as a foreground server (no fork).
 * On Unix the forked server inherits cfg_files; here they must be passed on
 * the command line, or a client's -f files never reach the server. cfg_quiet
 * is clear only when -f was given.
 */
void
win32_launch_server(const char *label)
{
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	char exe[MAX_PATH];
	char cmdline[8192];
	size_t len;
	unsigned int i;

	if (GetModuleFileNameA(NULL, exe, sizeof exe) == 0)
		return;

	len = snprintf(cmdline, sizeof cmdline,
	    "\"%s\" %s%s-D -S \"%s\"", exe, log_get_level() > 0 ? "-v " : "",
	    log_get_level() > 1 ? "-v " : "", label);
	for (i = 0; !cfg_quiet && i < cfg_nfiles && len < sizeof cmdline; i++)
		len += snprintf(cmdline + len, sizeof cmdline - len,
		    " -f \"%s\"", cfg_files[i]);
	if (len >= sizeof cmdline)
		return;

	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	memset(&pi, 0, sizeof pi);

	/*
	 * CREATE_BREAKAWAY_FROM_JOB detaches the server from any job object
	 * the parent belongs to (e.g., sshd's session job). Without this,
	 * closing an SSH connection kills the tmux server along with all
	 * processes in the job. Fall back without the flag if the job
	 * doesn't allow breakaway (ERROR_ACCESS_DENIED).
	 */
	if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
	    DETACHED_PROCESS | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
	    NULL, NULL, &si, &pi)) {
		if (GetLastError() != ERROR_ACCESS_DENIED)
			return;
		if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
		    DETACHED_PROCESS | CREATE_NO_WINDOW, NULL, NULL,
		    &si, &pi))
			return;
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
}

void
win32_process_unwatch(pid_t pid)
{
	int i;

	EnterCriticalSection(&watch_lock);
	for (i = 0; i < nwatched; i++) {
		if (watched[i].active && watched[i].pid == pid) {
			CloseHandle(watched[i].hProcess);
			watched[i].active = 0;
			if (i == nwatched - 1)
				nwatched--;
			break;
		}
	}
	LeaveCriticalSection(&watch_lock);

	if (watcher_event)
		SetEvent(watcher_event);
}
