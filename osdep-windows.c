/*
 * OS-dependent functions for Windows.
 * Implements osdep_get_name(), osdep_get_cwd(), osdep_event_init(), and the
 * pid-based win32_pane_command() that format.c uses here instead.
 */

#ifdef _WIN32

#include <sys/types.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#include <windows.h>
#include <tlhelp32.h>

/*
 * A pane's fd is a socket bridged from ConPTY and names no process; format.c
 * calls win32_pane_command() with the pane's pid instead.
 */
char *
osdep_get_name(int fd, char *tty)
{
	(void)fd;
	(void)tty;
	return (NULL);
}

static ULONGLONG
win32_process_start(DWORD pid)
{
	HANDLE		h;
	FILETIME	created, exited, kernel, user;
	ULONGLONG	t = 0;

	h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (h == NULL)
		return (0);
	if (GetProcessTimes(h, &created, &exited, &kernel, &user))
		t = ((ULONGLONG)created.dwHighDateTime << 32) | created.dwLowDateTime;
	CloseHandle(h);
	return (t);
}

/*
 * The name of what a pane is running. On Unix tmux names the terminal's
 * foreground process group; a job the shell starts leads its own group, and
 * that job's children stay in it. The equivalent here is the shell's newest
 * child, else the shell itself: Claude Code stays "claude" while it runs
 * commands of its own. conhost.exe, ConPTY's host, is not a job. A child must
 * have started after the shell, or its parent pid was reused.
 */
char *
win32_pane_command(pid_t pid)
{
	HANDLE		 snap;
	PROCESSENTRY32W	 pe;
	ULONGLONG	 shell_start, start, best_start = 0;
	WCHAR		 shell_name[MAX_PATH] = L"", best[MAX_PATH] = L"";
	char		 name[MAX_PATH * 3], *dot;

	if (pid <= 0)
		return (NULL);
	shell_start = win32_process_start((DWORD)pid);

	snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return (NULL);
	pe.dwSize = sizeof pe;
	for (BOOL ok = Process32FirstW(snap, &pe); ok;
	    ok = Process32NextW(snap, &pe)) {
		if (pe.th32ProcessID == (DWORD)pid) {
			wcscpy_s(shell_name, MAX_PATH, pe.szExeFile);
			continue;
		}
		if (pe.th32ParentProcessID != (DWORD)pid ||
		    _wcsicmp(pe.szExeFile, L"conhost.exe") == 0)
			continue;
		start = win32_process_start(pe.th32ProcessID);
		if (start < shell_start || start <= best_start)
			continue;
		best_start = start;
		wcscpy_s(best, MAX_PATH, pe.szExeFile);
	}
	CloseHandle(snap);

	if (*best == L'\0')
		wcscpy_s(best, MAX_PATH, shell_name);
	if (*best == L'\0')
		return (NULL);
	WideCharToMultiByte(CP_UTF8, 0, best, -1, name, sizeof name, NULL, NULL);
	if ((dot = strrchr(name, '.')) != NULL && _stricmp(dot, ".exe") == 0)
		*dot = '\0';
	return (xstrdup(name));
}

/*
 * A process's working directory sits in its PEB, and pwsh's Set-Location
 * doesn't change it anyway; format.c falls back to the folder the pane
 * reports itself (OSC 7).
 */
char *
osdep_get_cwd(int fd)
{
	(void)fd;
	return (NULL);
}

/*
 * Initialize the event system.
 */
struct event_base *
osdep_event_init(void)
{
	return (event_init());
}

/*
 * Are all the modifiers in a key still physically held? Terminals report
 * presses, not releases, so this asks Windows for the keyboard of the desktop
 * the server runs on.
 */
int
win32_modifiers_held(key_code modifiers)
{
	if ((modifiers & KEYC_CTRL) && !(GetAsyncKeyState(VK_CONTROL) & 0x8000))
		return (0);
	if ((modifiers & KEYC_META) && !(GetAsyncKeyState(VK_MENU) & 0x8000))
		return (0);
	if ((modifiers & KEYC_SHIFT) && !(GetAsyncKeyState(VK_SHIFT) & 0x8000))
		return (0);
	return (1);
}

/*
 * Keys pressed anywhere on the desktop with the Windows key held, for the
 * global key table. A low-level keyboard hook sees them before Windows hands
 * out hotkeys and passes every key on, so a key another program registered
 * (Windows Terminal's summon) still reaches it. The hook's thread writes each
 * key to a socket that the event loop reads.
 */
static SOCKET		global_keys[2] = { INVALID_SOCKET, INVALID_SOCKET };
static struct event	global_keys_event;
static DWORD		global_keys_down; /* held key, to skip auto-repeat */

/* The key a virtual key types, unshifted, with the modifiers held. */
static key_code
win32_global_key(DWORD vk)
{
	UINT		ch;
	key_code	key;

	ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;
	if (ch <= ' ' || ch > '~')
		return (KEYC_NONE);
	key = tolower(ch) | KEYC_SUPER;
	if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
		key |= KEYC_CTRL;
	if (GetAsyncKeyState(VK_MENU) & 0x8000)
		key |= KEYC_META;
	if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
		key |= KEYC_SHIFT;
	return (key);
}

static LRESULT CALLBACK
win32_global_keys_hook(int code, WPARAM wp, LPARAM lp)
{
	KBDLLHOOKSTRUCT	*k = (KBDLLHOOKSTRUCT *)lp;
	key_code	 key;

	if (code != HC_ACTION)
		return (CallNextHookEx(NULL, code, wp, lp));
	if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
		if (k->vkCode == global_keys_down)
			global_keys_down = 0;
	} else if (k->vkCode != global_keys_down &&
	    ((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000)) {
		key = win32_global_key(k->vkCode);
		if (key != KEYC_NONE) {
			global_keys_down = k->vkCode;
			send(global_keys[1], (char *)&key, sizeof key, 0);
		}
	}
	return (CallNextHookEx(NULL, code, wp, lp));
}

static DWORD WINAPI
win32_global_keys_thread(LPVOID arg)
{
	MSG	msg;

	(void)arg;
	if (SetWindowsHookExW(WH_KEYBOARD_LL, win32_global_keys_hook,
	    GetModuleHandleW(NULL), 0) == NULL)
		return (0);
	while (GetMessageW(&msg, NULL, 0, 0) > 0)
		/* nothing */;
	return (0);
}

static void
win32_global_keys_read(evutil_socket_t fd, __unused short events,
    __unused void *arg)
{
	key_code	key;

	if (recv((SOCKET)fd, (char *)&key, sizeof key, MSG_WAITALL) ==
	    sizeof key)
		key_bindings_global(key);
}

/* Start the hook, once, when the global table first gets a key. */
void
win32_global_keys_start(void)
{
	int	sv[2];
	HANDLE	thread;

	if (global_keys[0] != INVALID_SOCKET)
		return;
	if (win32_socketpair(AF_INET, SOCK_STREAM, 0, sv) != 0)
		return;
	global_keys[0] = (SOCKET)sv[0];
	global_keys[1] = (SOCKET)sv[1];
	event_set(&global_keys_event, sv[0], EV_READ|EV_PERSIST,
	    win32_global_keys_read, NULL);
	event_add(&global_keys_event, NULL);
	thread = CreateThread(NULL, 0, win32_global_keys_thread, NULL, 0, NULL);
	if (thread != NULL)
		CloseHandle(thread);
}

#endif /* _WIN32 */
