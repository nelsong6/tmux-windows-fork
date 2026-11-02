#include <sys/types.h>
#ifndef _WIN32
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

#ifdef HAVE_SERVER_UPGRADE

/*
 * Upgrading a running server to another build without restarting anything in
 * it. The server waits until nothing is in flight, stops reading its panes'
 * consoles (parsing what was already read), starts the new build and hands it
 * every console, pipe, process and socket together with its sessions, windows,
 * screens, options, key bindings and buffers. The new build rebuilds all of it
 * without touching any of them and says so; only then does this server exit,
 * and only then does the new one start reading. If anything fails before that,
 * this server carries on as it was.
 */

#define UPGRADE_MAGIC "tmux-upgrade"
#define UPGRADE_VERSION 1
#define UPGRADE_ACK_TIMEOUT 30000
#define UPGRADE_FLUSH_TIMEOUT 5000

int			 server_upgrading;
static char		*upgrade_exe;
static char		*upgrade_status;
static struct event	 upgrade_timer;

static void	upgrade_timer_callback(int, short, void *);

#ifdef _WIN32
typedef HANDLE upgrade_child;
#else
/*
 * On Linux the new build is started from this process and inherits every
 * descriptor handed over at the same number; these are the ones to keep open
 * across its exec.
 */
typedef int upgrade_child;
static int	*upgrade_fds;
static u_int	 upgrade_nfds;

static void
upgrade_keep_fd(int fd)
{
	upgrade_fds = xreallocarray(upgrade_fds, upgrade_nfds + 1,
	    sizeof *upgrade_fds);
	upgrade_fds[upgrade_nfds++] = fd;
}
#endif

static void printflike(1, 2)
upgrade_set_status(const char *fmt, ...)
{
	va_list	ap;

	free(upgrade_status);
	va_start(ap, fmt);
	xvasprintf(&upgrade_status, fmt, ap);
	va_end(ap);
	log_debug("upgrade: %s", upgrade_status);
}

/* Writing and reading the stream. */

void
upw_u32(struct evbuffer *evb, uint32_t v)
{
	evbuffer_add(evb, &v, sizeof v);
}

void
upw_u64(struct evbuffer *evb, uint64_t v)
{
	evbuffer_add(evb, &v, sizeof v);
}

void
upw_data(struct evbuffer *evb, const void *data, size_t size)
{
	upw_u32(evb, (uint32_t)size);
	if (size != 0)
		evbuffer_add(evb, data, size);
}

void
upw_str(struct evbuffer *evb, const char *s)
{
	if (s == NULL)
		upw_u32(evb, UINT32_MAX);
	else
		upw_data(evb, s, strlen(s));
}

void
upw_obj(struct evbuffer *evb, struct evbuffer *obj)
{
	upw_u32(evb, (uint32_t)EVBUFFER_LENGTH(obj));
	evbuffer_add_buffer(evb, obj);
}

static const void *
upr_take(struct upgrade_reader *r, size_t size)
{
	const void	*p;

	if (r->error || size > r->left) {
		/* An object that ends early is read as zeroes. */
		if (!r->error && r->left == 0)
			return (NULL);
		r->error = 1;
		return (NULL);
	}
	p = r->p;
	r->p += size;
	r->left -= size;
	return (p);
}

uint32_t
upr_u32(struct upgrade_reader *r)
{
	const void	*p = upr_take(r, sizeof (uint32_t));
	uint32_t	 v = 0;

	if (p != NULL)
		memcpy(&v, p, sizeof v);
	return (v);
}

uint64_t
upr_u64(struct upgrade_reader *r)
{
	const void	*p = upr_take(r, sizeof (uint64_t));
	uint64_t	 v = 0;

	if (p != NULL)
		memcpy(&v, p, sizeof v);
	return (v);
}

const void *
upr_data(struct upgrade_reader *r, size_t *size)
{
	const void	*p;
	uint32_t	 n = upr_u32(r);

	if (n == UINT32_MAX)
		n = 0;
	p = upr_take(r, n);
	*size = (p == NULL) ? 0 : n;
	return (p == NULL ? "" : p);
}

char *
upr_str(struct upgrade_reader *r)
{
	const void	*p;
	uint32_t	 n;
	char		*s;

	if (r->left == 0)
		return (NULL);
	n = upr_u32(r);
	if (n == UINT32_MAX || r->error)
		return (NULL);
	if ((p = upr_take(r, n)) == NULL && n != 0)
		return (NULL);
	s = xmalloc((size_t)n + 1);
	if (n != 0)
		memcpy(s, p, n);
	s[n] = '\0';
	return (s);
}

void
upr_obj(struct upgrade_reader *r, struct upgrade_reader *obj)
{
	uint32_t	 n = upr_u32(r);
	const void	*p = upr_take(r, n);

	obj->p = p;
	obj->left = (p == NULL) ? 0 : n;
	obj->error = r->error;
}

void
upw_utf8(struct evbuffer *evb, const struct utf8_data *ud)
{
	upw_data(evb, ud->data, ud->size);
	upw_u32(evb, ud->width);
}

void
upr_utf8(struct upgrade_reader *r, struct utf8_data *ud)
{
	const void	*p;
	size_t		 size;

	memset(ud, 0, sizeof *ud);
	p = upr_data(r, &size);
	if (size > sizeof ud->data) {
		r->error = 1;
		size = 0;
	}
	memcpy(ud->data, p, size);
	ud->size = ud->have = (u_char)size;
	ud->width = (u_char)upr_u32(r);
}

/* A hyperlink is a table index here, so it goes by its URI and ID. */
void
upw_link(struct evbuffer *evb, struct hyperlinks *hl, u_int link)
{
	const char	*uri, *internal_id, *external_id;

	if (link == 0 || hl == NULL ||
	    !hyperlinks_get(hl, link, &uri, &internal_id, &external_id)) {
		upw_str(evb, NULL);
		upw_str(evb, NULL);
		return;
	}
	upw_str(evb, uri);
	upw_str(evb, internal_id);
}

u_int
upr_link(struct upgrade_reader *r, struct hyperlinks *hl)
{
	char	*uri = upr_str(r), *internal_id = upr_str(r);
	u_int	 link = 0;

	if (uri != NULL && hl != NULL)
		link = hyperlinks_put(hl, uri, internal_id);
	free(uri);
	free(internal_id);
	return (link);
}

void
upw_cell(struct evbuffer *evb, struct hyperlinks *hl,
    const struct grid_cell *gc)
{
	upw_utf8(evb, &gc->data);
	upw_u32(evb, gc->attr);
	upw_u32(evb, gc->flags);
	upw_u32(evb, gc->fg);
	upw_u32(evb, gc->bg);
	upw_u32(evb, gc->us);
	upw_link(evb, hl, gc->link);
}

void
upr_cell(struct upgrade_reader *r, struct hyperlinks *hl,
    struct grid_cell *gc)
{
	memset(gc, 0, sizeof *gc);
	upr_utf8(r, &gc->data);
	gc->attr = upr_u32(r);
	gc->flags = upr_u32(r);
	gc->fg = upr_u32(r);
	gc->bg = upr_u32(r);
	gc->us = upr_u32(r);
	gc->link = upr_link(r, hl);
}

static void
upw_time(struct evbuffer *evb, const struct timeval *tv)
{
	upw_u64(evb, (uint64_t)tv->tv_sec);
	upw_u64(evb, (uint64_t)tv->tv_usec);
}

static void
upr_time(struct upgrade_reader *r, struct timeval *tv)
{
	tv->tv_sec = (long)upr_u64(r);
	tv->tv_usec = (long)upr_u64(r);
}

/* Environments and key bindings. */

static void
upgrade_save_environ(struct evbuffer *evb, struct environ *env)
{
	struct evbuffer		*o = evbuffer_new();
	struct environ_entry	*envent;
	u_int			 n = 0;

	for (envent = environ_first(env); envent != NULL;
	    envent = environ_next(envent))
		n++;
	upw_u32(o, n);
	for (envent = environ_first(env); envent != NULL;
	    envent = environ_next(envent)) {
		upw_str(o, envent->name);
		upw_str(o, envent->value);
		upw_u32(o, envent->flags);
	}
	upw_obj(evb, o);
	evbuffer_free(o);
}

static void
upgrade_restore_environ(struct upgrade_reader *r, struct environ *env)
{
	struct upgrade_reader	 o;
	char			*name, *value;
	u_int			 n, i, flags;

	upr_obj(r, &o);
	n = upr_u32(&o);
	for (i = 0; i < n && !o.error; i++) {
		name = upr_str(&o);
		value = upr_str(&o);
		flags = upr_u32(&o);
		if (name != NULL) {
			if (value == NULL)
				environ_clear(env, name);
			else
				environ_set(env, name, flags, "%s", value);
		}
		free(name);
		free(value);
	}
	if (o.error)
		r->error = 1;
}

