/* $OpenBSD$ */

/*
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "tmux.h"

/*
 * The session set: a server whose global option @session-set-dir names a
 * folder keeps live/<socket>.set there, everything needed to rebuild its
 * sessions, windows and panes after a reboot or a crash, written by the server
 * itself whenever something it holds changes (session_set_changed, from every
 * command, notification and reported folder, written only when the result
 * differs from the last one). What each pane runs is looked up apart from that,
 * a second after a pane prints (ss_programs). No process is started for it.
 *
 * A snapshot is lines of tab-separated fields (ss_field escapes them): a
 * version line, then start, pid, seen, closed, attached and socket, then one
 * line per window: "window", whether it is its session's current window, its
 * size, and the line that brings the window back. That last line is kept on
 * the window too: when a window is destroyed (killed, or its last pane exited,
 * but not moved) it goes on top of closed-windows in the same folder, and
 * reopen-window takes it off again. Nothing is recorded while the server
 * itself shuts down.
 *
 * The snapshot an earlier server of the same name left is the previous set:
 * the first write sets it aside as live/<socket>-previous.set, and the first
 * client to attach is offered it in a menu. Enter rebuilds it in this server
 * in place of the fresh session the client opened in; Escape archives it.
 * restore-set rebuilds any snapshot the same way, beside the server's own
 * sessions. archive/ keeps the last SESSION_SET_ARCHIVE_MAX.
 *
 * A pane's Claude Code session comes from the pane options @claude-session and
 * @claude-cwd, which Claude Code's own hooks set. @session-set-resume is the
 * command that resumes one ("{id}" for its ID); @session-set-restart-<program>
 * the command that starts a program again. Without either a pane comes back as
 * a shell in its folder.
 */

#define SESSION_SET_CLOSED_MAX 25
#define SESSION_SET_ARCHIVE_MAX 50
#define SESSION_SET_VERSION "tmux-session-set 1"

static struct event	 ss_timer;
static int		 ss_pending;
static int		 ss_written;
static struct event	 ss_programs_timer;
static int		 ss_programs_pending;
static int		 ss_stopped;
static int		 ss_set_aside;
static char		*ss_attached;
static char		*ss_last_sessions;

static const char *
ss_dir(void)
{
	const char	*dir;

	if (options_get_only(global_s_options, "@session-set-dir") == NULL)
		return (NULL);
	dir = options_get_string(global_s_options, "@session-set-dir");
	return (*dir == '\0' ? NULL : dir);
}

static const char *
ss_socket_name(void)
{
	const char	*p, *q;

	p = socket_path;
	if ((q = strrchr(p, '/')) != NULL)
		p = q + 1;
	if ((q = strrchr(p, '\\')) != NULL)
		p = q + 1;
	return (p);
}

static void
ss_mkdir(const char *path)
{
#ifdef _WIN32
	_mkdir(path);
#else
	mkdir(path, S_IRWXU);
#endif
}

/* Replace a file with another, as one step. */
static int
ss_replace(const char *from, const char *to)
{
#ifdef _WIN32
	return (MoveFileExA(from, to, MOVEFILE_REPLACE_EXISTING) ? 0 : -1);
#else
	return (rename(from, to));
#endif
}

static int
ss_write_file(const char *path, const char *data, size_t size)
{
	char	*tmp;
	FILE	*f;
	int	 ok;

	xasprintf(&tmp, "%s.tmp", path);
	if ((f = fopen(tmp, "wb")) == NULL) {
		free(tmp);
		return (-1);
	}
	ok = (fwrite(data, 1, size, f) == size);
	if (fclose(f) != 0)
		ok = 0;
	if (ok && ss_replace(tmp, path) != 0)
		ok = 0;
	if (!ok)
		unlink(tmp);
	free(tmp);
	return (ok ? 0 : -1);
}

static char *
ss_read_file(const char *path)
{
	FILE	*f;
	char	*data = NULL;
	size_t	 size = 0, n;
	char	 buf[8192];

	if ((f = fopen(path, "rb")) == NULL)
		return (NULL);
	while ((n = fread(buf, 1, sizeof buf, f)) != 0) {
		data = xrealloc(data, size + n + 1);
		memcpy(data + size, buf, n);
		size += n;
	}
	fclose(f);
	if (data == NULL)
		data = xstrdup("");
	else
		data[size] = '\0';
	return (data);
}

/*
 * A field of a snapshot or closed-window line: tabs, newlines and backslashes
 * escaped.
 */
static void
ss_field(struct evbuffer *evb, const char *s)
{
	const char	*p;

	evbuffer_add(evb, "\t", 1);
	for (p = (s != NULL ? s : ""); *p != '\0'; p++) {
		switch (*p) {
		case '\\':
			evbuffer_add(evb, "\\\\", 2);
			break;
		case '\t':
			evbuffer_add(evb, "\\t", 2);
			break;
		case '\n':
			evbuffer_add(evb, "\\n", 2);
			break;
		case '\r':
			evbuffer_add(evb, "\\r", 2);
			break;
		default:
			evbuffer_add(evb, p, 1);
			break;
		}
	}
}

static const char *
ss_pane_option(struct window_pane *wp, const char *name)
{
	struct options_entry	*o;
	const char		*s;

	if ((o = options_get_only(wp->options, name)) == NULL)
		return (NULL);
	s = options_get_string(wp->options, name);
	return (*s == '\0' ? NULL : s);
}

static char *
ss_format(const char *fmt, struct session *s, struct winlink *wl,
    struct window_pane *wp)
{
	struct format_tree	*ft;
	char			*value;

	ft = format_create(NULL, NULL, FORMAT_NONE, FORMAT_NOJOBS);
	format_defaults(ft, NULL, s, wl, wp);
	value = format_expand(ft, fmt);
	format_free(ft);
	return (value);
}

/* A program's name from its path: the file name without ".exe". */
static char *
ss_program_name(const char *path)
{
	const char	*p, *q;
	char		*name, *dot;

	p = path;
	if ((q = strrchr(p, '\\')) != NULL)
		p = q + 1;
	if ((q = strrchr(p, '/')) != NULL)
		p = q + 1;
	name = xstrdup(p);
	if ((dot = strrchr(name, '.')) != NULL && strcasecmp(dot, ".exe") == 0)
		*dot = '\0';
	return (name);
}

