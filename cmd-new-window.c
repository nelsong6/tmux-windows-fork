/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 *
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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "tmux.h"

/*
 * Create a new window.
 */

#define NEW_WINDOW_TEMPLATE "#{session_name}:#{window_index}.#{pane_index}"

static enum cmd_retval	cmd_new_window_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_new_window_entry = {
	.name = "new-window",
	.alias = "neww",

	.args = { "abc:de:F:kn:PSt:W:", 0, -1, NULL },
	.usage = "[-abdkPS] [-c start-directory] [-e environment] [-F format] "
		 "[-n window-name] " CMD_TARGET_WINDOW_USAGE
		 " [-W target-client] [shell-command [argument ...]]",

	.target = { 't', CMD_FIND_WINDOW, CMD_FIND_WINDOW_INDEX },

	.flags = 0,
	.exec = cmd_new_window_exec
};

/*
 * new-window -W: the window opens in the background and a client is switched
 * to it once its pane has drawn something, so the client never shows a blank
 * pane while the program starts. The command waits until then. A pane that
 * exits or is closed first fails the command and switches nothing; the
 * waiting command's client going away drops the wait. Nothing is kept once it
 * ends, and upgrade-server waits while one is pending.
 */
struct new_window_draw {
	struct cmdq_item		*item;
	struct window_pane		*wp;
	struct client			*tc;	/* referenced */
	u_int				 session;

	TAILQ_ENTRY(new_window_draw)	 entry;
};
static TAILQ_HEAD(, new_window_draw) new_window_draws =
    TAILQ_HEAD_INITIALIZER(new_window_draws);

/*
 * Has the pane drawn anything? Its visible screen has a cell that isn't a
 * space. A new console's own setup (clearing the screen, a title, moving the
 * cursor) leaves it blank.
 */
static int
new_window_drawn(struct window_pane *wp)
{
	struct grid		*gd = wp->base.grid;
	const struct grid_line	*gl;
	struct grid_cell	 gc;
	u_int			 x, y;

	for (y = gd->hsize; y < gd->hsize + gd->sy; y++) {
		gl = grid_peek_line(gd, y);
		for (x = 0; x < gl->cellused; x++) {
			grid_get_cell(gd, x, y, &gc);
			if (gc.flags & GRID_FLAG_PADDING)
				continue;
			if (gc.data.size != 1 || *gc.data.data != ' ')
				return (1);
		}
	}
	return (0);
}

static void
new_window_switch(struct client *tc, struct session *s, struct winlink *wl)
{
	session_set_current(s, wl);
	environ_update(s->options, tc->environ, s->environ);
	server_client_set_session(tc, s);
}

static void
new_window_draw_free(struct new_window_draw *nwd)
{
	nwd->wp->draw_wait = NULL;
	TAILQ_REMOVE(&new_window_draws, nwd, entry);
	server_client_unref(nwd->tc);
	free(nwd);
}

/* After a pane's output is parsed: switch the client if it has drawn. */
void
new_window_draw_check(struct window_pane *wp)
{
	struct new_window_draw	*nwd = wp->draw_wait;
	struct cmdq_item	*item;
	struct session		*s;
	struct winlink		*wl = NULL;

	if (nwd == NULL || !new_window_drawn(wp))
		return;
	item = nwd->item;

	if ((s = session_find_by_id(nwd->session)) != NULL)
		wl = winlink_find_by_window(&s->windows, wp->window);
	if (wl == NULL)
		cmdq_error(item, "window @%u left its session", wp->window->id);
	else if (nwd->tc->flags & CLIENT_DEAD)
		cmdq_error(item, "client %s has gone", nwd->tc->name);
	else
		new_window_switch(nwd->tc, s, wl);
	log_debug("%s: %%%u drew", __func__, wp->id);

	new_window_draw_free(nwd);
	cmdq_continue(item);
}

/* The pane is going before it drew: fail the command. */
void
new_window_draw_end(struct window_pane *wp, const char *why)
{
	struct new_window_draw	*nwd = wp->draw_wait;
	struct cmdq_item	*item;

	if (nwd == NULL)
		return;
	item = nwd->item;
	log_debug("%s: %%%u %s", __func__, wp->id, why);
	cmdq_error(item, "pane %%%u %s", wp->id, why);
	new_window_draw_free(nwd);
	cmdq_continue(item);
}

/*
 * A client has gone: a wait it was running is given up, and one that would
 * switch it fails.
 */
void
new_window_draw_client_lost(struct client *c)
{
	struct new_window_draw	*nwd, *nwd1;
	struct cmdq_item	*item;

	TAILQ_FOREACH_SAFE(nwd, &new_window_draws, entry, nwd1) {
		item = nwd->item;
		if (cmdq_get_client(item) == c) {
			log_debug("%s: %%%u given up", __func__, nwd->wp->id);
			new_window_draw_free(nwd);
			cmdq_continue(item);
		} else if (nwd->tc == c) {
			cmdq_error(item, "client %s has gone", c->name);
			new_window_draw_free(nwd);
			cmdq_continue(item);
		}
	}
}