static void
upgrade_save_keys(struct evbuffer *evb)
{
	struct evbuffer		*o = evbuffer_new();
	struct key_table	*table;
	struct key_binding	*bd;
	char			*s;
	u_int			 n;

	n = 0;
	for (table = key_bindings_first_table(); table != NULL;
	    table = key_bindings_next_table(table))
		n++;
	upw_u32(o, n);
	for (table = key_bindings_first_table(); table != NULL;
	    table = key_bindings_next_table(table)) {
		upw_str(o, table->name);
		n = 0;
		for (bd = key_bindings_first(table); bd != NULL;
		    bd = key_bindings_next(table, bd))
			n++;
		upw_u32(o, n);
		for (bd = key_bindings_first(table); bd != NULL;
		    bd = key_bindings_next(table, bd)) {
			upw_u64(o, bd->key);
			upw_str(o, bd->note);
			upw_u32(o, bd->flags);
			s = cmd_list_print(bd->cmdlist, 0);
			upw_str(o, s);
			free(s);
		}
	}
	upw_obj(evb, o);
	evbuffer_free(o);
}

/* Take out every binding of this build's own, then put the old server's in. */
static void
upgrade_restore_keys(struct upgrade_reader *r)
{
	struct upgrade_reader	 o;
	struct key_table	*table;
	struct key_binding	*bd;
	struct cmd_parse_result	*pr;
	struct cmd_parse_input	 pi;
	key_code		*keys = NULL, key;
	char			*name, *note, *cmd;
	u_int			 ntables, n, i, j, k, flags;

	for (table = key_bindings_first_table(); table != NULL;
	    table = key_bindings_next_table(table)) {
		n = 0;
		for (bd = key_bindings_first(table); bd != NULL;
		    bd = key_bindings_next(table, bd)) {
			keys = xreallocarray(keys, n + 1, sizeof *keys);
			keys[n++] = bd->key;
		}
		for (k = 0; k < n; k++)
			key_bindings_remove(table->name, keys[k]);
	}
	free(keys);

	upr_obj(r, &o);
	ntables = upr_u32(&o);
	for (i = 0; i < ntables && !o.error; i++) {
		name = upr_str(&o);
		n = upr_u32(&o);
		for (j = 0; j < n && !o.error; j++) {
			key = upr_u64(&o);
			note = upr_str(&o);
			flags = upr_u32(&o);
			cmd = upr_str(&o);
			if (name != NULL && cmd != NULL) {
				/*
				 * A saved binding is commands the old server
				 * had already resolved: an alias that has since
				 * taken one of their names is not what it ran.
				 */
				memset(&pi, 0, sizeof pi);
				pi.flags = CMD_PARSE_NOALIAS;
				pr = cmd_parse_from_string(cmd, &pi);
				if (pr->status == CMD_PARSE_SUCCESS) {
					key_bindings_add(name, key, note,
					    flags & KEY_BINDING_REPEAT,
					    pr->cmdlist);
				} else {
					log_debug("%s: %s: %s", __func__, name,
					    pr->error);
					free(pr->error);
				}
			}
			free(note);
			free(cmd);
		}
		free(name);
	}
	if (o.error)
		r->error = 1;
}

/* Windows, panes and screens. */

static void
upgrade_save_screen(struct evbuffer *evb, struct screen *s)
{
	struct evbuffer	*o = evbuffer_new();

	upw_str(o, s->title);
	upw_str(o, s->path);
	grid_upgrade_save(o, s->grid, s->hyperlinks);
	upw_u32(o, s->saved_grid != NULL);
	if (s->saved_grid != NULL)
		grid_upgrade_save(o, s->saved_grid, s->hyperlinks);
	upw_u32(o, s->cx);
	upw_u32(o, s->cy);
	upw_u32(o, s->cstyle);
	upw_u32(o, s->default_cstyle);
	upw_u32(o, s->ccolour);
	upw_u32(o, s->default_ccolour);
	upw_u32(o, s->rupper);
	upw_u32(o, s->rlower);
	upw_u32(o, s->mode);
	upw_u32(o, s->default_mode);
	upw_u32(o, s->saved_cx);
	upw_u32(o, s->saved_cy);
	upw_cell(o, s->hyperlinks, &s->saved_cell);
	upw_u32(o, s->saved_flags);
	upw_data(o, s->tabs, bitstr_size(s->grid->sx));
	upw_obj(evb, o);
	evbuffer_free(o);
}

static void
upgrade_restore_screen(struct upgrade_reader *r, struct screen *s)
{
	struct upgrade_reader	 o;
	struct grid		*gd;
	char			*title, *path;
	const void		*tabs;
	size_t			 size;

	upr_obj(r, &o);
	title = upr_str(&o);
	path = upr_str(&o);
	if ((gd = grid_upgrade_restore(&o, s->hyperlinks)) == NULL) {
		free(title);
		free(path);
		r->error = 1;
		return;
	}
	grid_destroy(s->grid);
	s->grid = gd;
	if (upr_u32(&o)) {
		if ((gd = grid_upgrade_restore(&o, s->hyperlinks)) == NULL) {
			free(title);
			free(path);
			r->error = 1;
			return;
		}
		s->saved_grid = gd;
	}
	if (title != NULL)
		screen_set_title(s, title);
	if (path != NULL)
		screen_set_path(s, path);
	free(title);
	free(path);

	s->cx = upr_u32(&o);
	s->cy = upr_u32(&o);
	s->cstyle = upr_u32(&o);
	s->default_cstyle = upr_u32(&o);
	s->ccolour = upr_u32(&o);
	s->default_ccolour = upr_u32(&o);
	s->rupper = upr_u32(&o);
	s->rlower = upr_u32(&o);
	s->mode = upr_u32(&o);
	s->default_mode = upr_u32(&o);
	s->saved_cx = upr_u32(&o);
	s->saved_cy = upr_u32(&o);
	upr_cell(&o, s->hyperlinks, &s->saved_cell);
	s->saved_flags = upr_u32(&o);

	free(s->tabs);
	s->tabs = bit_alloc(s->grid->sx);
	tabs = upr_data(&o, &size);
	if (size > (size_t)bitstr_size(s->grid->sx))
		size = bitstr_size(s->grid->sx);
	memcpy(s->tabs, tabs, size);

	if (o.error || s->cx > s->grid->sx || s->cy >= s->grid->sy ||
	    s->rlower >= s->grid->sy || s->rupper > s->rlower)
		r->error = 1;
}

static void
upgrade_save_layout(struct evbuffer *evb, struct layout_cell *lc)
{
	struct layout_cell	*lcchild;
	u_int			 n = 0;

	upw_u32(evb, lc->type);
	upw_u32(evb, lc->sx);
	upw_u32(evb, lc->sy);
	upw_u32(evb, lc->xoff);
	upw_u32(evb, lc->yoff);
	upw_u32(evb, lc->wp == NULL ? UINT32_MAX : lc->wp->id);
	TAILQ_FOREACH(lcchild, &lc->cells, entry)
		n++;
	upw_u32(evb, n);
	TAILQ_FOREACH(lcchild, &lc->cells, entry)
		upgrade_save_layout(evb, lcchild);
}

static struct layout_cell *
upgrade_restore_layout(struct upgrade_reader *r, struct window *w,
    struct layout_cell *parent, u_int depth)
{
	struct layout_cell	*lc, *lcchild;
	struct window_pane	*wp;
	u_int			 type, sx, sy, xoff, yoff, id, n, i;

	type = upr_u32(r);
	sx = upr_u32(r);
	sy = upr_u32(r);
	xoff = upr_u32(r);
	yoff = upr_u32(r);
	id = upr_u32(r);
	n = upr_u32(r);
	if (r->error || depth > 100 || type > LAYOUT_WINDOWPANE) {
		r->error = 1;
		return (NULL);
	}

	lc = layout_create_cell(parent);
	layout_set_size(lc, sx, sy, xoff, yoff);
	if (type == LAYOUT_WINDOWPANE) {
		wp = window_pane_find_by_id(id);
		if (n != 0 || wp == NULL || wp->window != w ||
		    wp->layout_cell != NULL) {
			r->error = 1;
			return (lc);
		}
		layout_make_leaf(lc, wp);
		return (lc);
	}
	layout_make_node(lc, type);
	for (i = 0; i < n && !r->error; i++) {
		lcchild = upgrade_restore_layout(r, w, lc, depth + 1);
		if (lcchild != NULL)
			TAILQ_INSERT_TAIL(&lc->cells, lcchild, entry);
	}
	if (n == 0)
		r->error = 1;
	return (lc);
}

static void
upgrade_save_palette(struct evbuffer *evb, struct colour_palette *p)
{
	upw_u32(evb, p->fg);
	upw_u32(evb, p->bg);
	if (p->palette == NULL)
		upw_data(evb, NULL, 0);
	else
		upw_data(evb, p->palette, 256 * sizeof *p->palette);
}

static void
upgrade_restore_palette(struct upgrade_reader *r, struct colour_palette *p)
{
	const void	*data;
	size_t		 size;

	p->fg = upr_u32(r);
	p->bg = upr_u32(r);
	data = upr_data(r, &size);
	if (size == 256 * sizeof *p->palette) {
		if (p->palette == NULL)
			p->palette = xcalloc(256, sizeof *p->palette);
		memcpy(p->palette, data, size);
	}
}

