/* $OpenBSD$ */

#include <sys/types.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#ifdef __linux__
#include <sys/syscall.h>
#endif

#include "tmux.h"

/*
 * Take over a pane's terminal from another tmux server, so its program keeps
 * running under this one. Moving a server that has no upgrade-server (stock
 * tmux, or a build before it) onto this build goes pane by pane: a window is
 * made here as usual, then adopt-pane swaps its terminal for the other
 * server's, which can then be stopped without the program noticing.
 */

static enum cmd_retval	cmd_adopt_pane_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_adopt_pane_entry = {
	.name = "adopt-pane",
	.alias = NULL,

	.args = { "f:p:P:s:t:", 0, 0, NULL },
	.usage = "-p server-pid -f fd -P pane-pid [-s screen-file] "
		 CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = 0,
	.exec = cmd_adopt_pane_exec
};

#ifdef __linux__
/*
 * Draw a captured screen (capture-pane -e -p, less its last newline, then
 * the cursor's position) into the pane, its earlier lines scrolling into the
 * history. Lines end in a bare newline there.
 */
static void
cmd_adopt_pane_draw(struct window_pane *wp, const char *path, char **cause)
{
	FILE	*f;
	u_char	*buf, *out;
	long	 size;
	size_t	 i, n = 0;

	if ((f = fopen(path, "rb")) == NULL) {
		xasprintf(cause, "%s: %s", path, strerror(errno));
		return;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0) {
		xasprintf(cause, "%s: %s", path, strerror(errno));
		fclose(f);
		return;
	}
	rewind(f);
	buf = xmalloc(size + 1);
	if (fread(buf, 1, size, f) != (size_t)size) {
		xasprintf(cause, "%s: short read", path);
		fclose(f);
		free(buf);
		return;
	}
	fclose(f);

	out = xmalloc(2 * size + 1);
	for (i = 0; i < (size_t)size; i++) {
		if (buf[i] == '\n')
			out[n++] = '\r';
		out[n++] = buf[i];
	}
	input_parse_buffer(wp, out, n);
	free(out);
	free(buf);
}
#endif

static enum cmd_retval
cmd_adopt_pane_exec(struct cmd *self, struct cmdq_item *item)
{
#ifdef __linux__
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct window_pane	*wp = target->wp;
	char			*cause = NULL;
	const char		*tty;
	struct winsize		 ws;
	pid_t			 server_pid, pane_pid;
	int			 fd_number, pidfd, fd;

	if (!args_has(args, 'p') || !args_has(args, 'f') ||
	    !args_has(args, 'P')) {
		cmdq_error(item, "-p, -f and -P are needed");
		return (CMD_RETURN_ERROR);
	}
	server_pid = args_strtonum(args, 'p', 1, INT_MAX, &cause);
	if (cause == NULL)
		fd_number = args_strtonum(args, 'f', 0, INT_MAX, &cause);
	if (cause == NULL)
		pane_pid = args_strtonum(args, 'P', 1, INT_MAX, &cause);
	if (cause != NULL) {
		cmdq_error(item, "%s", cause);
		free(cause);
		return (CMD_RETURN_ERROR);
	}

	/* A copy of the other server's descriptor, as dup would give. */
	pidfd = syscall(SYS_pidfd_open, server_pid, 0);
	if (pidfd == -1) {
		cmdq_error(item, "server %ld: %s", (long)server_pid,
		    strerror(errno));
		return (CMD_RETURN_ERROR);
	}
	fd = syscall(SYS_pidfd_getfd, pidfd, fd_number, 0);
	close(pidfd);
	if (fd == -1) {
		cmdq_error(item, "fd %d of server %ld: %s", fd_number,
		    (long)server_pid, strerror(errno));
		return (CMD_RETURN_ERROR);
	}
	if ((tty = ptsname(fd)) == NULL) {
		cmdq_error(item, "fd %d of server %ld is not a terminal",
		    fd_number, (long)server_pid);
		close(fd);
		return (CMD_RETURN_ERROR);
	}
	fcntl(fd, F_SETFD, FD_CLOEXEC);

	/*
	 * Let go of the pane's own terminal (its program gets a hangup and
	 * its exit matches no pane once the pid is the adopted one).
	 */
	if (wp->fd != -1) {
		bufferevent_free(wp->event);
		wp->event = NULL;
		close(wp->fd);
	}
	if (wp->ictx != NULL) {
		input_free(wp->ictx);
		wp->ictx = NULL;
	}
	wp->fd = fd;
	wp->pid = pane_pid;
	strlcpy(wp->tty, tty, sizeof wp->tty);
	wp->flags &= ~(PANE_EXITED|PANE_STATUSREADY|PANE_STATUSDRAWN);
	screen_reinit(&wp->base);
	window_pane_set_event(wp);

	if (args_has(args, 's')) {
		cmd_adopt_pane_draw(wp, args_get(args, 's'), &cause);
		if (cause != NULL) {
			cmdq_error(item, "%s", cause);
			free(cause);
		}
	}

	/*
	 * The program draws again at this pane's size. A terminal already at
	 * that size sends no SIGWINCH, so it is nudged off it first.
	 */
	if (ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col == wp->sx &&
	    ws.ws_row == wp->sy && wp->sy > 1)
		window_pane_send_resize(wp, wp->sx, wp->sy - 1);
	window_pane_send_resize(wp, wp->sx, wp->sy);
	wp->flags |= PANE_REDRAW;
	server_redraw_window(wp->window);
	return (CMD_RETURN_NORMAL);
#else
	(void)self;
	cmdq_error(item, "adopt-pane is only on Linux");
	return (CMD_RETURN_ERROR);
#endif
}
