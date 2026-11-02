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
#include <unistd.h>
#endif

#include "tmux.h"

/*
 * The session set: a server whose global option @session-set-dir names a
 * folder keeps live/<socket>.json there, everything needed to rebuild its
 * sessions, windows and panes after a reboot or a crash, written by the server
 * itself whenever something it holds changes (session_set_changed, from every
 * command, notification and reported folder, written only when the result
 * differs from the last one). What each pane runs is looked up apart from that,
 * a second after a pane prints (ss_programs). No process is started for it.
 *
 * Each write also keeps, on every window, the line that would bring the window
 * back. When a window is destroyed (killed, or its last pane exited, but not
 * moved) that line goes on top of closed-windows in the same folder, and
 * reopen-window takes it off again. Nothing is recorded while the server
 * itself shuts down.
 *
 * A pane's Claude Code session comes from the pane options @claude-session and
 * @claude-cwd, which Claude Code's own hooks set. @session-set-resume is the
 * command that resumes one ("{id}" for its ID); @session-set-restart-<program>
 * the command that starts a program again. Without either a pane comes back as
 * a shell in its folder.
 */

#define SESSION_SET_CLOSED_MAX 25

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

static void
ss_json_string(struct evbuffer *evb, const char *s)
{
	const u_char	*p;

	if (s == NULL) {
		evbuffer_add_printf(evb, "null");
		return;
	}
	evbuffer_add(evb, "\"", 1);
	for (p = (const u_char *)s; *p != '\0'; p++) {
		if (*p == '"' || *p == '\\')
			evbuffer_add_printf(evb, "\\%c", *p);
		else if (*p < 0x20)
			evbuffer_add_printf(evb, "\\u%04x", *p);
		else
			evbuffer_add(evb, p, 1);
	}
	evbuffer_add(evb, "\"", 1);
}

/* A field of a closed-window line: tabs, newlines and backslashes escaped. */
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

/*
 * What a pane runs: the program its last look at the process table found
 * (ss_programs), or before that its shell's.
 */