static int
upgrade_save_pane(struct evbuffer *evb, struct window_pane *wp,
    upgrade_child child)
{
	struct evbuffer			*o = evbuffer_new();
#ifdef _WIN32
	struct win32_pty_handoff	 h;
#endif
	int				 i;

	upw_u32(o, wp->id);
	upw_u32(o, wp->sx);
	upw_u32(o, wp->sy);
	upw_u32(o, wp->xoff);
	upw_u32(o, wp->yoff);
	upw_u32(o, wp->flags);
	upw_u32(o, wp->argc);
	for (i = 0; i < wp->argc; i++)
		upw_str(o, wp->argv[i]);
	upw_str(o, wp->shell);
	upw_str(o, wp->cwd);
	upw_u32(o, (uint32_t)wp->pid);
	upw_str(o, wp->tty);
	upw_u32(o, wp->status);
	upw_time(o, &wp->dead_time);
	options_upgrade_save(o, wp->options);
	upgrade_save_palette(o, &wp->palette);
	upgrade_save_screen(o, &wp->base);

#ifdef _WIN32
	upw_u32(o, wp->win32_pty != NULL);
	if (wp->win32_pty != NULL) {
		if (win32_pty_export(wp->win32_pty, child, &h) != 0) {
			evbuffer_free(o);
			return (-1);
		}
		upw_u64(o, h.signal);
		upw_u64(o, h.reference);
		upw_u64(o, h.conhost);
		upw_u64(o, h.in);
		upw_u64(o, h.out);
		upw_u64(o, h.process);
		upw_u32(o, h.pid);
	}
#else
	(void)child;
	upw_u32(o, wp->fd != -1);
	if (wp->fd != -1) {
		upw_u32(o, wp->fd);
		upgrade_keep_fd(wp->fd);
	}
#endif
	upw_u32(o, wp->ictx != NULL);
	if (wp->ictx != NULL)
		input_upgrade_save(o, wp->ictx, wp->base.hyperlinks);
	upw_obj(evb, o);
	evbuffer_free(o);
	return (0);
}

static struct window_pane *
upgrade_restore_pane(struct upgrade_reader *r, struct window *w)
{
	struct upgrade_reader		 o;
	struct window_pane		*wp;
#ifdef _WIN32
	struct win32_pty_handoff	 h;
#endif
	struct grid			*gd;
	u_int				 id, next_window, next_pane;
	int				 i;
	char				*s;

	upr_obj(r, &o);
	id = upr_u32(&o);
	if (o.error || window_pane_find_by_id(id) != NULL) {
		r->error = 1;
		return (NULL);
	}
	window_get_next_ids(&next_window, &next_pane);
	window_set_next_ids(next_window, id);
	wp = window_add_pane(w, NULL, 0, SPAWN_FULLSIZE);
	window_set_next_ids(next_window, next_pane);

	wp->sx = upr_u32(&o);
	wp->sy = upr_u32(&o);
	wp->xoff = upr_u32(&o);
	wp->yoff = upr_u32(&o);
	/* Its cached default style is this process's to work out again. */
	wp->flags = (upr_u32(&o) & ~PANE_VISITED) | PANE_REDRAW|PANE_STYLECHANGED;
	wp->argc = upr_u32(&o);
	if (wp->argc < 0 || (size_t)wp->argc > o.left)
		o.error = 1;
	else if (wp->argc != 0) {
		wp->argv = xcalloc(wp->argc, sizeof *wp->argv);
		for (i = 0; i < wp->argc; i++) {
			s = upr_str(&o);
			wp->argv[i] = (s == NULL) ? xstrdup("") : s;
		}
	} else
		wp->argv = NULL;
	wp->shell = upr_str(&o);
	wp->cwd = upr_str(&o);
	wp->pid = (pid_t)upr_u32(&o);
	if ((s = upr_str(&o)) != NULL) {
		strlcpy(wp->tty, s, sizeof wp->tty);
		free(s);
	}
	wp->status = upr_u32(&o);
	upr_time(&o, &wp->dead_time);
	options_upgrade_restore(&o, wp->options);
	upgrade_restore_palette(&o, &wp->palette);

	/*
	 * The screen was made at the window's size; make it the pane's. The
	 * grid restored into it brings its own history limit.
	 */
	screen_free(&wp->base);
	screen_init(&wp->base, wp->sx, wp->sy, 0);
	upgrade_restore_screen(&o, &wp->base);
	gd = wp->base.grid;
	if (gd->sx != wp->sx || gd->sy != wp->sy)
		o.error = 1;

#ifdef _WIN32
	if (upr_u32(&o)) {
		h.signal = upr_u64(&o);
		h.reference = upr_u64(&o);
		h.conhost = upr_u64(&o);
		h.in = upr_u64(&o);
		h.out = upr_u64(&o);
		h.process = upr_u64(&o);
		h.pid = upr_u32(&o);
		if (!o.error) {
			wp->win32_pty = win32_pty_import(&h);
			if (wp->win32_pty == NULL)
				o.error = 1;
		}
	}
#else
	/* Inherited at the same number; not read until the old server goes. */
	if (upr_u32(&o))
		wp->fd = (int)upr_u32(&o);
#endif
	if (upr_u32(&o)) {
		wp->ictx = input_init(wp, NULL, &wp->palette, NULL);
		input_upgrade_restore(&o, wp->ictx, wp->base.hyperlinks);
	}
	if (o.error)
		r->error = 1;
	return (wp);
}

static int
upgrade_save_window(struct evbuffer *evb, struct window *w,
    upgrade_child child)
{
	struct evbuffer		*o = evbuffer_new();
	struct window_pane	*wp;
	struct layout_cell	*root;
	u_int			 n;

	upw_u32(o, w->id);
	upw_str(o, w->name);
	upw_u32(o, w->flags & ~(WINDOW_ZOOMED|WINDOW_WASZOOMED|WINDOW_RESIZE));
	upw_u32(o, (w->flags & WINDOW_ZOOMED) != 0);
	upw_u32(o, w->sx);
	upw_u32(o, w->sy);
	upw_u32(o, w->manual_sx);
	upw_u32(o, w->manual_sy);
	upw_u32(o, w->xpixel);
	upw_u32(o, w->ypixel);
	upw_time(o, &w->activity_time);
	upw_time(o, &w->creation_time);
	upw_time(o, &w->name_time);
	options_upgrade_save(o, w->options);

	n = 0;
	TAILQ_FOREACH(wp, &w->panes, entry)
		n++;
	upw_u32(o, n);
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (upgrade_save_pane(o, wp, child) != 0) {
			evbuffer_free(o);
			return (-1);
		}
	}
	root = (w->flags & WINDOW_ZOOMED) ? w->saved_layout_root :
	    w->layout_root;
	upgrade_save_layout(o, root);
	upw_u32(o, w->active == NULL ? UINT32_MAX : w->active->id);
	n = 0;
	TAILQ_FOREACH(wp, &w->last_panes, sentry)
		n++;
	upw_u32(o, n);
	TAILQ_FOREACH(wp, &w->last_panes, sentry)
		upw_u32(o, wp->id);
	upw_obj(evb, o);
	evbuffer_free(o);
	return (0);
}

static void
upgrade_restore_window(struct upgrade_reader *r)
{
	struct upgrade_reader	 o;
	struct window		*w;
	struct window_pane	*wp;
	u_int			 id, next_window, next_pane, flags, zoomed;
	u_int			 sx, sy, n, i;
	char			*name;

	upr_obj(r, &o);
	id = upr_u32(&o);
	name = upr_str(&o);
	flags = upr_u32(&o);
	zoomed = upr_u32(&o);
	sx = upr_u32(&o);
	sy = upr_u32(&o);
	if (o.error || sx == 0 || sy == 0 || window_find_by_id(id) != NULL) {
		free(name);
		r->error = 1;
		return;
	}

	window_get_next_ids(&next_window, &next_pane);
	window_set_next_ids(id, next_pane);
	w = window_create(sx, sy, 0, 0);
	window_set_next_ids(next_window, next_pane);
	free(w->name);
	w->name = (name == NULL) ? xstrdup("") : name;
	w->flags = flags;
	w->manual_sx = upr_u32(&o);
	w->manual_sy = upr_u32(&o);
	w->xpixel = upr_u32(&o);
	w->ypixel = upr_u32(&o);
	upr_time(&o, &w->activity_time);
	upr_time(&o, &w->creation_time);
	upr_time(&o, &w->name_time);
	options_upgrade_restore(&o, w->options);
	window_set_fill_character(w);

	n = upr_u32(&o);
	for (i = 0; i < n && !o.error; i++)
		upgrade_restore_pane(&o, w);
	if (o.error) {
		r->error = 1;
		return;
	}
	w->layout_root = upgrade_restore_layout(&o, w, NULL, 0);
	TAILQ_FOREACH(wp, &w->panes, entry) {
		if (wp->layout_cell == NULL)
			o.error = 1;
	}

	id = upr_u32(&o);
	w->active = window_pane_find_by_id(id);
	if (w->active == NULL || w->active->window != w)
		w->active = TAILQ_FIRST(&w->panes);
	n = upr_u32(&o);
	for (i = 0; i < n && !o.error; i++) {
		wp = window_pane_find_by_id(upr_u32(&o));
		if (wp != NULL && wp->window == w && wp != w->active &&
		    (~wp->flags & PANE_VISITED)) {
			TAILQ_INSERT_TAIL(&w->last_panes, wp, sentry);
			wp->flags |= PANE_VISITED;
		}
	}
	if (o.error) {
		r->error = 1;
		return;
	}
	if (zoomed)
		window_zoom(w->active);
}

