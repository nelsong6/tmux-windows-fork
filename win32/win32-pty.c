/*
 * ConPTY wrapper for Windows.
 * Creates pseudo-consoles and spawns child processes with them.
 * Bridges ConPTY pipe output to a socket for libevent integration.
 */

#include <sys/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "win32-platform.h"
#include <event2/buffer.h>

struct win32_pty {
	HPCON    hPC;        /* ConPTY handle */
	HANDLE   hPipeIn;    /* Write end: input to PTY */
	HANDLE   hPipeOut;   /* Read end: output from PTY */
	HANDLE   hProcess;   /* Child process handle */
	HANDLE   hThread;    /* Child main thread handle */
	DWORD    dwProcessId;
	SOCKET   sock;       /* Socket FD for libevent (read end of bridge) */
	SOCKET   bridge_peer;/* Write end of bridge socket pair */
	HANDLE   bridge_thread; /* Thread: hPipeOut -> bridge_peer (output) */
	HANDLE   input_thread;  /* Thread: bridge_peer -> hPipeIn (input) */
	volatile int closing;
};

/*
 * The pseudoconsole implementation. Windows' built-in one (kernel32, served by
 * the inbox conhost.exe) lags Windows Terminal's by a release or more, and
 * Claude Code's redraws came out garbled through it. conpty.dll with
 * OpenConsole.exe beside tmux.exe (Microsoft.Windows.Console.ConPTY, the build
 * Windows Terminal ships) is used when present.
 */
struct pty_api {
	HRESULT	(WINAPI *create)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
	HRESULT	(WINAPI *resize)(HPCON, COORD);
	void	(WINAPI *destroy)(HPCON);
	HRESULT	(WINAPI *pack)(HANDLE, HANDLE, HANDLE, HPCON *);
};

/*
 * What an HPCON from conpty.dll points to (src/winconpty/winconpty.h in
 * microsoft/terminal). ConptyPackPseudoConsole builds one from these handles
 * in another process, which is how a console changes hands.
 */
struct pty_conpty {
	HANDLE	signal;
	HANDLE	reference;
	HANDLE	process;
};

static const struct pty_api *
pty_api(void)
{
	static struct pty_api	 api;
	static int		 loaded;
	wchar_t			 path[MAX_PATH], *slash;
	HMODULE			 m;

	if (loaded)
		return (&api);
	loaded = 1;

	if (GetModuleFileNameW(NULL, path, MAX_PATH) != 0 &&
	    (slash = wcsrchr(path, L'\\')) != NULL &&
	    wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"conpty.dll") == 0 &&
	    (m = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH)) != NULL) {
		api.create = (void *)GetProcAddress(m, "ConptyCreatePseudoConsole");
		api.resize = (void *)GetProcAddress(m, "ConptyResizePseudoConsole");
		api.destroy = (void *)GetProcAddress(m, "ConptyClosePseudoConsole");
		api.pack = (void *)GetProcAddress(m, "ConptyPackPseudoConsole");
		if (api.create != NULL && api.resize != NULL && api.destroy != NULL)
			return (&api);
		FreeLibrary(m);
	}
	api.create = CreatePseudoConsole;
	api.resize = ResizePseudoConsole;
	api.destroy = ClosePseudoConsole;
	api.pack = NULL;
	return (&api);
}

/* Output bridge: reads from ConPTY pipe, sends to socket (child -> server). */
static DWORD WINAPI
pty_bridge_thread(LPVOID arg)
{
	struct win32_pty *pty = (struct win32_pty *)arg;
	char buf[4096];
	DWORD n;

	while (!pty->closing) {
		if (!ReadFile(pty->hPipeOut, buf, sizeof buf, &n, NULL))
			break;
		if (n == 0)
			break;
		/* Write to bridge socket so libevent can pick it up. */
		if (send(pty->bridge_peer, buf, (int)n, 0) <= 0)
			break;
	}

	/* Signal EOF by closing the socket. */
	closesocket(pty->bridge_peer);
	pty->bridge_peer = INVALID_SOCKET;
	return 0;
}

/* Input bridge: reads from socket, writes to ConPTY pipe (server -> child). */
static DWORD WINAPI
pty_input_thread(LPVOID arg)
{
	struct win32_pty *pty = (struct win32_pty *)arg;
	char buf[4096];
	int n;
	DWORD written;

	while (!pty->closing) {
		n = recv(pty->bridge_peer, buf, sizeof buf, 0);
		if (n <= 0)
			break;
		if (!WriteFile(pty->hPipeIn, buf, (DWORD)n, &written, NULL))
			break;
	}
	return 0;
}

/*
 * Spawn cmd in a new pseudo console. A standard handle is the console's own
 * unless infd or outfd (a CRT descriptor, -1 for none) is given for it; only
 * those are inherited.
 */