/*
 * What a pane runs: the program its last look at the process table found
 * (ss_programs), or before that its shell's.
 */
static char *
ss_pane_command(struct window_pane *wp)
{
	if (wp->session_set_command != NULL)
		return (xstrdup(wp->session_set_command));
	if (wp->shell == NULL)
		return (NULL);
	return (ss_program_name(wp->shell));
}

/*
 * Look up what every pane runs, in one look at the process table, and write a
 * snapshot if any pane's program changed. Runs a second after a pane prints,
 * at most once a second, so a snapshot itself never reads the process table.
 */
static void
ss_programs(__unused int fd, __unused short events, __unused void *data)
{
	struct window		 *w;
	struct window_pane	 *wp, **panes = NULL;
	char			**names;
	u_int			  n = 0, i;
	int			  changed = 0;
	uint64_t		  started = get_timer();
#ifdef _WIN32
	pid_t			 *pids;
#endif

	ss_programs_pending = 0;
	if (ss_stopped || ss_dir() == NULL)
		return;
	RB_FOREACH(w, windows, &windows) {
		TAILQ_FOREACH(wp, &w->panes, entry) {
			panes = xreallocarray(panes, n + 1, sizeof *panes);
			panes[n++] = wp;
		}
	}
	names = xcalloc(n + 1, sizeof *names);
#ifdef _WIN32
	pids = xcalloc(n + 1, sizeof *pids);
	for (i = 0; i < n; i++)
		pids[i] = panes[i]->pid;
	win32_pane_commands(pids, n, names);
	free(pids);
#else
	for (i = 0; i < n; i++) {
		names[i] = ss_format("#{pane_current_command}", NULL, NULL,
		    panes[i]);
	}
#endif
	for (i = 0; i < n; i++) {
		wp = panes[i];
		if (names[i] == NULL || *names[i] == '\0' ||
		    (wp->session_set_command != NULL &&
		    strcmp(names[i], wp->session_set_command) == 0)) {
			free(names[i]);
			continue;
		}
		free(wp->session_set_command);
		wp->session_set_command = names[i];
		changed = 1;
	}
	free(names);
	free(panes);
	log_debug("session-set: programs read in %llu ms%s",
	    (unsigned long long)(get_timer() - started),
	    changed ? ", changed" : "");
	if (changed)
		session_set_changed();
}

/* A pane printed something: its program may have changed. */
void
session_set_output(void)
{
	struct timeval	tv = { 1, 0 };

	if (ss_stopped || ss_programs_pending || ss_dir() == NULL)
		return;
	if (!event_initialized(&ss_programs_timer))
		evtimer_set(&ss_programs_timer, ss_programs, NULL);
	ss_programs_pending = 1;
	evtimer_add(&ss_programs_timer, &tv);
}

/* A pane's folder, with forward slashes so it quotes plainly for reopening. */
static char *
ss_pane_cwd(struct session *s, struct winlink *wl, struct window_pane *wp)
{
	const char	*claude_cwd;
	char		*cwd, *p;

	if (ss_pane_option(wp, "@claude-session") != NULL &&
	    (claude_cwd = ss_pane_option(wp, "@claude-cwd")) != NULL)
		cwd = xstrdup(claude_cwd);
	else
		cwd = ss_format("#{pane_current_path}", s, wl, wp);
	for (p = cwd; *p != '\0'; p++) {
		if (*p == '\\')
			*p = '/';
	}
	return (cwd);
}

/*
 * One window's line in the snapshot, and the line that reopens it, kept on the
 * window for closed-windows. A window is not offered for reopening when every
 * pane runs the at menu (it closes whenever the menu does); a restore's
 * stand-in session is left out of both.
 */
static void
ss_window(struct evbuffer *evb, struct session *s, struct winlink *wl,
    u_int position)
{
	struct window		*w = wl->window;
	struct window_pane	*wp;
	struct evbuffer		*line;
	char			*layout, *cwd, *command;
	u_int			 i, active = 0;
	int			 autoname, only_menu = 1;

	free(w->session_set_line);
	w->session_set_line = NULL;
	if (strncmp(s->name, "restoring-", 10) == 0)
		return;

	autoname = options_get_number(w->options, "automatic-rename");
	layout = layout_dump(w->layout_root);

	line = evbuffer_new();
	if (line == NULL)
		fatalx("out of memory");
	ss_field(line, s->name);
	ss_field(line, s->cwd);
	evbuffer_add_printf(line, "\t%u\t%d", position, autoname);
	ss_field(line, w->name);
	ss_field(line, layout);
	i = 0;
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp == w->active)
			active = i;
		i++;
	}
	evbuffer_add_printf(line, "\t%u", active);
	TAILQ_FOREACH(wp, &w->panes, entry) {
		cwd = ss_pane_cwd(s, wl, wp);
		command = ss_pane_command(wp);
		if (command == NULL || strcmp(command, "fzt-automate") != 0)
			only_menu = 0;
		ss_field(line, cwd);
		ss_field(line, ss_pane_option(wp, "@claude-session"));
		ss_field(line, command);
		free(command);
		free(cwd);
	}

	evbuffer_add_printf(evb, "window\t%d\t%u\t%u", wl == s->curw, w->sx,
	    w->sy);
	evbuffer_add(evb, EVBUFFER_DATA(line), EVBUFFER_LENGTH(line));
	evbuffer_add(evb, "\n", 1);

	if (!only_menu) {
		w->session_set_line = xmalloc(EVBUFFER_LENGTH(line) + 1);
		memcpy(w->session_set_line, EVBUFFER_DATA(line),
		    EVBUFFER_LENGTH(line));
		w->session_set_line[EVBUFFER_LENGTH(line)] = '\0';
	}
	evbuffer_free(line);
	free(layout);
}