/* Sessions. */

static void
upgrade_save_session(struct evbuffer *evb, struct session *s)
{
	struct evbuffer		*o = evbuffer_new();
	struct winlink		*wl;
	struct session_group	*sg;
	u_int			 n;

	upw_u32(o, s->id);
	upw_str(o, s->name);
	upw_str(o, s->cwd);
	upw_time(o, &s->creation_time);
	upw_time(o, &s->last_attached_time);
	upw_time(o, &s->activity_time);
	upw_time(o, &s->last_activity_time);
	upw_u32(o, s->flags);
	options_upgrade_save(o, s->options);
	upgrade_save_environ(o, s->environ);
	sg = session_group_contains(s);
	upw_str(o, sg == NULL ? NULL : sg->name);

	n = 0;
	RB_FOREACH(wl, winlinks, &s->windows)
		n++;
	upw_u32(o, n);
	RB_FOREACH(wl, winlinks, &s->windows) {
		upw_u32(o, wl->idx);
		upw_u32(o, wl->window->id);
		upw_u32(o, wl->flags);
	}
	upw_u32(o, s->curw == NULL ? UINT32_MAX : (uint32_t)s->curw->idx);
	n = 0;
	TAILQ_FOREACH(wl, &s->lastw, sentry)
		n++;
	upw_u32(o, n);
	TAILQ_FOREACH(wl, &s->lastw, sentry)
		upw_u32(o, wl->idx);
	upw_obj(evb, o);
	evbuffer_free(o);
}

static void
upgrade_restore_session(struct upgrade_reader *r)
{
	struct upgrade_reader	 o;
	struct session		*s;
	struct session_group	*sg;
	struct options		*oo;
	struct environ		*env;
	struct winlink		*wl;
	struct window		*w;
	struct timeval		 tv[4];
	char			*name, *cwd, *group;
	u_int			 id, flags, n, i, idx, wid, wlflags;

	upr_obj(r, &o);
	id = upr_u32(&o);
	name = upr_str(&o);
	cwd = upr_str(&o);
	for (i = 0; i < nitems(tv); i++)
		upr_time(&o, &tv[i]);
	flags = upr_u32(&o);
	oo = options_create(global_s_options);
	options_upgrade_restore(&o, oo);
	env = environ_create();
	upgrade_restore_environ(&o, env);
	group = upr_str(&o);
	if (o.error || name == NULL || session_find(name) != NULL ||
	    session_find_by_id(id) != NULL) {
		free(name);
		free(cwd);
		free(group);
		options_free(oo);
		environ_free(env);
		r->error = 1;
		return;
	}

	next_session_id = id;
	s = session_create(NULL, name, cwd == NULL ? "" : cwd, env, oo, NULL);
	free(name);
	free(cwd);
	memcpy(&s->creation_time, &tv[0], sizeof s->creation_time);
	memcpy(&s->last_attached_time, &tv[1], sizeof s->last_attached_time);
	memcpy(&s->activity_time, &tv[2], sizeof s->activity_time);
	memcpy(&s->last_activity_time, &tv[3], sizeof s->last_activity_time);
	s->flags = flags;
	if (group != NULL) {
		if ((sg = session_group_find(group)) == NULL)
			sg = session_group_new(group);
		session_group_add(sg, s);
		free(group);
	}

	n = upr_u32(&o);
	for (i = 0; i < n && !o.error; i++) {
		idx = upr_u32(&o);
		wid = upr_u32(&o);
		wlflags = upr_u32(&o);
		w = window_find_by_id(wid);
		if (w == NULL || (wl = winlink_add(&s->windows, idx)) == NULL) {
			o.error = 1;
			break;
		}
		wl->session = s;
		winlink_set_window(wl, w);
		wl->flags = wlflags & ~WINLINK_VISITED;
	}
	idx = upr_u32(&o);
	s->curw = winlink_find_by_index(&s->windows, idx);
	if (s->curw == NULL)
		s->curw = RB_MIN(winlinks, &s->windows);
	n = upr_u32(&o);
	for (i = 0; i < n && !o.error; i++) {
		wl = winlink_find_by_index(&s->windows, upr_u32(&o));
		if (wl != NULL && wl != s->curw &&
		    (~wl->flags & WINLINK_VISITED)) {
			TAILQ_INSERT_TAIL(&s->lastw, wl, sentry);
			wl->flags |= WINLINK_VISITED;
		}
	}
	if (o.error || s->curw == NULL)
		r->error = 1;
}

/* Clients. */

#define UPGRADE_CLIENT_FLAGS (CLIENT_UTF8|CLIENT_CONTROL|		\
	CLIENT_CONTROLCONTROL|CLIENT_READONLY|CLIENT_IGNORESIZE|	\
	CLIENT_FOCUSED|CLIENT_STATUSOFF|CLIENT_CONTROL_NOOUTPUT|	\
	CLIENT_DEFAULTSOCKET|CLIENT_STARTSERVER|CLIENT_NOSTARTSERVER|	\
	CLIENT_NOFORK|CLIENT_ACTIVEPANE|CLIENT_CONTROL_PAUSEAFTER|	\
	CLIENT_CONTROL_WAITEXIT|CLIENT_NO_DETACH_ON_DESTROY|CLIENT_LOGIN)

static int
#ifdef _WIN32
upgrade_save_client(struct evbuffer *evb, struct client *c, DWORD child_pid)
#else
upgrade_save_client(struct evbuffer *evb, struct client *c,
    __unused int child_pid)
#endif
{
	struct evbuffer		*o = evbuffer_new();
	u_int			 i;
#ifdef _WIN32
	WSAPROTOCOL_INFOW	 info;

	if (win32_socket_export(proc_peer_fd(c->peer), child_pid, &info) != 0) {
		evbuffer_free(o);
		return (-1);
	}
	upw_data(o, &info, sizeof info);
	upw_u32(o, c->fd != -1);
	if (c->fd != -1) {
		if (win32_socket_export(c->fd, child_pid, &info) != 0) {
			evbuffer_free(o);
			return (-1);
		}
		upw_data(o, &info, sizeof info);
	}
#else
	upw_u32(o, proc_peer_fd(c->peer));
	upgrade_keep_fd(proc_peer_fd(c->peer));
	upw_u32(o, c->fd != -1);
	if (c->fd != -1) {
		upw_u32(o, c->fd);
		upgrade_keep_fd(c->fd);
	}
	upw_u32(o, c->out_fd != -1);
	if (c->out_fd != -1) {
		upw_u32(o, c->out_fd);
		upgrade_keep_fd(c->out_fd);
	}
#endif
	upw_u64(o, c->flags & UPGRADE_CLIENT_FLAGS);
	upw_u32(o, (uint32_t)c->pid);
	upw_str(o, c->term_name);
	upw_u32(o, c->term_features);
	upw_str(o, c->term_type);
	upw_u32(o, c->term_ncaps);
	for (i = 0; i < c->term_ncaps; i++)
		upw_str(o, c->term_caps[i]);
	upw_str(o, c->ttyname);
	upw_str(o, c->cwd);
	upgrade_save_environ(o, c->environ);
	upw_time(o, &c->creation_time);
	upw_time(o, &c->activity_time);
	upw_u32(o, c->tty.sx);
	upw_u32(o, c->tty.sy);
	upw_u32(o, c->tty.xpixel);
	upw_u32(o, c->tty.ypixel);
	upw_u32(o, c->theme);
	upw_u32(o, c->session == NULL ? UINT32_MAX : c->session->id);
	upw_u32(o, c->last_session == NULL ? UINT32_MAX :
	    c->last_session->id);
	upw_u32(o, c->pause_age);
	upw_u32(o, (c->flags & CLIENT_CONTROL) != 0);
	if (c->flags & CLIENT_CONTROL)
		control_upgrade_save(o, c);
	/*
	 * The colours the terminal reported, which a pane asking for its own
	 * (OSC 10 and 11) is told; written one up so an older writer's zero
	 * reads as unknown.
	 */
	upw_u32(o, (uint32_t)(c->tty.fg + 1));
	upw_u32(o, (uint32_t)(c->tty.bg + 1));
	upw_obj(evb, o);
	evbuffer_free(o);
	return (0);
}

/*
 * A client keeps its connection, so it only sees its screen drawn again. Its
 * session is set without a hook running: nothing has attached.
 */