static enum cmd_retval
cmd_new_window_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct client		*c = cmdq_get_client(item);
	struct cmd_find_state	*current = cmdq_get_current(item);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct spawn_context	 sc = { 0 };
	struct client		*tc = cmdq_get_target_client(item);
	struct session		*s = target->s;
	struct winlink		*wl = target->wl, *new_wl = NULL;
	int			 idx = target->idx, before;
	char			*cause = NULL, *cp, *expanded;
	const char		*template, *name;
	struct cmd_find_state	 fs;
	struct args_value	*av;
	struct client		*wc = NULL;
	struct new_window_draw	*nwd;
	int			 detached;

	if (args_has(args, 'W')) {
		wc = cmd_find_client(item, args_get(args, 'W'), 0);
		if (wc == NULL)
			return (CMD_RETURN_ERROR);
		if (wc->session == NULL) {
			cmdq_error(item, "client %s is not attached", wc->name);
			return (CMD_RETURN_ERROR);
		}
	}
	detached = args_has(args, 'd') || wc != NULL;

	/*
	 * If -S and -n are given and -t is not and a single window with this
	 * name already exists, select it.
	 */
	name = args_get(args, 'n');
	if (args_has(args, 'S') && name != NULL && target->idx == -1) {
		expanded = format_single(item, name, c, s, NULL, NULL);
		RB_FOREACH(wl, winlinks, &s->windows) {
			if (strcmp(wl->window->name, expanded) != 0)
				continue;
			if (new_wl == NULL) {
				new_wl = wl;
				continue;
			}
			cmdq_error(item, "multiple windows named %s", name);
			free(expanded);
			return (CMD_RETURN_ERROR);
		}
		free(expanded);
		if (new_wl != NULL) {
			if (wc != NULL) {
				new_window_switch(wc, s, new_wl);
				return (CMD_RETURN_NORMAL);
			}
			if (args_has(args, 'd'))
				return (CMD_RETURN_NORMAL);
			if (session_set_current(s, new_wl) == 0)
				server_redraw_session(s);
			if (c != NULL && c->session != NULL)
				s->curw->window->latest = c;
			recalculate_sizes();
			return (CMD_RETURN_NORMAL);
		}
	}

	before = args_has(args, 'b');
	if (args_has(args, 'a') || before) {
		idx = winlink_shuffle_up(s, wl, before);
		if (idx == -1)
			idx = target->idx;
	}

	sc.item = item;
	sc.s = s;
	sc.tc = tc;

	sc.name = args_get(args, 'n');
	args_to_vector(args, &sc.argc, &sc.argv);
	sc.environ = environ_create();

	av = args_first_value(args, 'e');
	while (av != NULL) {
		environ_put(sc.environ, av->string, 0);
		av = args_next_value(av);
	}

	sc.idx = idx;
	sc.cwd = args_get(args, 'c');

	sc.flags = 0;
	if (detached)
		sc.flags |= SPAWN_DETACHED;
	if (args_has(args, 'k'))
		sc.flags |= SPAWN_KILL;

	if ((new_wl = spawn_window(&sc, &cause)) == NULL) {
		cmdq_error(item, "create window failed: %s", cause);
		free(cause);
		if (sc.argv != NULL)
			cmd_free_argv(sc.argc, sc.argv);
		environ_free(sc.environ);
		return (CMD_RETURN_ERROR);
	}
	if (!detached || new_wl == s->curw) {
		cmd_find_from_winlink(current, new_wl, 0);
		server_redraw_session_group(s);
	} else
		server_status_session_group(s);

	if (args_has(args, 'P')) {
		if ((template = args_get(args, 'F')) == NULL)
			template = NEW_WINDOW_TEMPLATE;
		cp = format_single(item, template, tc, s, new_wl,
			new_wl->window->active);
		cmdq_print(item, "%s", cp);
		free(cp);
	}

	cmd_find_from_winlink(&fs, new_wl, 0);
	cmdq_insert_hook(s, item, &fs, "after-new-window");

	if (sc.argv != NULL)
		cmd_free_argv(sc.argc, sc.argv);
	environ_free(sc.environ);

	if (wc != NULL) {
		nwd = xcalloc(1, sizeof *nwd);
		nwd->item = item;
		nwd->wp = new_wl->window->active;
		nwd->tc = wc;
		wc->references++;
		nwd->session = s->id;
		TAILQ_INSERT_TAIL(&new_window_draws, nwd, entry);
		nwd->wp->draw_wait = nwd;
		return (CMD_RETURN_WAIT);
	}
	return (CMD_RETURN_NORMAL);
}