struct win32_pty *
win32_pty_spawn(const char *cmd, const char *cwd, char *env,
    int cols, int rows, pid_t *out_pid, int infd, int outfd)
{
	struct win32_pty *pty;
	HANDLE pipeIn_read = NULL, pipeIn_write = NULL;
	HANDLE pipeOut_read = NULL, pipeOut_write = NULL;
	HRESULT hr;
	COORD size;
	STARTUPINFOEXW si;
	PROCESS_INFORMATION pi;
	SIZE_T attrSize = 0;
	HANDLE inherit[2];
	DWORD attrs = 1, ninherit = 0;
	int sv[2];
	wchar_t wcmd[32768];
	wchar_t wcwd[MAX_PATH];

	pty = calloc(1, sizeof *pty);
	if (pty == NULL)
		return (NULL);
	pty->sock = INVALID_SOCKET;
	pty->bridge_peer = INVALID_SOCKET;
	pty->hPC = NULL;

	/* Create pipes for ConPTY. */
	if (!CreatePipe(&pipeIn_read, &pipeIn_write, NULL, 0))
		goto fail;
	if (!CreatePipe(&pipeOut_read, &pipeOut_write, NULL, 0))
		goto fail;

	/* Create pseudo console. */
	size.X = (SHORT)cols;
	size.Y = (SHORT)rows;
	hr = pty_api()->create(size, pipeIn_read, pipeOut_write, 0, &pty->hPC);
	if (FAILED(hr))
		goto fail;

	/* Close the sides of the pipes that ConPTY now owns. */
	CloseHandle(pipeIn_read);
	pipeIn_read = NULL;
	CloseHandle(pipeOut_write);
	pipeOut_write = NULL;

	pty->hPipeIn = pipeIn_write;
	pty->hPipeOut = pipeOut_read;

	/* Set up STARTUPINFOEX with the pseudo console attribute. */
	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;

	/*
	 * Force the child to get its standard handles from the pseudo console
	 * rather than inheriting the parent's handles. Without this, cmd.exe
	 * and other shells get non-console STD handles (pipes/files from the
	 * parent's context), causing them to treat stdin as non-interactive
	 * and exit immediately.
	 */
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = INVALID_HANDLE_VALUE;
	si.StartupInfo.hStdOutput = INVALID_HANDLE_VALUE;
	si.StartupInfo.hStdError = INVALID_HANDLE_VALUE;
	if (infd != -1) {
		if (!DuplicateHandle(GetCurrentProcess(),
		    (HANDLE)_get_osfhandle(infd), GetCurrentProcess(),
		    &si.StartupInfo.hStdInput, 0, TRUE, DUPLICATE_SAME_ACCESS))
			goto fail;
		inherit[ninherit++] = si.StartupInfo.hStdInput;
	}
	if (outfd != -1) {
		if (!DuplicateHandle(GetCurrentProcess(),
		    (HANDLE)_get_osfhandle(outfd), GetCurrentProcess(),
		    &si.StartupInfo.hStdOutput, 0, TRUE, DUPLICATE_SAME_ACCESS))
			goto fail;
		inherit[ninherit++] = si.StartupInfo.hStdOutput;
	}
	if (ninherit != 0)
		attrs++;

	InitializeProcThreadAttributeList(NULL, attrs, 0, &attrSize);
	si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attrSize);
	if (si.lpAttributeList == NULL)
		goto fail;
	if (!InitializeProcThreadAttributeList(si.lpAttributeList, attrs, 0,
	    &attrSize))
		goto fail;
	if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, pty->hPC, sizeof(HPCON),
	    NULL, NULL))
		goto fail;
	if (ninherit != 0 && !UpdateProcThreadAttribute(si.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
	    ninherit * sizeof inherit[0], NULL, NULL))
		goto fail;

	/* Convert command and cwd to wide strings. */
	MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, sizeof wcmd / sizeof wcmd[0]);
	if (cwd != NULL && *cwd != '\0')
		MultiByteToWideChar(CP_UTF8, 0, cwd, -1, wcwd, sizeof wcwd / sizeof wcwd[0]);

	/* Create the child process. */
	memset(&pi, 0, sizeof pi);
	{
		DWORD flags = EXTENDED_STARTUPINFO_PRESENT;
		if (env != NULL)
			flags |= CREATE_UNICODE_ENVIRONMENT;
		if (!CreateProcessW(NULL, wcmd, NULL, NULL, ninherit != 0,
		    flags, env,
		    (cwd != NULL && *cwd != '\0') ? wcwd : NULL,
		    &si.StartupInfo, &pi))
			goto fail;
	}
	while (ninherit != 0)
		CloseHandle(inherit[--ninherit]);

	pty->hProcess = pi.hProcess;
	pty->hThread = pi.hThread;
	pty->dwProcessId = pi.dwProcessId;

	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);

	/* Create bridge socketpair. */
	if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0)
		goto fail;
	pty->sock = (SOCKET)sv[0];
	pty->bridge_peer = (SOCKET)sv[1];

	/* Start bridge threads (output and input). */
	pty->closing = 0;
	pty->bridge_thread = CreateThread(NULL, 0, pty_bridge_thread, pty, 0, NULL);
	if (pty->bridge_thread == NULL)
		goto fail;
	pty->input_thread = CreateThread(NULL, 0, pty_input_thread, pty, 0, NULL);
	if (pty->input_thread == NULL)
		goto fail;

	if (out_pid != NULL)
		*out_pid = (pid_t)pi.dwProcessId;

	return (pty);