static void
upgrade_restore_client(struct upgrade_reader *r)
{
	struct upgrade_reader	 o;
	struct client		*c;
	struct session		*s;
#ifdef _WIN32
	const void		*info;
	size_t			 size;
#endif
	int			 fd;
	u_int			 i, sx, sy, xpixel, ypixel, sid, lsid;
	uint64_t		 flags;
	char			*cause;

	upr_obj(r, &o);
#ifdef _WIN32
	info = upr_data(&o, &size);
	if (o.error || size != sizeof (WSAPROTOCOL_INFOW) ||
	    (fd = win32_socket_import(info)) == -1) {
		r->error = 1;
		return;
	}
	c = server_client_create(fd);
	if (upr_u32(&o)) {
		info = upr_data(&o, &size);
		if (size != sizeof (WSAPROTOCOL_INFOW) ||
		    (c->fd = win32_socket_import(info)) == -1) {
			r->error = 1;
			return;
		}
	}
#else
	fd = (int)upr_u32(&o);
	if (o.error) {
		r->error = 1;
		return;
	}
	c = server_client_create(fd);
	if (upr_u32(&o))
		c->fd = (int)upr_u32(&o);
	if (upr_u32(&o))
		c->out_fd = (int)upr_u32(&o);
#endif
	flags = upr_u64(&o);
	c->flags |= flags | CLIENT_IDENTIFIED;
	c->pid = (pid_t)upr_u32(&o);
	c->term_name = upr_str(&o);
	c->term_features = upr_u32(&o);
	c->term_type = upr_str(&o);
	c->term_ncaps = upr_u32(&o);
	if (c->term_ncaps > o.left) {
		r->error = 1;
		c->term_ncaps = 0;
		return;
	}
	if (c->term_ncaps != 0)
		c->term_caps = xcalloc(c->term_ncaps, sizeof *c->term_caps);
	for (i = 0; i < c->term_ncaps; i++) {
		c->term_caps[i] = upr_str(&o);
		if (c->term_caps[i] == NULL)
			c->term_caps[i] = xstrdup("");
	}
	c->ttyname = upr_str(&o);
	c->cwd = upr_str(&o);
	upgrade_restore_environ(&o, c->environ);
	upr_time(&o, &c->creation_time);
	upr_time(&o, &c->activity_time);
	sx = upr_u32(&o);
	sy = upr_u32(&o);
	xpixel = upr_u32(&o);
	ypixel = upr_u32(&o);
	c->theme = upr_u32(&o);
	sid = upr_u32(&o);
	lsid = upr_u32(&o);
	c->pause_age = upr_u32(&o);
	if (c->term_name == NULL)
		c->term_name = xstrdup("unknown");
	if (c->ttyname != NULL && *c->ttyname != '\0')
		c->name = xstrdup(c->ttyname);
	else
		xasprintf((char **)&c->name, "client-%ld", (long)c->pid);
	if (c->cwd == NULL)
		c->cwd = xstrdup(find_home() == NULL ? "/" : find_home());

	if (upr_u32(&o)) {
		control_start(c);
		control_upgrade_restore(&o, c);
	} else if (c->fd != -1) {
		if (tty_init(&c->tty, c) != 0) {
			r->error = 1;
			return;
		}
		tty_set_size(&c->tty, sx, sy, xpixel, ypixel);
		c->tty.flags |= TTY_INHERITED;
		c->flags |= CLIENT_TERMINAL;
	}
	c->tty.fg = (int)upr_u32(&o) - 1;
	c->tty.bg = (int)upr_u32(&o) - 1;
	if (o.error) {
		r->error = 1;
		return;
	}

	s = session_find_by_id(sid);
	if (s == NULL)
		return;
	if (server_client_open(c, &cause) != 0) {
		log_debug("%s: %s", __func__, cause);
		free(cause);
		r->error = 1;
		return;
	}
	server_client_set_session(c, s);
	if (~flags & CLIENT_FOCUSED)
		c->flags &= ~CLIENT_FOCUSED;	/* set_session assumes focus */
	c->last_session = session_find_by_id(lsid);
	server_client_set_key_table(c, NULL);
	c->flags |= CLIENT_ATTACHED;
	if (c->flags & CLIENT_CONTROL)
		control_ready(c);
}

/* Waiting for a quiet moment. */

/*
 * What would be lost or cut off if the server changed hands now. Each is
 * short-lived or ends when the person at the screen finishes with it.
 */
static char *
upgrade_busy(void)
{
	struct client		*c;
	struct window_pane	*wp;
	char			*reason = NULL;

#ifdef _WIN32
	if (!win32_pty_can_export())
		return (xstrdup("panes use Windows' own console, not conpty.dll"));
#endif
	if (job_still_running())
		return (xstrdup("a command started by tmux is still running"));
	if (server_pending_ttys() != 0)
		return (xstrdup("a client is connecting"));
	TAILQ_FOREACH(c, &clients, entry) {
		if (c->flags & (CLIENT_DEAD|CLIENT_EXIT|CLIENT_SUSPENDED))
			xasprintf(&reason, "client %s is closing", c->name);
		else if (~c->flags & CLIENT_ATTACHED)
			xasprintf(&reason, "a tmux command is running");
		else if (c->tty_token != NULL)
			xasprintf(&reason, "client %s is connecting", c->name);
		else if (c->overlay_draw != NULL)
			xasprintf(&reason, "a popup or menu is open");
		else if (c->prompt_string != NULL)
			xasprintf(&reason, "a prompt is open");
		else if (c->keytable != NULL &&
		    strcmp(c->keytable->name, "root") != 0)
			xasprintf(&reason, "key table %s is in use",
			    c->keytable->name);
		else if (!RB_EMPTY(&c->files))
			xasprintf(&reason, "client %s is reading a file",
			    c->name);
		else if (proc_peer_read_pending(c->peer) ||
		    ((c->flags & CLIENT_TERMINAL) && c->tty.in != NULL &&
		    EVBUFFER_LENGTH(c->tty.in) != 0))
			xasprintf(&reason, "client %s is sending keys",
			    c->name);
		else if ((c->flags & CLIENT_CONTROL) && !control_all_done(c))
			xasprintf(&reason, "client %s has output waiting",
			    c->name);
		if (reason != NULL)
			return (reason);
	}
	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (!TAILQ_EMPTY(&wp->modes)) {
			xasprintf(&reason, "pane %%%u is in %s", wp->id,
			    TAILQ_FIRST(&wp->modes)->mode->name);
		} else if (wp->pipe_fd != -1)
			xasprintf(&reason, "pane %%%u is piped", wp->id);
		else if (wp->draw_wait != NULL)
			xasprintf(&reason, "pane %%%u is waiting to draw",
			    wp->id);
#ifdef _WIN32
		else if (wp->fd != -1 && wp->win32_pty == NULL)
			xasprintf(&reason, "pane %%%u has no console", wp->id);
#endif
		if (reason != NULL)
			return (reason);
	}
	return (NULL);
}

/* Write all of an evbuffer to a non-blocking socket. */
static int
upgrade_send_all(int fd, struct evbuffer *evb)
{
	uint64_t	start = get_timer();
	int		n;

	while (EVBUFFER_LENGTH(evb) != 0) {
#ifdef _WIN32
		n = send((SOCKET)fd, (const char *)EVBUFFER_DATA(evb),
		    (int)EVBUFFER_LENGTH(evb), 0);
#else
		n = write(fd, EVBUFFER_DATA(evb), EVBUFFER_LENGTH(evb));
#endif
		if (n > 0) {
			evbuffer_drain(evb, n);
			continue;
		}
#ifdef _WIN32
		if (n == 0 || WSAGetLastError() != WSAEWOULDBLOCK ||
		    get_timer() - start > UPGRADE_FLUSH_TIMEOUT)
			return (-1);
		Sleep(1);
#else
		if (n == 0 || (errno != EAGAIN && errno != EINTR) ||
		    get_timer() - start > UPGRADE_FLUSH_TIMEOUT)
			return (-1);
		usleep(1000);
#endif
	}
	return (0);
}

/* Send each client everything queued for it. */
static int
upgrade_flush_clients(void)
{
	struct client		*c;
	struct evbuffer		*out;

	TAILQ_FOREACH(c, &clients, entry) {
		proc_flush_peer(c->peer);
		if (c->flags & CLIENT_CONTROL)
			out = control_upgrade_output(c);
		else if (c->flags & CLIENT_TERMINAL)
			out = c->tty.out;
		else
			continue;
		if (out != NULL && upgrade_send_all(c->fd, out) != 0)
			return (-1);
	}
	return (0);
}

#ifdef _WIN32
/*
 * Stop reading every console and read what is left in its bridge, so the new
 * server starts at the next byte. Replies the parser makes go straight to the
 * console, behind what was already queued.
 */
static void
upgrade_stop_panes(void)
{
	struct window_pane	*wp;
	struct evbuffer		*evb, *out;

	evb = evbuffer_new();
	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->win32_pty == NULL)
			continue;
		bufferevent_disable(wp->event, EV_READ|EV_WRITE);
		out = bufferevent_get_output(wp->event);
		upgrade_send_all(wp->fd, out);
		win32_pty_stop(wp->win32_pty, evb);
		log_debug("upgrade: %%%u stopped with %zu bytes left", wp->id,
		    EVBUFFER_LENGTH(evb));
		window_pane_upgrade_feed(wp, evb);
		evbuffer_drain(evb, EVBUFFER_LENGTH(evb));
		if (EVBUFFER_LENGTH(out) != 0) {
			win32_pty_write(wp->win32_pty, EVBUFFER_DATA(out),
			    EVBUFFER_LENGTH(out));
			evbuffer_drain(out, EVBUFFER_LENGTH(out));
		}
		wp->fd = -1;
	}
	evbuffer_free(evb);
}