/* The window lines of every session. */
static char *
ss_sessions(void)
{
	struct evbuffer		*evb;
	struct session		*s;
	struct winlink		*wl;
	u_int			 position;
	char			*out;

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	RB_FOREACH(s, sessions, &sessions) {
		position = 0;
		RB_FOREACH(wl, winlinks, &s->windows)
			ss_window(evb, s, wl, ++position);
	}

	out = xmalloc(EVBUFFER_LENGTH(evb) + 1);
	memcpy(out, EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
	out[EVBUFFER_LENGTH(evb)] = '\0';
	evbuffer_free(evb);
	return (out);
}

static char *
ss_snapshot_path(const char *dir, const char *suffix)
{
	char	*path, *live;

	xasprintf(&live, "%s/live", dir);
	ss_mkdir(dir);
	ss_mkdir(live);
	xasprintf(&path, "%s/%s%s.set", live, ss_socket_name(), suffix);
	free(live);
	return (path);
}

/* A snapshot read back: its header and its window lines, split. */
struct ss_line {
	char	**f;
	u_int	  n;
};
struct ss_snap {
	long long	 start;
	long long	 seen;
	long long	 closed;
	char		*attached;
	char		*socket;
	struct ss_line	*windows;
	u_int		 nwindows;
};

static void
ss_free_fields(char **f, u_int n)
{
	u_int	i;

	for (i = 0; i < n; i++)
		free(f[i]);
	free(f);
}

static void
ss_snap_free(struct ss_snap *snap)
{
	u_int	i;

	if (snap == NULL)
		return;
	for (i = 0; i < snap->nwindows; i++)
		ss_free_fields(snap->windows[i].f, snap->windows[i].n);
	free(snap->windows);
	free(snap->attached);
	free(snap->socket);
	free(snap);
}

static char	**ss_split(const char *, u_int *);

/*
 * Read a snapshot: NULL if the file is missing or is not one. A window line
 * holds "window", current, width and height, then the reopen line's fields:
 * session, its folder, position, automatic-rename, name, layout, active pane,
 * and folder, Claude session and program for each pane.
 */
static struct ss_snap *
ss_snap_read(const char *path)
{
	struct ss_snap	*snap;
	char		*data, *line, *next, *end, **f;
	u_int		 n;
	int		 first = 1, ok = 1;

	if ((data = ss_read_file(path)) == NULL)
		return (NULL);
	snap = xcalloc(1, sizeof *snap);
	for (line = data; ok && line != NULL; line = next) {
		if ((next = strchr(line, '\n')) != NULL)
			*next++ = '\0';
		end = line + strlen(line);
		if (end != line && end[-1] == '\r')
			end[-1] = '\0';
		if (first) {
			ok = (strcmp(line, SESSION_SET_VERSION) == 0);
			first = 0;
			continue;
		}
		if (*line == '\0')
			continue;
		f = ss_split(line, &n);
		if (strcmp(f[0], "window") == 0) {
			if (n < 14 || (n - 11) % 3 != 0) {
				ok = 0;
				ss_free_fields(f, n);
				continue;
			}
			snap->windows = xreallocarray(snap->windows,
			    snap->nwindows + 1, sizeof *snap->windows);
			snap->windows[snap->nwindows].f = f;
			snap->windows[snap->nwindows++].n = n;
			continue;
		}
		if (n == 2) {
			if (strcmp(f[0], "start") == 0)
				snap->start = strtoll(f[1], NULL, 10);
			else if (strcmp(f[0], "seen") == 0)
				snap->seen = strtoll(f[1], NULL, 10);
			else if (strcmp(f[0], "closed") == 0)
				snap->closed = strtoll(f[1], NULL, 10);
			else if (strcmp(f[0], "attached") == 0) {
				free(snap->attached);
				snap->attached = xstrdup(f[1]);
			} else if (strcmp(f[0], "socket") == 0) {
				free(snap->socket);
				snap->socket = xstrdup(f[1]);
			}
		}
		ss_free_fields(f, n);
	}
	free(data);
	if (first || !ok || snap->start == 0) {
		ss_snap_free(snap);
		return (NULL);
	}
	if (snap->attached == NULL)
		snap->attached = xstrdup("");
	if (snap->socket == NULL)
		snap->socket = xstrdup("");
	return (snap);
}

struct ss_entry {
	char	*path;
	time_t	 mtime;
};

static int
ss_entry_cmp(const void *a, const void *b)
{
	const struct ss_entry	*ea = a, *eb = b;

	if (ea->mtime != eb->mtime)
		return (ea->mtime > eb->mtime ? -1 : 1);
	return (strcmp(eb->path, ea->path));
}

/* Keep the newest SESSION_SET_ARCHIVE_MAX snapshots in archive/. */
static void
ss_prune(const char *archive)
{
	struct ss_entry	*entries = NULL;
	struct stat	 sb;
	u_int		 n = 0, i;
	char		*path;
	const char	*name;
#ifdef _WIN32
	WIN32_FIND_DATAA fd;
	HANDLE		 h;
	char		*pattern;

	xasprintf(&pattern, "%s/*.set", archive);
	h = FindFirstFileA(pattern, &fd);
	free(pattern);
	if (h == INVALID_HANDLE_VALUE)
		return;
	do {
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			continue;
		name = fd.cFileName;
#else
	DIR		*d;
	struct dirent	*de;
	size_t		 len;

	if ((d = opendir(archive)) == NULL)
		return;
	while ((de = readdir(d)) != NULL) {
		name = de->d_name;
		len = strlen(name);
		if (len < 4 || strcmp(name + len - 4, ".set") != 0)
			continue;
#endif
		xasprintf(&path, "%s/%s", archive, name);
		if (stat(path, &sb) != 0) {
			free(path);
			continue;
		}
		entries = xreallocarray(entries, n + 1, sizeof *entries);
		entries[n].path = path;
		entries[n++].mtime = sb.st_mtime;
#ifdef _WIN32
	} while (FindNextFileA(h, &fd));
	FindClose(h);
#else
	}
	closedir(d);
#endif
	if (n != 0)
		qsort(entries, n, sizeof *entries, ss_entry_cmp);
	for (i = 0; i < n; i++) {
		if (i >= SESSION_SET_ARCHIVE_MAX)
			unlink(entries[i].path);
		free(entries[i].path);
	}
	free(entries);
}

/* Move a snapshot to archive/, named by its server's socket and start time. */
static void
ss_archive(const char *dir, const char *path, struct ss_snap *snap)
{
	char		*archive, *to;
	const char	*name, *p;
	long long	 start;

	name = ss_socket_name();
	if (snap != NULL && *snap->socket != '\0') {
		name = snap->socket;
		if ((p = strrchr(name, '/')) != NULL)
			name = p + 1;
		if ((p = strrchr(name, '\\')) != NULL)
			name = p + 1;
	}
	start = (snap != NULL ? snap->start : (long long)time(NULL));

	xasprintf(&archive, "%s/archive", dir);
	ss_mkdir(archive);
	xasprintf(&to, "%s/%s-%lld.set", archive, name, start);
	if (strcmp(path, to) != 0 && ss_replace(path, to) != 0)
		log_debug("session-set: can't archive %s", path);
	free(to);
	ss_prune(archive);
	free(archive);
}

/*
 * Before the first write: the snapshot an earlier server of this name left
 * (after a reboot, a crash or kill-server) becomes the previous set,
 * <socket>-previous.set, archiving the one there. One that ended with its last
 * session, or that no client ever showed (a server started at logon that went
 * before the terminal opened), is archived instead, so the set before it stays
 * offered. An upgraded server keeps its start time, so its own snapshot stays
 * and the session its client showed carries over.
 */
static void
ss_take_earlier(const char *dir)
{
	struct ss_snap	*snap, *old;
	char		*path, *previous, *data;

	ss_set_aside = 1;
	path = ss_snapshot_path(dir, "");
	if ((snap = ss_snap_read(path)) == NULL) {
		if ((data = ss_read_file(path)) != NULL) {
			free(data);
			ss_archive(dir, path, NULL);
		}
	} else if (snap->start == (long long)start_time.tv_sec) {
		if (ss_attached == NULL && *snap->attached != '\0')
			ss_attached = xstrdup(snap->attached);
	} else if (snap->closed != 0 || *snap->attached == '\0')
		ss_archive(dir, path, snap);
	else {
		previous = ss_snapshot_path(dir, "-previous");
		if ((data = ss_read_file(previous)) != NULL) {
			free(data);
			old = ss_snap_read(previous);
			ss_archive(dir, previous, old);
			ss_snap_free(old);
		}
		if (ss_replace(path, previous) != 0)
			log_debug("session-set: can't set aside %s", path);
		free(previous);
	}
	ss_snap_free(snap);
	free(path);
}

static int
ss_write(const char *dir, const char *sessions, long long closed)
{
	struct evbuffer	*evb;
	char		*path;
	int		 retval;

	if (!ss_set_aside)
		ss_take_earlier(dir);
	path = ss_snapshot_path(dir, "");

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	evbuffer_add_printf(evb, SESSION_SET_VERSION "\nstart\t%lld\npid\t%ld"
	    "\nseen\t%lld\nclosed\t%lld\nattached",
	    (long long)start_time.tv_sec, (long)getpid(), (long long)time(NULL),
	    closed);
	ss_field(evb, ss_attached);
	evbuffer_add_printf(evb, "\nsocket");
	ss_field(evb, socket_path);
	evbuffer_add_printf(evb, "\n%s", sessions);

	retval = ss_write_file(path, EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
	if (retval != 0) {
		log_debug("session-set: can't write %s: %s", path,
		    strerror(errno));
	}
	evbuffer_free(evb);
	free(path);
	return (retval);
}

static void	ss_flush(int, short, void *);

static void
ss_schedule(time_t seconds)
{
	struct timeval	tv = { seconds, 0 };

	if (!event_initialized(&ss_timer))
		evtimer_set(&ss_timer, ss_flush, NULL);
	ss_pending = 1;
	evtimer_add(&ss_timer, &tv);
}

/*
 * Write the snapshot if anything in it has changed since the last one written.
 * A write that fails (a reader holding the file) is tried again a second later.
 */
static void
ss_flush(__unused int fd, __unused short events, __unused void *data)
{
	const char	*dir;
	struct client	*c;
	char		*current;
	uint64_t	 started = get_timer();

	ss_pending = 0;
	if (ss_stopped || (dir = ss_dir()) == NULL || RB_EMPTY(&sessions))
		return;

	/* The session the window shows; a closing window's is kept. */
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || (c->flags & CLIENT_CONTROL))
			continue;
		if (ss_attached == NULL ||
		    strcmp(ss_attached, c->session->name) != 0) {
			free(ss_attached);
			ss_attached = xstrdup(c->session->name);
			ss_written = 0;
		}
		break;
	}

	current = ss_sessions();
	if (ss_last_sessions == NULL ||
	    strcmp(current, ss_last_sessions) != 0) {
		free(ss_last_sessions);
		ss_last_sessions = current;
		ss_written = 0;
	} else
		free(current);
	if (ss_written)
		return;

	if (ss_write(dir, ss_last_sessions, 0) != 0) {
		ss_schedule(1);
		return;
	}
	ss_written = 1;
	log_debug("session-set: snapshot written in %llu ms",
	    (unsigned long long)(get_timer() - started));
}