static char *
ss_pane_command(struct window_pane *wp)
{
	const char	*p, *q;
	char		*name, *dot;

	if (wp->session_set_command != NULL)
		return (xstrdup(wp->session_set_command));
	if (wp->shell == NULL)
		return (NULL);
	p = wp->shell;
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
 * The JSON of one window, and the line that reopens it. A window is not
 * offered for reopening when every pane runs the at menu (it closes whenever
 * the menu does) or its session is a restore's stand-in.
 */
static void
ss_window(struct evbuffer *evb, struct session *s, struct winlink *wl,
    u_int position)
{
	struct window		*w = wl->window;
	struct window_pane	*wp;
	struct evbuffer		*line;
	char			*layout, *cwd, *command;
	const char		*claude;
	u_int			 i, active = 0;
	int			 first = 1, autoname, only_menu = 1;

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

	evbuffer_add_printf(evb, "{\"active\":%s,\"autoName\":%s,\"name\":",
	    wl == s->curw ? "true" : "false", autoname ? "true" : "false");
	ss_json_string(evb, w->name);
	evbuffer_add_printf(evb, ",\"width\":%u,\"height\":%u,\"layout\":",
	    w->sx, w->sy);
	ss_json_string(evb, layout);
	evbuffer_add_printf(evb, ",\"panes\":[");
	TAILQ_FOREACH(wp, &w->panes, entry) {
		cwd = ss_pane_cwd(s, wl, wp);
		claude = ss_pane_option(wp, "@claude-session");
		command = ss_pane_command(wp);
		if (command == NULL || strcmp(command, "fzt-automate") != 0)
			only_menu = 0;

		evbuffer_add_printf(evb, "%s{\"id\":\"%%%u\",\"active\":%s,"
		    "\"cwd\":", first ? "" : ",", wp->id,
		    wp == w->active ? "true" : "false");
		ss_json_string(evb, cwd);
		evbuffer_add_printf(evb, ",\"claude\":");
		ss_json_string(evb, claude);
		evbuffer_add_printf(evb, ",\"command\":");
		ss_json_string(evb, command);
		evbuffer_add(evb, "}", 1);
		first = 0;

		ss_field(line, cwd);
		ss_field(line, claude);
		ss_field(line, command);
		free(command);
		free(cwd);
	}
	evbuffer_add(evb, "]}", 2);

	free(w->session_set_line);
	w->session_set_line = NULL;
	if (!only_menu && strncmp(s->name, "restoring-", 10) != 0) {
		w->session_set_line = xmalloc(EVBUFFER_LENGTH(line) + 1);
		memcpy(w->session_set_line, EVBUFFER_DATA(line),
		    EVBUFFER_LENGTH(line));
		w->session_set_line[EVBUFFER_LENGTH(line)] = '\0';
	}
	evbuffer_free(line);
	free(layout);
}

/* The sessions, as the JSON array the snapshot holds. */
static char *
ss_sessions(void)
{
	struct evbuffer		*evb;
	struct session		*s;
	struct winlink		*wl;
	u_int			 position;
	int			 first_s = 1, first_w;
	char			*out;

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	evbuffer_add(evb, "[", 1);
	RB_FOREACH(s, sessions, &sessions) {
		evbuffer_add_printf(evb, "%s{\"name\":", first_s ? "" : ",");
		ss_json_string(evb, s->name);
		evbuffer_add_printf(evb, ",\"path\":");
		ss_json_string(evb, s->cwd);
		evbuffer_add_printf(evb, ",\"windows\":[");
		first_w = 1;
		position = 0;
		RB_FOREACH(wl, winlinks, &s->windows) {
			position++;
			if (!first_w)
				evbuffer_add(evb, ",", 1);
			ss_window(evb, s, wl, position);
			first_w = 0;
		}
		evbuffer_add(evb, "]}", 2);
		first_s = 0;
	}
	evbuffer_add(evb, "]", 1);

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
	xasprintf(&path, "%s/%s%s.json", live, ss_socket_name(), suffix);
	free(live);
	return (path);
}

/*
 * The snapshot an earlier server of this name left (after a reboot or a
 * recycle) is set aside under its start time, to be offered for restore; an
 * upgraded server keeps its start time, so its own snapshot stays.
 */
static void
ss_set_aside_earlier(const char *dir, const char *path)
{
	char		*data, *p, *aside, suffix[32];
	long long	 started;

	ss_set_aside = 1;
	if ((data = ss_read_file(path)) == NULL)
		return;
	p = strstr(data, "\"startTime\":");
	if (p != NULL) {
		started = strtoll(p + 12, NULL, 10);
		if (started != (long long)start_time.tv_sec) {
			snprintf(suffix, sizeof suffix, "-%lld", started);
			aside = ss_snapshot_path(dir, suffix);
			if (ss_replace(path, aside) != 0)
				log_debug("session-set: can't set aside %s", path);
			free(aside);
		}
	}
	free(data);
}

static int
ss_write(const char *dir, const char *sessions, long long closed)
{
	struct evbuffer	*evb;
	char		*path;
	int		 retval;

	path = ss_snapshot_path(dir, "");
	if (!ss_set_aside)
		ss_set_aside_earlier(dir, path);

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
	evbuffer_add_printf(evb, "{\"socket\":");
	ss_json_string(evb, socket_path);
	evbuffer_add_printf(evb, ",\"pid\":%ld,\"startTime\":%lld,"
	    "\"seenAt\":%lld,\"closedAt\":", (long)getpid(),
	    (long long)start_time.tv_sec, (long long)time(NULL));
	if (closed != 0)
		evbuffer_add_printf(evb, "%lld", closed);
	else
		evbuffer_add_printf(evb, "null");
	evbuffer_add_printf(evb, ",\"attached\":");
	ss_json_string(evb, ss_attached);
	evbuffer_add_printf(evb, ",\"sessions\":%s}\n", sessions);

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