/* Read the consoles again after a handover that did not happen. */
static void
upgrade_resume_panes(void)
{
	struct window_pane	*wp;

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->win32_pty == NULL || wp->fd != -1)
			continue;
		bufferevent_free(wp->event);
		wp->event = NULL;
		wp->fd = win32_pty_resume(wp->win32_pty);
		if (wp->fd == -1) {
			wp->flags |= PANE_EXITED;
			continue;
		}
		window_pane_upgrade_event(wp);
	}
}
#else
/*
 * Stop reading every pane and parse what was read, so the new server starts
 * at the next byte in the pty; what was waiting to go to a pane goes first.
 */
static void
upgrade_stop_panes(void)
{
	struct window_pane	*wp;
	struct evbuffer		*evb;

	evb = evbuffer_new();
	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->fd == -1 || wp->event == NULL)
			continue;
		bufferevent_disable(wp->event, EV_READ|EV_WRITE);
		upgrade_send_all(wp->fd, bufferevent_get_output(wp->event));
		evbuffer_add_buffer(evb, bufferevent_get_input(wp->event));
		window_pane_upgrade_feed(wp, evb);
		evbuffer_drain(evb, EVBUFFER_LENGTH(evb));
		upgrade_send_all(wp->fd, bufferevent_get_output(wp->event));
	}
	evbuffer_free(evb);
}

/* Read the panes again after a handover that did not happen. */
static void
upgrade_resume_panes(void)
{
	struct window_pane	*wp;

	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->fd != -1 && wp->event != NULL)
			bufferevent_enable(wp->event, EV_READ|EV_WRITE);
	}
}
#endif

/* Build the state for the new server, with handles that are its own. */
static int
#ifdef _WIN32
upgrade_save(struct evbuffer *evb, HANDLE child, DWORD child_pid,
    char **cause)
#else
upgrade_save(struct evbuffer *evb, int child, int child_pid, char **cause)
#endif
{
	struct evbuffer		*o;
	struct window		*w;
	struct session		*s;
	struct client		*c;
#ifdef _WIN32
	WSAPROTOCOL_INFOW	 info;
	uint64_t		 pipe;
	uint16_t		 port;
	char			*nonces;
#endif
	u_int			 n, next_window, next_pane;

	upw_str(evb, UPGRADE_MAGIC);
	upw_u32(evb, UPGRADE_VERSION);
	upw_u32(evb, PROTOCOL_VERSION);

	o = evbuffer_new();
#ifdef _WIN32
	if (win32_ipc_export(child, &pipe, &port, &nonces) != 0 ||
	    win32_socket_export(server_listen_fd(), child_pid, &info) != 0) {
		evbuffer_free(o);
		*cause = xstrdup("can't hand over the server's socket");
		return (-1);
	}
	log_debug("upgrade: socket and pipe handed over");
	upw_u64(o, pipe);
	upw_u32(o, port);
	upw_str(o, nonces);
	free(nonces);
	upw_data(o, &info, sizeof info);
#else
	upw_u32(o, server_listen_fd());
	upgrade_keep_fd(server_listen_fd());
#endif
	upw_obj(evb, o);

	window_get_next_ids(&next_window, &next_pane);
	upw_time(o, &start_time);
	upw_u32(o, next_session_id);
	upw_u32(o, next_window);
	upw_u32(o, next_pane);
	upw_obj(evb, o);
	evbuffer_free(o);

	options_upgrade_save(evb, global_options);
	options_upgrade_save(evb, global_s_options);
	options_upgrade_save(evb, global_w_options);
	upgrade_save_environ(evb, global_environ);
	upgrade_save_keys(evb);
	paste_upgrade_save(evb);

	n = 0;
	RB_FOREACH(w, windows, &windows)
		n++;
	upw_u32(evb, n);
	RB_FOREACH(w, windows, &windows) {
		if (upgrade_save_window(evb, w, child) != 0) {
			*cause = xstrdup("can't hand over a pane's console");
			return (-1);
		}
	}

	n = 0;
	RB_FOREACH(s, sessions, &sessions)
		n++;
	upw_u32(evb, n);
	RB_FOREACH(s, sessions, &sessions)
		upgrade_save_session(evb, s);

	n = 0;
	TAILQ_FOREACH(c, &clients, entry)
		n++;
	upw_u32(evb, n);
	TAILQ_FOREACH(c, &clients, entry) {
		if (upgrade_save_client(evb, c, child_pid) != 0) {
			*cause = xstrdup("can't hand over a client");
			return (-1);
		}
	}
	return (0);
}

#ifdef _WIN32
/* Start the new build, with the two pipes it talks to us on. */
static int
upgrade_spawn(HANDLE state, HANDLE ack, PROCESS_INFORMATION *pi,
    char **cause)
{
	STARTUPINFOEXA	 si;
	SIZE_T		 size = 0;
	HANDLE		 inherit[2] = { state, ack };
	DWORD		 flags;
	char		 env[64], *cmdline;
	int		 ok;

	snprintf(env, sizeof env, "%llu,%llu",
	    (unsigned long long)(uintptr_t)state,
	    (unsigned long long)(uintptr_t)ack);
	xasprintf(&cmdline, "\"%s\" %s%s-D -S \"%s\"", upgrade_exe,
	    log_get_level() > 0 ? "-v " : "", log_get_level() > 1 ? "-v " : "",
	    socket_path);

	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	InitializeProcThreadAttributeList(NULL, 1, 0, &size);
	si.lpAttributeList = xmalloc(size);
	InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size);
	UpdateProcThreadAttribute(si.lpAttributeList, 0,
	    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof inherit, NULL,
	    NULL);

	SetEnvironmentVariableA("TMUX_UPGRADE", env);
	flags = DETACHED_PROCESS|CREATE_NO_WINDOW|EXTENDED_STARTUPINFO_PRESENT;
	ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
	    flags|CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si.StartupInfo, pi);
	if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
		ok = CreateProcessA(NULL, cmdline, NULL, NULL, TRUE, flags,
		    NULL, NULL, &si.StartupInfo, pi);
	}
	if (!ok)
		xasprintf(cause, "can't start %s (error %lu)", upgrade_exe,
		    GetLastError());
	SetEnvironmentVariableA("TMUX_UPGRADE", NULL);

	DeleteProcThreadAttributeList(si.lpAttributeList);
	free(si.lpAttributeList);
	free(cmdline);
	return (ok ? 0 : -1);
}

static int
upgrade_write_all(HANDLE h, const void *data, size_t size)
{
	const char	*p = data;
	DWORD		 n;

	while (size != 0) {
		if (!WriteFile(h, p, size > 65536 ? 65536 : (DWORD)size, &n,
		    NULL))
			return (-1);
		p += n;
		size -= n;
	}
	return (0);
}

/*
 * The state goes from a thread so a new server that stops reading can't hold
 * this one up: the wait for its answer times out and ends it.
 */
struct upgrade_write {
	HANDLE		 h;
	const void	*data;
	uint64_t	 size;
};

static DWORD WINAPI
upgrade_writer(LPVOID arg)
{
	struct upgrade_write	*uw = arg;

	if (upgrade_write_all(uw->h, &uw->size, sizeof uw->size) == 0)
		upgrade_write_all(uw->h, uw->data, (size_t)uw->size);
	return (0);
}

/* Wait for the new server's line: "ok" once it holds everything. */
static int
upgrade_wait_ack(HANDLE ack, HANDLE child, char **cause)
{
	uint64_t	 start = get_timer();
	char		 line[512];
	size_t		 len = 0;
	DWORD		 avail, n;

	for (;;) {
		if (PeekNamedPipe(ack, NULL, 0, NULL, &avail, NULL) &&
		    avail != 0) {
			if (!ReadFile(ack, line + len, 1, &n, NULL) || n == 0)
				break;
			if (line[len] == '\n') {
				line[len] = '\0';
				if (strcmp(line, "ok") == 0)
					return (0);
				*cause = xstrdup(line);
				return (-1);
			}
			if (++len == sizeof line - 1)
				break;
			continue;
		}
		if (WaitForSingleObject(child, 10) == WAIT_OBJECT_0) {
			*cause = xstrdup("the new server exited");
			return (-1);
		}
		if (get_timer() - start > UPGRADE_ACK_TIMEOUT) {
			*cause = xstrdup("the new server did not answer");
			return (-1);
		}
	}
	*cause = xstrdup("lost the new server");
	return (-1);
}