/* Something a snapshot holds may have changed: write one soon. */
void
session_set_changed(void)
{
	if (ss_stopped || ss_pending || ss_dir() == NULL)
		return;
	ss_schedule(0);
}

/* The server is shutting itself down: its windows are not being closed. */
void
session_set_stop(void)
{
	ss_stopped = 1;
}

/*
 * The last session has gone: the server ends with it, so its snapshot is
 * marked closed and never offered for restore.
 */
void
session_set_closed(void)
{
	const char	*dir;

	if (ss_stopped || (dir = ss_dir()) == NULL || ss_last_sessions == NULL)
		return;
	ss_write(dir, ss_last_sessions, (long long)time(NULL));
	ss_stopped = 1;
}

static char *
ss_closed_path(const char *dir)
{
	char	*path;

	ss_mkdir(dir);
	xasprintf(&path, "%s/closed-windows", dir);
	return (path);
}

/* A window is being destroyed: put it on top of closed-windows. */
void
session_set_window_destroyed(struct window *w)
{
	const char	*dir;
	char		*path, *old, *p, *data;
	u_int		 kept = 0;

	if (w->session_set_line == NULL)
		return;
	if (!ss_stopped && (dir = ss_dir()) != NULL) {
		path = ss_closed_path(dir);
		if ((old = ss_read_file(path)) == NULL)
			old = xstrdup("");
		for (p = old; *p != '\0' && kept < SESSION_SET_CLOSED_MAX - 1;
		    p++) {
			if (*p == '\n')
				kept++;
		}
		if (kept == SESSION_SET_CLOSED_MAX - 1)
			*p = '\0';
		xasprintf(&data, "%lld%s\n%s", (long long)time(NULL),
		    w->session_set_line, old);
		if (ss_write_file(path, data, strlen(data)) != 0)
			log_debug("session-set: can't write %s", path);
		free(data);
		free(old);
		free(path);
	}
	free(w->session_set_line);
	w->session_set_line = NULL;
}