fail:
	while (ninherit != 0)
		CloseHandle(inherit[--ninherit]);
	if (pipeIn_read) CloseHandle(pipeIn_read);
	if (pipeIn_write) CloseHandle(pipeIn_write);
	if (pipeOut_read) CloseHandle(pipeOut_read);
	if (pipeOut_write) CloseHandle(pipeOut_write);
	if (pty->hPC) pty_api()->destroy(pty->hPC);
	if (pty->hProcess) CloseHandle(pty->hProcess);
	if (pty->hThread) CloseHandle(pty->hThread);
	if (pty->sock != INVALID_SOCKET) closesocket(pty->sock);
	if (pty->bridge_peer != INVALID_SOCKET) closesocket(pty->bridge_peer);
	free(pty);
	return (NULL);
}

int
win32_pty_resize(struct win32_pty *pty, int cols, int rows)
{
	COORD size;
	HRESULT hr;

	if (pty == NULL || pty->hPC == NULL)
		return (-1);

	size.X = (SHORT)cols;
	size.Y = (SHORT)rows;
	hr = pty_api()->resize(pty->hPC, size);
	return SUCCEEDED(hr) ? 0 : -1;
}

/*
 * Close the pseudo console once its process has exited. Until then the output
 * pipe stays open and the bridge never sees EOF; closing it flushes what is
 * left and ends the pipe, and the bridge closes the socket.
 */
void
win32_pty_hangup(struct win32_pty *pty)
{
	if (pty == NULL || pty->hPC == NULL)
		return;
	pty_api()->destroy(pty->hPC);
	pty->hPC = NULL;
}

void
win32_pty_close(struct win32_pty *pty)
{
	if (pty == NULL)
		return;

	pty->closing = 1;

	if (pty->hPC != NULL)
		pty_api()->destroy(pty->hPC);
	if (pty->hPipeIn != NULL)
		CloseHandle(pty->hPipeIn);
	if (pty->hPipeOut != NULL)
		CloseHandle(pty->hPipeOut);

	/* Wait for bridge threads. */
	if (pty->bridge_thread != NULL) {
		WaitForSingleObject(pty->bridge_thread, 2000);
		CloseHandle(pty->bridge_thread);
	}
	if (pty->input_thread != NULL) {
		WaitForSingleObject(pty->input_thread, 2000);
		CloseHandle(pty->input_thread);
	}

	if (pty->hProcess != NULL) {
		TerminateProcess(pty->hProcess, 1);
		CloseHandle(pty->hProcess);
	}
	if (pty->hThread != NULL)
		CloseHandle(pty->hThread);

	if (pty->sock != INVALID_SOCKET)
		closesocket(pty->sock);
	if (pty->bridge_peer != INVALID_SOCKET)
		closesocket(pty->bridge_peer);

	free(pty);
}

int
win32_pty_get_fd(struct win32_pty *pty)
{
	if (pty == NULL)
		return (-1);
	return (int)pty->sock;
}

HANDLE
win32_pty_get_process(struct win32_pty *pty)
{
	if (pty == NULL)
		return NULL;
	return pty->hProcess;
}

/*
 * Write data to the PTY input (from server to child process).
 */
int
win32_pty_write(struct win32_pty *pty, const void *data, size_t len)
{
	DWORD written;

	if (pty == NULL || pty->hPipeIn == NULL)
		return (-1);
	if (!WriteFile(pty->hPipeIn, data, (DWORD)len, &written, NULL))
		return (-1);
	return (int)written;
}

/* Start the bridge threads on a new socket pair. */
static int
pty_start_bridge(struct win32_pty *pty)
{
	int	sv[2];

	if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0)
		return (-1);
	pty->sock = (SOCKET)sv[0];
	pty->bridge_peer = (SOCKET)sv[1];
	pty->closing = 0;
	pty->bridge_thread = CreateThread(NULL, 0, pty_bridge_thread, pty, 0,
	    NULL);
	pty->input_thread = CreateThread(NULL, 0, pty_input_thread, pty, 0,
	    NULL);
	if (pty->bridge_thread == NULL || pty->input_thread == NULL)
		return (-1);
	return (0);
}