static int
upgrade_run(char **cause)
{
	SECURITY_ATTRIBUTES	 sa = { sizeof sa, NULL, TRUE };
	PROCESS_INFORMATION	 pi;
	HANDLE			 state_r, state_w, ack_r, ack_w, writer;
	struct evbuffer		*evb;
	struct client		*c;
	struct upgrade_write	 uw;

	if (upgrade_flush_clients() != 0) {
		*cause = xstrdup("a client is not reading");
		return (-1);
	}
	status_prompt_save_history();

	if (!CreatePipe(&state_r, &state_w, &sa, 0)) {
		*cause = xstrdup("can't create a pipe");
		return (-1);
	}
	if (!CreatePipe(&ack_r, &ack_w, &sa, 0)) {
		CloseHandle(state_r);
		CloseHandle(state_w);
		*cause = xstrdup("can't create a pipe");
		return (-1);
	}
	SetHandleInformation(state_w, HANDLE_FLAG_INHERIT, 0);
	SetHandleInformation(ack_r, HANDLE_FLAG_INHERIT, 0);

	if (upgrade_spawn(state_r, ack_w, &pi, cause) != 0) {
		CloseHandle(state_r);
		CloseHandle(state_w);
		CloseHandle(ack_r);
		CloseHandle(ack_w);
		return (-1);
	}
	CloseHandle(state_r);
	CloseHandle(ack_w);

	log_debug("upgrade: started %s (pid %lu)", upgrade_exe, pi.dwProcessId);
	upgrade_stop_panes();
	evb = evbuffer_new();
	writer = NULL;
	if (upgrade_save(evb, pi.hProcess, pi.dwProcessId, cause) == 0) {
		uw.h = state_w;
		uw.size = EVBUFFER_LENGTH(evb);
		uw.data = EVBUFFER_DATA(evb);
		writer = CreateThread(NULL, 0, upgrade_writer, &uw, 0, NULL);
		if (writer == NULL)
			*cause = xstrdup("can't send the new server its state");
	}
	log_debug("upgrade: state is %zu bytes", EVBUFFER_LENGTH(evb));

	if (*cause == NULL && upgrade_wait_ack(ack_r, pi.hProcess, cause) == 0) {
		/*
		 * The new server holds every console and socket now. Nothing
		 * here may close one on the way out, so no exit handlers run.
		 */
		log_debug("upgrade: handed over to %s (pid %lu)", upgrade_exe,
		    pi.dwProcessId);
		TerminateProcess(GetCurrentProcess(), 0);
	}

	TerminateProcess(pi.hProcess, 1);
	WaitForSingleObject(pi.hProcess, 5000);
	if (writer != NULL) {
		WaitForSingleObject(writer, INFINITE);
		CloseHandle(writer);
	}
	CloseHandle(state_w);
	evbuffer_free(evb);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(ack_r);
	upgrade_resume_panes();
	win32_ipc_resume();
	TAILQ_FOREACH(c, &clients, entry)
		server_redraw_client(c);
	return (-1);
}
#else
/*
 * Start the new build as a child of this server, which it outlives. It keeps
 * every descriptor handed over at the same number, and the state pipe and
 * the answer pipe besides.
 */
static pid_t
upgrade_spawn(int state, int ack, char **cause)
{
	char	 env[64], *argv[6];
	pid_t	 pid;
	u_int	 i;
	int	 argc = 0;

	argv[argc++] = upgrade_exe;
	if (log_get_level() > 0)
		argv[argc++] = (char *)"-v";
	if (log_get_level() > 1)
		argv[argc++] = (char *)"-v";
	argv[argc++] = (char *)"-D";
	argv[argc++] = (char *)"-S";
	argv[argc++] = (char *)socket_path;
	argv[argc] = NULL;

	switch (pid = fork()) {
	case -1:
		xasprintf(cause, "can't fork: %s", strerror(errno));
		return (-1);
	case 0:
		for (i = 0; i < upgrade_nfds; i++)
			fcntl(upgrade_fds[i], F_SETFD, 0);
		fcntl(state, F_SETFD, 0);
		fcntl(ack, F_SETFD, 0);
		snprintf(env, sizeof env, "%d,%d", state, ack);
		setenv("TMUX_UPGRADE", env, 1);
		setsid();
		execv(upgrade_exe, argv);
		_exit(127);
	}
	return (pid);
}

static int
upgrade_write_all(int fd, const void *data, size_t size)
{
	const char	*p = data;
	ssize_t		 n;

	while (size != 0) {
		n = write(fd, p, size);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			return (-1);
		p += n;
		size -= n;
	}
	return (0);
}

/* Wait for the new server's line: "ok" once it holds everything. */
static int
upgrade_wait_ack(int ack, char **cause)
{
	uint64_t	 start = get_timer();
	struct pollfd	 pfd = { .fd = ack, .events = POLLIN };
	char		 line[512];
	size_t		 len = 0;
	ssize_t		 n;

	for (;;) {
		if (get_timer() - start > UPGRADE_ACK_TIMEOUT) {
			*cause = xstrdup("the new server did not answer");
			return (-1);
		}
		if (poll(&pfd, 1, 100) <= 0)
			continue;
		n = read(ack, line + len, 1);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0) {
			*cause = xstrdup("the new server exited");
			return (-1);
		}
		if (line[len] == '\n') {
			line[len] = '\0';
			if (strcmp(line, "ok") == 0)
				return (0);
			*cause = xstrdup(line);
			return (-1);
		}
		if (++len == sizeof line - 1) {
			*cause = xstrdup("lost the new server");
			return (-1);
		}
	}
}

static int
upgrade_run(char **cause)
{
	struct evbuffer	*evb;
	struct client	*c;
	int		 state[2], ack[2];
	pid_t		 pid;
	uint64_t	 size;

	if (upgrade_flush_clients() != 0) {
		*cause = xstrdup("a client is not reading");
		return (-1);
	}
	status_prompt_save_history();

	free(upgrade_fds);
	upgrade_fds = NULL;
	upgrade_nfds = 0;
	upgrade_stop_panes();
	evb = evbuffer_new();
	if (upgrade_save(evb, -1, -1, cause) != 0)
		goto resume;
	log_debug("upgrade: state is %zu bytes", EVBUFFER_LENGTH(evb));

	if (pipe(state) != 0) {
		*cause = xstrdup("can't create a pipe");
		goto resume;
	}
	if (pipe(ack) != 0) {
		close(state[0]);
		close(state[1]);
		*cause = xstrdup("can't create a pipe");
		goto resume;
	}
	fcntl(state[1], F_SETFD, FD_CLOEXEC);
	fcntl(ack[0], F_SETFD, FD_CLOEXEC);
	pid = upgrade_spawn(state[0], ack[1], cause);
	close(state[0]);
	close(ack[1]);
	if (pid == -1) {
		close(state[1]);
		close(ack[0]);
		goto resume;
	}
	log_debug("upgrade: started %s (pid %ld)", upgrade_exe, (long)pid);

	size = EVBUFFER_LENGTH(evb);
	if (upgrade_write_all(state[1], &size, sizeof size) != 0 ||
	    upgrade_write_all(state[1], EVBUFFER_DATA(evb), size) != 0)
		*cause = xstrdup("can't send the new server its state");
	close(state[1]);

	if (*cause == NULL && upgrade_wait_ack(ack[0], cause) == 0) {
		/*
		 * The new server holds every pty and socket now. Nothing here
		 * may close one on the way out, so no exit handlers run.
		 */
		log_debug("upgrade: handed over to %s (pid %ld)", upgrade_exe,
		    (long)pid);
		_exit(0);
	}
	kill(pid, SIGKILL);
	close(ack[0]);

resume:
	evbuffer_free(evb);
	upgrade_resume_panes();
	TAILQ_FOREACH(c, &clients, entry)
		server_redraw_client(c);
	return (-1);
}
#endif

static void
upgrade_timer_callback(__unused int fd, __unused short events,
    __unused void *data)
{
	struct timeval	 tv = { .tv_sec = 1 };
	char		*reason, *cause = NULL;

	if (upgrade_exe == NULL)
		return;
	if ((reason = upgrade_busy()) != NULL) {
		upgrade_set_status("waiting: %s", reason);
		free(reason);
		evtimer_add(&upgrade_timer, &tv);
		return;
	}
	if (upgrade_run(&cause) != 0) {
		upgrade_set_status("failed: %s", cause);
		server_add_message("upgrade to %s failed: %s", upgrade_exe,
		    cause);
		free(cause);
	}
	free(upgrade_exe);
	upgrade_exe = NULL;
}

/* Ask for the server to move to another build once it is quiet. */
int
server_upgrade_request(const char *exe, char **cause)
{
	struct timeval	tv = { 0 };
#ifdef _WIN32
	DWORD		attr;

	attr = GetFileAttributesA(exe);
	if (attr == INVALID_FILE_ATTRIBUTES ||
	    (attr & FILE_ATTRIBUTE_DIRECTORY)) {
		xasprintf(cause, "no such file: %s", exe);
		return (-1);
	}
#else
	if (*exe != '/' || access(exe, X_OK) != 0) {
		xasprintf(cause, "no such file: %s", exe);
		return (-1);
	}
#endif
	free(upgrade_exe);
	upgrade_exe = xstrdup(exe);
	upgrade_set_status("waiting");
	if (!event_initialized(&upgrade_timer))
		evtimer_set(&upgrade_timer, upgrade_timer_callback, NULL);
	evtimer_add(&upgrade_timer, &tv);
	return (0);
}