/* Undo ss_field. */
static char *
ss_unfield(const char *s, size_t len)
{
	char	*out, *o;
	size_t	 i;

	out = o = xmalloc(len + 1);
	for (i = 0; i < len; i++) {
		if (s[i] == '\\' && i + 1 < len) {
			i++;
			switch (s[i]) {
			case 't':
				*o++ = '\t';
				break;
			case 'n':
				*o++ = '\n';
				break;
			case 'r':
				*o++ = '\r';
				break;
			default:
				*o++ = s[i];
				break;
			}
		} else
			*o++ = s[i];
	}
	*o = '\0';
	return (out);
}

static char **
ss_split(const char *line, u_int *count)
{
	char		**fields = NULL;
	const char	 *start = line, *p;
	u_int		  n = 0;

	for (p = line;; p++) {
		if (*p == '\t' || *p == '\0') {
			fields = xreallocarray(fields, n + 1, sizeof *fields);
			fields[n++] = ss_unfield(start, p - start);
			if (*p == '\0')
				break;
			start = p + 1;
		}
	}
	*count = n;
	return (fields);
}

static void
ss_add_command(struct evbuffer *evb, const char **argv, u_int argc)
{
	char	*escaped;
	u_int	 i;

	if (EVBUFFER_LENGTH(evb) != 0)
		evbuffer_add(evb, " ; ", 3);
	for (i = 0; i < argc; i++) {
		if (argv[i] == NULL)
			continue;
		escaped = args_escape(argv[i]);
		evbuffer_add_printf(evb, "%s%s", i == 0 ? "" : " ", escaped);
		free(escaped);
	}
}

/* The command that brings a pane's program back, or NULL for a shell. */
static char *
ss_start_command(const char *claude, const char *command)
{
	struct options_entry	*o;
	const char		*template, *at;
	char			*name, *out;

	if (claude != NULL && *claude != '\0') {
		if (options_get_only(global_s_options, "@session-set-resume") ==
		    NULL)
			return (NULL);
		template = options_get_string(global_s_options,
		    "@session-set-resume");
		if ((at = strstr(template, "{id}")) == NULL)
			return (xstrdup(template));
		xasprintf(&out, "%.*s%s%s", (int)(at - template), template,
		    claude, at + 4);
		return (out);
	}
	if (command == NULL || *command == '\0')
		return (NULL);
	xasprintf(&name, "@session-set-restart-%s", command);
	o = options_get_only(global_s_options, name);
	out = (o == NULL ? NULL :
	    xstrdup(options_get_string(global_s_options, name)));
	free(name);
	if (out != NULL && *out == '\0') {
		free(out);
		out = NULL;
	}
	return (out);
}

/*
 * Take the newest closed window off closed-windows and return the commands that
 * rebuild it: in its session (made again if it has gone) at its place, with
 * its splits, layout, folders, name and active pane, and shown.
 */
