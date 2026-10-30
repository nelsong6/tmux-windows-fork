/* $OpenBSD$ */

#include <sys/types.h>

#include "tmux.h"

/*
 * Move the server to another build without restarting anything in it.
 */

static enum cmd_retval	cmd_upgrade_server_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_upgrade_server_entry = {
	.name = "upgrade-server",
	.alias = NULL,

	.args = { "", 1, 1, NULL },
	.usage = "tmux-path",

	.flags = 0,
	.exec = cmd_upgrade_server_exec
};

static enum cmd_retval
cmd_upgrade_server_exec(struct cmd *self, struct cmdq_item *item)
{
#ifdef _WIN32
	struct args	*args = cmd_get_args(self);
	char		*cause;

	if (server_upgrade_request(args_string(args, 0), &cause) != 0) {
		cmdq_error(item, "%s", cause);
		free(cause);
		return (CMD_RETURN_ERROR);
	}
	return (CMD_RETURN_NORMAL);
#else
	(void)self;
	cmdq_error(item, "upgrade-server is only on Windows");
	return (CMD_RETURN_ERROR);
#endif
}