/* "", "waiting[: reason]" or "failed: reason". */
const char *
server_upgrade_status(void)
{
	return (upgrade_status == NULL ? "" : upgrade_status);
}

/* The new server's side. */

#ifdef _WIN32
typedef HANDLE upgrade_pipe;
#else
typedef int upgrade_pipe;
#endif

static void
upgrade_answer(upgrade_pipe ack, const char *line)
{
	upgrade_write_all(ack, line, strlen(line));
	upgrade_write_all(ack, "\n", 1);
}

#ifdef _WIN32
static int
upgrade_read_state(HANDLE state, u_char **data, size_t *size)
{
	uint64_t	 n;
	size_t		 done = 0;
	DWORD		 got;

	if (!ReadFile(state, &n, sizeof n, &got, NULL) || got != sizeof n ||
	    n > (uint64_t)SIZE_MAX)
		return (-1);
	*data = xmalloc(n == 0 ? 1 : (size_t)n);
	while (done < n) {
		if (!ReadFile(state, *data + done, (DWORD)(n - done > 65536 ?
		    65536 : n - done), &got, NULL) || got == 0) {
			free(*data);
			return (-1);
		}
		done += got;
	}
	*size = (size_t)n;
	return (0);
}

#else
static int
upgrade_read_all(int fd, void *data, size_t size)
{
	char	*p = data;
	ssize_t	 n;

	while (size != 0) {
		n = read(fd, p, size);
		if (n == -1 && errno == EINTR)
			continue;
		if (n <= 0)
			return (-1);
		p += n;
		size -= n;
	}
	return (0);
}

static int
upgrade_read_state(int state, u_char **data, size_t *size)
{
	uint64_t	n;

	if (upgrade_read_all(state, &n, sizeof n) != 0 ||
	    n > (uint64_t)SIZE_MAX)
		return (-1);
	*data = xmalloc(n == 0 ? 1 : (size_t)n);
	if (upgrade_read_all(state, *data, (size_t)n) != 0) {
		free(*data);
		return (-1);
	}
	*size = (size_t)n;
	return (0);
}
#endif

/*
 * Rebuild the server handed over in TMUX_UPGRADE and answer. Returns the
 * listening socket, or exits if this build can't take it over, leaving the
 * old server to carry on. Nothing handed over is read or written until the
 * old server is told: it may still need them.
 */
int
server_upgrade_restore(void)
{
	struct upgrade_reader	 r, o;
	struct window		*w;
	struct window_pane	*wp;
	struct client		*c;
	upgrade_pipe		 state, ack;
	u_char			*data;
	size_t			 size;
	char			*magic, *cause = NULL;
	uint32_t		 version;
#ifdef _WIN32
	unsigned long long	 hstate, hack;
	const void		*info;
	char			*nonces = NULL;
	uint64_t		 pipe;
	uint32_t		 port;
#else
	int			 istate, iack;
#endif
	u_int			 n, i, next_session, next_window, next_pane;
	int			 fd = -1;

#ifdef _WIN32
	if (sscanf(getenv("TMUX_UPGRADE"), "%llu,%llu", &hstate, &hack) != 2)
		exit(1);
	state = (HANDLE)(uintptr_t)hstate;
	ack = (HANDLE)(uintptr_t)hack;
	_putenv("TMUX_UPGRADE=");
#else
	if (sscanf(getenv("TMUX_UPGRADE"), "%d,%d", &istate, &iack) != 2)
		exit(1);
	state = istate;
	ack = iack;
	unsetenv("TMUX_UPGRADE");
#endif

	if (upgrade_read_state(state, &data, &size) != 0) {
		upgrade_answer(ack, "can't read the state");
		exit(1);
	}
#ifdef _WIN32
	CloseHandle(state);
#else
	close(state);
#endif
	r.p = data;
	r.left = size;
	r.error = 0;

	magic = upr_str(&r);
	version = upr_u32(&r);
	if (magic == NULL || strcmp(magic, UPGRADE_MAGIC) != 0 ||
	    version != UPGRADE_VERSION) {
		xasprintf(&cause, "state version %u is not %u", version,
		    UPGRADE_VERSION);
		goto fail;
	}
	free(magic);

	/* Clients keep talking as they did, so they must be understood. */
	version = upr_u32(&r);
	if (version != PROTOCOL_VERSION) {
		xasprintf(&cause, "clients use protocol %u, not %u", version,
		    PROTOCOL_VERSION);
		goto fail;
	}

	server_upgrading = 1;

	/* This build's default key bindings are queued; set them first. */
	while (cmdq_next(NULL) != 0)
		/* nothing */;

	upr_obj(&r, &o);
#ifdef _WIN32
	pipe = upr_u64(&o);
	port = upr_u32(&o);
	nonces = upr_str(&o);
	info = upr_data(&o, &size);
	if (o.error || size != sizeof (WSAPROTOCOL_INFOW) ||
	    (fd = win32_socket_import(info)) == -1) {
		cause = xstrdup("can't take over the server's socket");
		goto fail;
	}
#else
	fd = (int)upr_u32(&o);
	if (o.error) {
		cause = xstrdup("can't take over the server's socket");
		goto fail;
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif

	upr_obj(&r, &o);
	upr_time(&o, &start_time);
	next_session = upr_u32(&o);
	next_window = upr_u32(&o);
	next_pane = upr_u32(&o);

	options_upgrade_restore(&r, global_options);
	options_upgrade_restore(&r, global_s_options);
	options_upgrade_restore(&r, global_w_options);
	/*
	 * The old server's global environment replaces the one built from this
	 * process's environment at startup, which holds what was removed since.
	 * TMUX_UPGRADE is in the state of a server an older build took over:
	 * panes must not inherit it.
	 */
	environ_free(global_environ);
	global_environ = environ_create();
	upgrade_restore_environ(&r, global_environ);
	environ_unset(global_environ, "TMUX_UPGRADE");
	upgrade_restore_keys(&r);
	paste_upgrade_restore(&r);
	if (r.error) {
		cause = xstrdup("can't read the options");
		goto fail;
	}

	n = upr_u32(&r);
	for (i = 0; i < n && !r.error; i++)
		upgrade_restore_window(&r);
	if (r.error) {
		cause = xstrdup("can't rebuild the windows");
		goto fail;
	}
	n = upr_u32(&r);
	for (i = 0; i < n && !r.error; i++)
		upgrade_restore_session(&r);
	if (r.error) {
		cause = xstrdup("can't rebuild the sessions");
		goto fail;
	}
	window_set_next_ids(next_window, next_pane);
	next_session_id = next_session;

	n = upr_u32(&r);
	for (i = 0; i < n && !r.error; i++)
		upgrade_restore_client(&r);
	if (r.error) {
		cause = xstrdup("can't take over the clients");
		goto fail;
	}
	free(data);
	cfg_finished = 1;
	status_prompt_load_history();

#ifdef _WIN32
	if (win32_ipc_import(socket_path, pipe, port, nonces) != 0) {
		cause = xstrdup("can't take over the discovery pipe");
		goto fail;
	}
	free(nonces);
#endif

	upgrade_answer(ack, "ok");
#ifdef _WIN32
	CloseHandle(ack);
#else
	close(ack);
#endif

	/* The old server has gone; start reading. */
#ifndef _WIN32
	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->fd == -1)
			continue;
		fcntl(wp->fd, F_SETFD, FD_CLOEXEC);
		window_pane_upgrade_event(wp);
	}
	TAILQ_FOREACH(c, &clients, entry) {
		fcntl(proc_peer_fd(c->peer), F_SETFD, FD_CLOEXEC);
		if (c->fd != -1)
			fcntl(c->fd, F_SETFD, FD_CLOEXEC);
		if (c->out_fd != -1)
			fcntl(c->out_fd, F_SETFD, FD_CLOEXEC);
	}
#else
	RB_FOREACH(wp, window_pane_tree, &all_window_panes) {
		if (wp->win32_pty == NULL)
			continue;
		wp->fd = win32_pty_resume(wp->win32_pty);
		if (wp->fd == -1) {
			wp->flags |= PANE_EXITED;
			continue;
		}
		window_pane_upgrade_event(wp);
		win32_process_watch(win32_pty_get_process(wp->win32_pty),
		    wp->pid);
	}
#endif
	recalculate_sizes();
	TAILQ_FOREACH(c, &clients, entry) {
		server_redraw_client(c);
		/* Ask each terminal again for what it reports, its colours too. */
		if (c->flags & CLIENT_TERMINAL)
			tty_send_requests(&c->tty);
	}
	server_upgrading = 0;
	session_set_changed();
	RB_FOREACH(w, windows, &windows)
		window_update_focus(w);
	log_debug("upgrade: took over from the old server");
	return (fd);

fail:
	log_debug("upgrade: %s", cause);
	upgrade_answer(ack, cause);
#ifdef _WIN32
	TerminateProcess(GetCurrentProcess(), 1);
#else
	_exit(1);
#endif
	return (-1);
}

#endif /* HAVE_SERVER_UPGRADE */