char *
session_set_reopen(int show, char **cause)
{
	const char	*dir, *argv[12];
	char		*path, *data, *nl, **f, *target, *start, *idx;
	char		*placeholder = NULL, *out;
	struct session	*s;
	struct winlink	*wl, *before = NULL, *first_wl = NULL;
	struct evbuffer	*evb;
	u_int		 n, i, position, panes, at;
	int		 autoname;

	if ((dir = ss_dir()) == NULL) {
		*cause = xstrdup("@session-set-dir is not set");
		return (NULL);
	}
	path = ss_closed_path(dir);
	data = ss_read_file(path);
	if (data == NULL || *data == '\0') {
		free(data);
		free(path);
		*cause = xstrdup("no closed window to reopen");
		return (NULL);
	}
	if ((nl = strchr(data, '\n')) != NULL)
		*nl++ = '\0';
	if (ss_write_file(path, nl != NULL ? nl : "",
	    nl != NULL ? strlen(nl) : 0) != 0) {
		free(data);
		free(path);
		*cause = xstrdup("can't update closed-windows");
		return (NULL);
	}
	free(path);

	/* closedAt session path position autoname name layout active panes... */
	f = ss_split(data, &n);
	free(data);
	if (n < 11 || (n - 8) % 3 != 0) {
		for (i = 0; i < n; i++)
			free(f[i]);
		free(f);
		*cause = xstrdup("a closed-windows line is malformed");
		return (NULL);
	}
	position = strtoul(f[3], NULL, 10);
	autoname = atoi(f[4]);
	at = strtoul(f[7], NULL, 10);
	panes = (n - 8) / 3;

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	xasprintf(&target, "=%s:", f[1]);

	if ((s = session_find(f[1])) == NULL) {
		argv[0] = "new-session"; argv[1] = "-d"; argv[2] = "-s";
		argv[3] = f[1]; argv[4] = "-c"; argv[5] = f[2];
		ss_add_command(evb, argv, 6);
		xasprintf(&placeholder, "=%s:%lld", f[1],
		    options_get_number(global_s_options, "base-index"));
	} else {
		i = 0;
		RB_FOREACH(wl, winlinks, &s->windows) {
			if (first_wl == NULL)
				first_wl = wl;
			if (++i == position - 1)
				before = wl;
		}
	}

	argv[0] = "new-window";
	i = 1;
	if (position <= 1 && first_wl != NULL) {
		argv[i++] = "-b";
		xasprintf(&idx, "=%s:%d", f[1], first_wl->idx);
		argv[i++] = "-t";
		argv[i++] = idx;
	} else if (before != NULL) {
		argv[i++] = "-a";
		xasprintf(&idx, "=%s:%d", f[1], before->idx);
		argv[i++] = "-t";
		argv[i++] = idx;
	} else {
		idx = NULL;
		argv[i++] = "-t";
		argv[i++] = target;
	}
	argv[i++] = "-c";
	argv[i++] = f[8];
	start = ss_start_command(f[9], f[10]);
	argv[i++] = start;
	ss_add_command(evb, argv, i);
	free(start);
	free(idx);

	for (u_int p = 1; p < panes; p++) {
		start = ss_start_command(f[9 + p * 3], f[10 + p * 3]);
		argv[0] = "split-window"; argv[1] = "-t"; argv[2] = target;
		argv[3] = "-c"; argv[4] = f[8 + p * 3]; argv[5] = start;
		ss_add_command(evb, argv, 6);
		free(start);
	}
	argv[0] = "select-layout"; argv[1] = "-t"; argv[2] = target;
	argv[3] = f[6];
	ss_add_command(evb, argv, 4);
	if (!autoname) {
		argv[0] = "rename-window"; argv[1] = "-t"; argv[2] = target;
		argv[3] = f[5];
		ss_add_command(evb, argv, 4);
	}
	xasprintf(&idx, "=%s:.%lld", f[1], (long long)at +
	    options_get_number(global_w_options, "pane-base-index"));
	argv[0] = "select-pane"; argv[1] = "-t"; argv[2] = idx;
	ss_add_command(evb, argv, 3);
	free(idx);
	if (placeholder != NULL) {
		argv[0] = "kill-window"; argv[1] = "-t"; argv[2] = placeholder;
		ss_add_command(evb, argv, 3);
		free(placeholder);
	}
	if (show) {
		xasprintf(&idx, "=%s", f[1]);
		argv[0] = "switch-client"; argv[1] = "-t"; argv[2] = idx;
		ss_add_command(evb, argv, 3);
		free(idx);
	}
	free(target);

	out = xmalloc(EVBUFFER_LENGTH(evb) + 1);
	memcpy(out, EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
	out[EVBUFFER_LENGTH(evb)] = '\0';
	evbuffer_free(evb);

	for (i = 0; i < n; i++)
		free(f[i]);
	free(f);
	return (out);
}

/* Whether a session name is in use, or made by this restore already. */
static int
ss_name_taken(const char *name, struct session *replace, char **made,
    u_int nmade)
{
	struct session	*s;
	u_int		 i;

	if ((s = session_find(name)) != NULL && s != replace)
		return (1);
	for (i = 0; i < nmade; i++) {
		if (strcmp(made[i], name) == 0)
			return (1);
	}
	return (0);
}

/* Whether a session is one window with one pane running its shell. */
static int
ss_lone_shell(struct session *s)
{
	struct winlink		*wl;
	struct window_pane	*wp;
	char			*command, *shell;
	int			 lone;

	if (s == NULL || winlink_count(&s->windows) != 1)
		return (0);
	wl = RB_MIN(winlinks, &s->windows);
	if (window_count_panes(wl->window) != 1)
		return (0);
	wp = TAILQ_FIRST(&wl->window->panes);
	if (wp->shell == NULL || (command = ss_pane_command(wp)) == NULL)
		return (0);
	shell = ss_program_name(wp->shell);
	lone = (strcasecmp(command, shell) == 0);
	free(shell);
	free(command);
	return (lone);
}

/* The commands that build one saved session's windows as session name. */
static void
ss_restore_session(struct evbuffer *evb, struct ss_snap *snap,
    const char *from, const char *name)
{
	const char	*argv[12];
	struct ss_line	*w;
	char		*target, *idx, *start;
	u_int		 i, p, panes, k = 0, active = 0, first = 1;
	long long	 base;

	base = options_get_number(global_s_options, "base-index");
	xasprintf(&target, "=%s:", name);
	for (i = 0; i < snap->nwindows; i++) {
		w = &snap->windows[i];
		if (strcmp(w->f[4], from) != 0)
			continue;

		/* The session's folder and size, and a window to stand in. */
		if (first) {
			argv[0] = "new-session"; argv[1] = "-d";
			argv[2] = "-s"; argv[3] = name;
			argv[4] = "-c"; argv[5] = w->f[5];
			argv[6] = "-x"; argv[7] = w->f[2];
			argv[8] = "-y"; argv[9] = w->f[3];
			ss_add_command(evb, argv, 10);
			first = 0;
		}

		/* Each window after the last, so they keep their order. */
		k++;
		if (atoi(w->f[1]))
			active = k;
		panes = (w->n - 11) / 3;
		start = ss_start_command(w->f[12], w->f[13]);
		argv[0] = "new-window"; argv[1] = "-a"; argv[2] = "-t";
		argv[3] = target; argv[4] = "-c"; argv[5] = w->f[11];
		argv[6] = start;
		ss_add_command(evb, argv, 7);
		free(start);
		for (p = 1; p < panes; p++) {
			start = ss_start_command(w->f[12 + p * 3],
			    w->f[13 + p * 3]);
			argv[0] = "split-window"; argv[1] = "-t";
			argv[2] = target; argv[3] = "-c";
			argv[4] = w->f[11 + p * 3]; argv[5] = start;
			ss_add_command(evb, argv, 6);
			free(start);
		}
		argv[0] = "select-layout"; argv[1] = "-t"; argv[2] = target;
		argv[3] = w->f[9];
		ss_add_command(evb, argv, 4);
		if (!atoi(w->f[7])) {
			argv[0] = "rename-window"; argv[1] = "-t";
			argv[2] = target; argv[3] = w->f[8];
			ss_add_command(evb, argv, 4);
		}
		xasprintf(&idx, "=%s:.%lld", name, strtoll(w->f[10], NULL, 10) +
		    options_get_number(global_w_options, "pane-base-index"));
		argv[0] = "select-pane"; argv[1] = "-t"; argv[2] = idx;
		ss_add_command(evb, argv, 3);
		free(idx);
	}
	if (active != 0) {
		xasprintf(&idx, "=%s:%lld", name, base + active);
		argv[0] = "select-window"; argv[1] = "-t"; argv[2] = idx;
		ss_add_command(evb, argv, 3);
		free(idx);
	}
	xasprintf(&idx, "=%s:%lld", name, base);
	argv[0] = "kill-window"; argv[1] = "-t"; argv[2] = idx;
	ss_add_command(evb, argv, 3);
	free(idx);
	free(target);
}

/*
 * The commands that rebuild the saved servers in snaps in this server, beside
 * its own sessions, and show them on client c if it is not NULL: each session
 * with its windows, splits, layouts, folders and names, Claude Code resumed and
 * programs started again. A session name already taken gets "-2" and up; each
 * snapshot's primary session is made first. replace is a session that goes
 * once c shows the set, if it is still a lone shell: it steps aside first so
 * the set's sessions keep their names.
 */
static char *
ss_restore(struct ss_snap **snaps, u_int nsnaps, struct session *replace,
    struct client *c)
{
	struct evbuffer	*evb;
	struct ss_snap	*snap;
	const char	*argv[6];
	char		**made = NULL, **from = NULL, *aside = NULL, *name;
	char		*show = NULL, *out, *target;
	u_int		 i, j, k, nmade = 0, nfrom;
	int		 seen;

	if (c == NULL || !ss_lone_shell(replace))
		replace = NULL;

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	if (replace != NULL) {
		xasprintf(&aside, "restoring-%ld", (long)getpid());
		xasprintf(&target, "=%s", replace->name);
		argv[0] = "rename-session"; argv[1] = "-t"; argv[2] = target;
		argv[3] = aside;
		ss_add_command(evb, argv, 4);
		free(target);
	}

	for (i = 0; i < nsnaps; i++) {
		snap = snaps[i];

		/* Its sessions in the order of their windows, primary first. */
		nfrom = 0;
		for (j = 0; j < snap->nwindows; j++) {
			seen = 0;
			for (k = 0; k < nfrom; k++) {
				if (strcmp(from[k], snap->windows[j].f[4]) == 0)
					seen = 1;
			}
			if (seen)
				continue;
			from = xreallocarray(from, nfrom + 1, sizeof *from);
			if (strcmp(snap->windows[j].f[4], "primary") == 0) {
				memmove(from + 1, from, nfrom * sizeof *from);
				from[0] = snap->windows[j].f[4];
			} else
				from[nfrom] = snap->windows[j].f[4];
			nfrom++;
		}

		for (k = 0; k < nfrom; k++) {
			name = xstrdup(from[k]);
			for (j = 2; ss_name_taken(name, replace, made, nmade);
			    j++) {
				free(name);
				xasprintf(&name, "%s-%u", from[k], j);
			}
			made = xreallocarray(made, nmade + 1, sizeof *made);
			made[nmade++] = name;
			if (show == NULL && i == 0 &&
			    strcmp(from[k], snap->attached) == 0)
				show = name;
			ss_restore_session(evb, snap, from[k], name);
		}
	}
	free(from);

	if (c != NULL && nmade != 0) {
		xasprintf(&target, "=%s", show != NULL ? show : made[0]);
		argv[0] = "switch-client"; argv[1] = "-c"; argv[2] = c->name;
		argv[3] = "-t"; argv[4] = target;
		ss_add_command(evb, argv, 5);
		free(target);
	}
	if (aside != NULL) {
		xasprintf(&target, "=%s", aside);
		argv[0] = "kill-session"; argv[1] = "-t"; argv[2] = target;
		ss_add_command(evb, argv, 3);
		free(target);
		free(aside);
	}
	for (i = 0; i < nmade; i++)
		free(made[i]);
	free(made);

	out = xmalloc(EVBUFFER_LENGTH(evb) + 1);
	memcpy(out, EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
	out[EVBUFFER_LENGTH(evb)] = '\0';
	evbuffer_free(evb);
	return (out);
}

/*
 * Rebuild saved servers (snapshot files, a relative path under
 * @session-set-dir) in this server, beside its own sessions, and show them on
 * client c if it is not NULL. The files go to archive/. Returns the commands
 * that do it, or NULL with cause.
 */
char *
session_set_restore(const char **paths, u_int npaths, struct client *c,
    char **cause)
{
	const char	 *dir;
	struct ss_snap	**snaps;
	char		**files, *out;
	u_int		  i, n;

	if ((dir = ss_dir()) == NULL) {
		*cause = xstrdup("@session-set-dir is not set");
		return (NULL);
	}
	snaps = xcalloc(npaths, sizeof *snaps);
	files = xcalloc(npaths, sizeof *files);
	for (i = 0; i < npaths; i++) {
		if (paths[i][0] == '/' || paths[i][0] == '\\' ||
		    (paths[i][0] != '\0' && paths[i][1] == ':'))
			files[i] = xstrdup(paths[i]);
		else
			xasprintf(&files[i], "%s/%s", dir, paths[i]);
		if ((snaps[i] = ss_snap_read(files[i])) == NULL) {
			xasprintf(cause, "%s is not a session-set snapshot",
			    files[i]);
			break;
		}
	}
	n = i;
	out = NULL;
	if (n == npaths) {
		out = ss_restore(snaps, n, NULL, c);
		for (i = 0; i < n; i++)
			ss_archive(dir, files[i], snaps[i]);
	}
	for (i = 0; i < npaths; i++) {
		ss_snap_free(snaps[i]);
		free(files[i]);
	}
	free(snaps);
	free(files);
	return (out);
}

static struct event	 ss_offer_timer;
static struct client	*ss_offer_client;

/* A client's choice in the offer: Enter restores, Escape archives. */
static void
ss_offer_choice(__unused struct menu *menu, u_int idx, key_code key,
    void *data)
{
	struct client		*c = data;
	struct ss_snap		*snap;
	struct cmdq_state	*state;
	const char		*dir;
	char			*previous, *commands, *error;

	ss_offer_client = NULL;
	if (idx == UINT_MAX || (dir = ss_dir()) == NULL)
		return;
	previous = ss_snapshot_path(dir, "-previous");
	if ((snap = ss_snap_read(previous)) == NULL) {
		free(previous);
		return;
	}
	if (key == '\r' && c->session != NULL) {
		commands = ss_restore(&snap, 1, c->session, c);
		state = cmdq_new_state(NULL, NULL, 0);
		if (cmd_parse_and_append(commands, NULL, c, state, &error) ==
		    CMD_PARSE_ERROR) {
			cmdq_append(c, cmdq_get_error(error));
			free(error);
		}
		cmdq_free_state(state);
		free(commands);
	}
	if (key == '\r' || key == '\033')
		ss_archive(dir, previous, snap);
	ss_snap_free(snap);
	free(previous);
}

/* The offer: when the set was saved, its sessions, and the two choices. */
static struct menu *
ss_offer_menu(struct ss_snap *snap, struct client *c)
{
	struct menu		*menu;
	struct menu_item	 item;
	struct tm		*tm, now;
	time_t			 t;
	char			 when[64], day[64], *line, *name, *p;
	u_int			 i, j, windows, claude, pane;
	int			 seen;

	t = time(NULL);
	tm = localtime(&t);
	now = *tm;
	t = (time_t)snap->seen;
	tm = localtime(&t);
	strftime(when, sizeof when, "%I:%M %p", tm);
	if (tm->tm_year == now.tm_year && tm->tm_yday == now.tm_yday)
		strlcpy(day, "today", sizeof day);
	else if (tm->tm_year == now.tm_year && tm->tm_yday + 1 == now.tm_yday)
		strlcpy(day, "yesterday", sizeof day);
	else
		strftime(day, sizeof day, "%A %b %d", tm);

	menu = menu_create(" Previous session set ");
	memset(&item, 0, sizeof item);
	item.key = KEYC_NONE;
	xasprintf(&line, "-Last saved %s %s", when + (when[0] == '0'), day);
	item.name = line;
	menu_add_item(menu, &item, NULL, c, NULL);
	free(line);
	menu_add_item(menu, NULL, NULL, c, NULL);

	for (i = 0; i < snap->nwindows; i++) {
		seen = 0;
		for (j = 0; j < i; j++) {
			if (strcmp(snap->windows[j].f[4],
			    snap->windows[i].f[4]) == 0)
				seen = 1;
		}
		if (seen)
			continue;
		windows = claude = 0;
		for (j = i; j < snap->nwindows; j++) {
			if (strcmp(snap->windows[j].f[4],
			    snap->windows[i].f[4]) != 0)
				continue;
			windows++;
			for (pane = 12; pane < snap->windows[j].n; pane += 3) {
				if (*snap->windows[j].f[pane] != '\0')
					claude++;
			}
		}
		name = xmalloc(strlen(snap->windows[i].f[4]) * 2 + 1);
		for (p = name, j = 0; snap->windows[i].f[4][j] != '\0'; j++) {
			if ((*p++ = snap->windows[i].f[4][j]) == '#')
				*p++ = '#';
		}
		*p = '\0';
		if (claude != 0) {
			xasprintf(&line, "-%-20s%u window%s, %u Claude", name,
			    windows, windows == 1 ? "" : "s", claude);
		} else {
			xasprintf(&line, "-%-20s%u window%s", name, windows,
			    windows == 1 ? "" : "s");
		}
		free(name);
		item.name = line;
		menu_add_item(menu, &item, NULL, c, NULL);
		free(line);
	}

	menu_add_item(menu, NULL, NULL, c, NULL);
	item.name = "Restore";
	item.key = '\r';
	menu_add_item(menu, &item, NULL, c, NULL);
	item.name = "Start fresh";
	item.key = '\033';
	menu_add_item(menu, &item, NULL, c, NULL);
	return (menu);
}

/*
 * Offer the previous set on the first client that can show it. Tried when a
 * client attaches and when one is resized (a client too small for the menu
 * waits for its terminal's size); a menu closed without a choice, by a detach
 * say, leaves the set to be offered on the next attach.
 */
static void
ss_offer(__unused int fd, __unused short events, __unused void *data)
{
	const char	*dir;
	struct client	*c;
	struct ss_snap	*snap;
	struct menu	*menu;
	char		*previous;
	u_int		 w, h, px, py;

	if (ss_stopped || ss_offer_client != NULL || (dir = ss_dir()) == NULL)
		return;
	if (!ss_set_aside)
		ss_take_earlier(dir);
	previous = ss_snapshot_path(dir, "-previous");
	snap = ss_snap_read(previous);
	free(previous);
	if (snap == NULL)
		return;

	TAILQ_FOREACH(c, &clients, entry) {
		if (c->session == NULL || c->overlay_draw != NULL ||
		    (c->flags & (CLIENT_CONTROL|CLIENT_DEAD|CLIENT_EXIT|
		    CLIENT_SUSPENDED)))
			continue;
		break;
	}
	if (c == NULL) {
		ss_snap_free(snap);
		return;
	}

	menu = ss_offer_menu(snap, c);
	ss_snap_free(snap);
	w = menu->width + 4;
	h = menu->count + 2;
	px = (c->tty.sx > w ? (c->tty.sx - w) / 2 : 0);
	py = (c->tty.sy > h ? (c->tty.sy - h) / 2 : 0);
	if (menu_display(menu, 0, 0, NULL, px, py, c, BOX_LINES_DEFAULT, NULL,
	    NULL, NULL, NULL, ss_offer_choice, c) != 0) {
		menu_free(menu);
		return;
	}
	ss_offer_client = c;
}

/* A client attached or was resized: offer the previous set if there is one. */
void
session_set_offer_soon(void)
{
	struct timeval	tv = { 0, 0 };

	if (ss_stopped || ss_offer_client != NULL || ss_dir() == NULL)
		return;
	if (!event_initialized(&ss_offer_timer))
		evtimer_set(&ss_offer_timer, ss_offer, NULL);
	if (!evtimer_pending(&ss_offer_timer, NULL))
		evtimer_add(&ss_offer_timer, &tv);
}