/*
 * Stop both bridge threads and leave the console as it is, so another process
 * can take it over. Input already queued reaches the console first. Output the
 * bridge already passed on, and any it was reading, is added to out; nothing
 * the console wrote is lost or read twice.
 */
void
win32_pty_stop(struct win32_pty *pty, struct evbuffer *out)
{
	char	buf[4096];
	int	n;
	u_long	nonblocking = 1;

	ioctlsocket(pty->sock, FIONBIO, &nonblocking);

	/* The input bridge reads what is left and stops at end of file. */
	shutdown(pty->sock, SD_SEND);
	if (pty->input_thread != NULL) {
		while (WaitForSingleObject(pty->input_thread, 5000) ==
		    WAIT_TIMEOUT)
			CancelSynchronousIo(pty->input_thread);
		CloseHandle(pty->input_thread);
		pty->input_thread = NULL;
	}

	/*
	 * The output bridge is cancelled out of its read; it may be blocked
	 * sending instead, so keep reading the socket until it has gone and
	 * closed its end.
	 */
	pty->closing = 1;
	while (pty->bridge_thread != NULL) {
		while ((n = recv(pty->sock, buf, sizeof buf, 0)) > 0)
			evbuffer_add(out, buf, n);
		if (WaitForSingleObject(pty->bridge_thread, 10) ==
		    WAIT_OBJECT_0) {
			CloseHandle(pty->bridge_thread);
			pty->bridge_thread = NULL;
		} else
			CancelSynchronousIo(pty->bridge_thread);
	}
	for (;;) {
		n = recv(pty->sock, buf, sizeof buf, 0);
		if (n > 0)
			evbuffer_add(out, buf, n);
		else if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK)
			break;
	}
	closesocket(pty->sock);
	pty->sock = INVALID_SOCKET;
}

/* Restart the bridges after win32_pty_stop. Returns the new socket. */
int
win32_pty_resume(struct win32_pty *pty)
{
	if (pty_start_bridge(pty) != 0)
		return (-1);
	return ((int)pty->sock);
}

/* Whether consoles can be handed to another process: conpty.dll's can. */
int
win32_pty_can_export(void)
{
	return (pty_api()->pack != NULL);
}

/*
 * Copy the console, its pipes and its process into another process. Only
 * conpty.dll's consoles can be packed again on the other side.
 */
int
win32_pty_export(struct win32_pty *pty, HANDLE child,
    struct win32_pty_handoff *h)
{
	struct pty_conpty	*pc = (struct pty_conpty *)pty->hPC;
	HANDLE			 src[6], dst[6];
	int			 i;

	if (pty_api()->pack == NULL || pc == NULL)
		return (-1);
	src[0] = pc->signal;
	src[1] = pc->reference;
	src[2] = pc->process;
	src[3] = pty->hPipeIn;
	src[4] = pty->hPipeOut;
	src[5] = pty->hProcess;
	for (i = 0; i < 6; i++) {
		if (!DuplicateHandle(GetCurrentProcess(), src[i], child,
		    &dst[i], 0, FALSE, DUPLICATE_SAME_ACCESS))
			return (-1);
	}
	h->signal = (uint64_t)(uintptr_t)dst[0];
	h->reference = (uint64_t)(uintptr_t)dst[1];
	h->conhost = (uint64_t)(uintptr_t)dst[2];
	h->in = (uint64_t)(uintptr_t)dst[3];
	h->out = (uint64_t)(uintptr_t)dst[4];
	h->process = (uint64_t)(uintptr_t)dst[5];
	h->pid = pty->dwProcessId;
	return (0);
}

/*
 * Take over a console handed on by win32_pty_export. Nothing reads it until
 * win32_pty_resume: the old server may still need it.
 */
struct win32_pty *
win32_pty_import(const struct win32_pty_handoff *h)
{
	struct win32_pty	*pty;

	if (pty_api()->pack == NULL)
		return (NULL);
	pty = calloc(1, sizeof *pty);
	if (pty == NULL)
		return (NULL);
	pty->sock = INVALID_SOCKET;
	pty->bridge_peer = INVALID_SOCKET;
	if (FAILED(pty_api()->pack((HANDLE)(uintptr_t)h->conhost,
	    (HANDLE)(uintptr_t)h->reference, (HANDLE)(uintptr_t)h->signal,
	    &pty->hPC))) {
		free(pty);
		return (NULL);
	}
	pty->hPipeIn = (HANDLE)(uintptr_t)h->in;
	pty->hPipeOut = (HANDLE)(uintptr_t)h->out;
	pty->hProcess = (HANDLE)(uintptr_t)h->process;
	pty->dwProcessId = h->pid;
	return (pty);
}
