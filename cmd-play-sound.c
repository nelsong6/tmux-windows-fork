/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Nelson Grip
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

#include <stdlib.h>

#include "tmux.h"

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

/*
 * Play a sound file from the server, without waiting for it.
 */

static enum cmd_retval	cmd_play_sound_exec(struct cmd *, struct cmdq_item *);

const struct cmd_entry cmd_play_sound_entry = {
	.name = "play-sound",
	.alias = NULL,

	.args = { "", 1, 1, NULL },
	.usage = "path",

	.flags = CMD_AFTERHOOK,
	.exec = cmd_play_sound_exec
};

static enum cmd_retval
cmd_play_sound_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args	*args = cmd_get_args(self);
	const char	*path = args_string(args, 0);
#ifdef _WIN32
	wchar_t		*wpath;
	int		 n, ok;

	if (access(path, R_OK) != 0) {
		cmdq_error(item, "can't read %s", path);
		return (CMD_RETURN_ERROR);
	}
	n = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
	if (n <= 0) {
		cmdq_error(item, "bad path: %s", path);
		return (CMD_RETURN_ERROR);
	}
	wpath = xcalloc(n, sizeof *wpath);
	MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, n);
	ok = PlaySoundW(wpath, NULL, SND_FILENAME|SND_ASYNC|SND_NODEFAULT);
	free(wpath);
	if (!ok) {
		cmdq_error(item, "can't play %s", path);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
#else
	cmdq_error(item, "play-sound is not supported on this platform: %s",
	    path);
	return (CMD_RETURN_ERROR);
#endif
}
