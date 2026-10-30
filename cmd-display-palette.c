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

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "tmux.h"
#include "palette-descriptions.h"

/*
 * Display a command palette: every command, alias and binding in the prefix
 * and root tables, as rows for a picker (fzt -tiered) run in a popup. The rows
 * go to the picker's standard input. It prints the id of the pick, with a tab
 * and the typed text for a row that asks for text, and the pick runs once the
 * popup closes. A row that offers a choice (a menu, a session, window or
 * buffer chooser) is a folder of the choices.
 *
 * A row is depth, name, description, the hint of a row that asks for text
 * (empty for the rest) and id, separated by tabs.
 */

static enum cmd_retval	cmd_display_palette_exec(struct cmd *,
			    struct cmdq_item *);

const struct cmd_entry cmd_display_palette_entry = {
	.name = "display-palette",
	.alias = NULL,

	.args = { "c:h:t:w:", 1, -1, NULL },
	.usage = "[-c target-client] [-h height] " CMD_TARGET_PANE_USAGE
		 " [-w width] program [argument ...]",

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK|CMD_CLIENT_CFLAG,
	.exec = cmd_display_palette_exec
};

extern const struct cmd_entry cmd_choose_buffer_entry;
extern const struct cmd_entry cmd_choose_tree_entry;
extern const struct cmd_entry cmd_command_prompt_entry;
extern const struct cmd_entry cmd_display_menu_entry;
extern const struct cmd_entry cmd_run_shell_entry;

/*
 * The server's ends of the picker's pipes are not inherited: a job started
 * while the palette is open (a status line #()) holding the write end of the
 * rows would keep the picker waiting for the end of its input.
 */
#ifdef _WIN32
#define palette_pipe(fds) _pipe(fds, 4096, _O_BINARY|_O_NOINHERIT)
#else
#define palette_pipe(fds) pipe(fds)
#endif

/* A prefix binding with no note is named by its command, cut to this. */
#define PALETTE_NAME_MAX 48

/* What a row runs: a binding's commands, a command, or a template. */
struct palette_entry {
	struct cmd_list		*cmdlist;
	char			*command;
	char			*template;
};

struct palette {
	struct cmdq_item	*item;
	struct client		*tc;
	struct cmd_find_state	*fs;

	struct evbuffer		*rows;
	struct palette_entry	*entries;
	u_int			 count;

	int			 out;
};

static void	palette_node(struct palette *, u_int, const char *,
		    const char *, struct cmd_list *, const char *, int);

/* A field with the tabs and newlines that would split the row made spaces. */
static void
palette_add_field(struct palette *p, const char *s)
{
	evbuffer_add(p->rows, "\t", 1);
	for (; s != NULL && *s != '\0'; s++) {
		if (*s == '\t' || *s == '\n' || *s == '\r')
			evbuffer_add(p->rows, " ", 1);
		else
			evbuffer_add(p->rows, s, 1);
	}
}

/* Add a row and return its entry, which runs nothing until it is set. */
static struct palette_entry *
palette_add(struct palette *p, u_int depth, const char *name, const char *desc,
    const char *hint)
{
	evbuffer_add_printf(p->rows, "%u", depth);
	palette_add_field(p, name);
	palette_add_field(p, desc);
	palette_add_field(p, hint);
	evbuffer_add_printf(p->rows, "\t%u\n", p->count);

	p->entries = xreallocarray(p->entries, p->count + 1,
	    sizeof *p->entries);
	memset(&p->entries[p->count], 0, sizeof *p->entries);
	return (&p->entries[p->count++]);
}

/* Add a row that runs a command. */
static void
palette_add_command(struct palette *p, u_int depth, const char *name,
    const char *desc, const char *command)
{
	palette_add(p, depth, name, desc, NULL)->command = xstrdup(command);
}

/*
 * command-prompt's substitution: %1 and the first %% take the text as typed,
 * and %%% escapes the characters a double-quoted string acts on. Unlike
 * command-prompt, the text is escaped inside double quotes too, so a typed "
 * cannot end the string early.
 */
static char *
palette_template(const char *template, const char *text)
{
	const char	*ptr, *cp, quote[] = "\"\\$;~";
	char		*buf, ch;
	size_t		 len = 0;
	int		 replaced = 0, quoted, inquotes = 0;

	buf = xmalloc(1);
	*buf = '\0';

	ptr = template;
	while (*ptr != '\0') {
		switch (ch = *ptr++) {
		case '\\':
			buf = xrealloc(buf, len + 3);
			buf[len++] = ch;
			if (*ptr != '\0')
				buf[len++] = *ptr++;
			buf[len] = '\0';
			continue;
		case '"':
			inquotes = !inquotes;
			break;
		case '%':
			if (*ptr != '1') {
				if (*ptr != '%' || replaced)
					break;
				replaced = 1;
			}
			ptr++;

			quoted = inquotes;
			if (*ptr == '%') {
				quoted = 1;
				ptr++;
			}

			buf = xrealloc(buf, len + (strlen(text) * 2) + 1);
			for (cp = text; *cp != '\0'; cp++) {
				if (quoted && strchr(quote, *cp) != NULL)
					buf[len++] = '\\';
				buf[len++] = *cp;
			}
			buf[len] = '\0';
			continue;
		}
		buf = xrealloc(buf, len + 2);
		buf[len++] = ch;
		buf[len] = '\0';
	}
	return (buf);
}

/* A command's entry by name or alias. */
static const struct cmd_entry *
palette_find_entry(const char *name)
{
	const struct cmd_entry	**loop;

	for (loop = cmd_table; *loop != NULL; loop++) {
		if (strcmp((*loop)->name, name) == 0)
			return (*loop);
		if ((*loop)->alias != NULL && strcmp((*loop)->alias, name) == 0)
			return (*loop);
	}
	return (NULL);
}

/*
 * The required arguments in a usage, space separated, or NULL if there are
 * none: what is left once the bracketed (optional) parts are gone.
 */
static char *
palette_required(const char *usage)
{
	char		*buf;
	const char	*cp;
	size_t		 len = 0;
	int		 depth = 0;

	buf = xmalloc(strlen(usage) + 1);
	for (cp = usage; *cp != '\0'; cp++) {
		if (*cp == '[')
			depth++;
		else if (*cp == ']' && depth > 0)
			depth--;
		else if (depth == 0 &&
		    (*cp != ' ' || (len != 0 && buf[len - 1] != ' ')))
			buf[len++] = *cp;
	}
	while (len != 0 && buf[len - 1] == ' ')
		len--;
	buf[len] = '\0';
	if (len == 0) {
		free(buf);
		return (NULL);
	}
	return (buf);
}

/* A command's description: its first sentence in the manual, or its usage. */
static const char *
palette_description(const struct cmd_entry *entry)
{
	const struct palette_description	*pd;

	for (pd = palette_descriptions; pd->name != NULL; pd++) {
		if (strcmp(pd->name, entry->name) == 0)
			return (pd->text);
	}
	return (entry->usage);
}

/* A format expanded as display-menu expands its items. */
static char *
palette_expand(struct palette *p, const char *fmt)
{
	return (format_single_from_state(p->item, fmt, p->tc, p->fs));
}

/*
 * Parse commands, or NULL if they don't parse. cmd_parse_from_string's result
 * is shared by every call, and adding a row can parse again (a run-shell -C
 * expanded, a menu's items), so the list is taken out of it at once.
 */
static struct cmd_list *
palette_parse(const char *text)
{
	struct cmd_parse_result	*pr;

	pr = cmd_parse_from_string(text, NULL);
	if (pr->status != CMD_PARSE_SUCCESS) {
		free(pr->error);
		return (NULL);
	}
	return (pr->cmdlist);
}

/* Remove the #[...] style directives from a name. */
static void
palette_strip_styles(char *s)
{
	char	*out = s, *end;

	while (*s != '\0') {
		if (s[0] == '#' && s[1] == '[' && (end = strchr(s, ']')) != NULL) {
			s = end + 1;
			continue;
		}
		*out++ = *s++;
	}
	*out = '\0';
}

/* A folder of a session's windows, each running the template with its id. */
static void
palette_add_windows(struct palette *p, u_int depth, const char *template,
    int byindex, struct session *s)
{
	struct winlink	*wl;
	char		*id, *name, *desc;

	RB_FOREACH(wl, winlinks, &s->windows) {
		if (byindex) {
			xasprintf(&id, "%d", wl->idx);
			name = xstrdup(id);
			desc = xstrdup(wl->window->name);
		} else {
			xasprintf(&id, "@%u", wl->window->id);
			xasprintf(&name, "%d: %s", wl->idx, wl->window->name);
			xasprintf(&desc, "%u panes",
			    window_count_panes(wl->window));
		}
		palette_add(p, depth, name, desc, NULL)->command =
		    palette_template(template, id);
		free(name);
		free(desc);
		free(id);
	}
}

/*
 * Add a folder of what a menu or chooser offers, or a row saying there is
 * nothing if it offers nothing; 0 if the command opens no menu or chooser. A
 * menu item can itself ask for text or offer a choice.
 */
static int
palette_menu(struct palette *p, u_int depth, const char *name,
    const char *desc, struct cmd *cmd)
{
	const struct cmd_entry	*entry = cmd_get_entry(cmd);
	struct args		*args = cmd_get_args(cmd);
	struct cmd_list		*cmdlist;
	struct paste_buffer	*pb;
	struct session		*s;
	const char		*template, *value;
	char			*command, *id, *sdesc, **items = NULL;
	u_int			 i, n = 0, count = args_count(args);
	int			 found;

	if (entry == &cmd_run_shell_entry) {
		value = args_string(args, 0);
		if (!args_has(args, 'C') || count != 1 ||
		    strstr(value, "#{") == NULL)
			return (0);
		command = palette_expand(p, value);
		cmdlist = palette_parse(command);
		free(command);
		if (cmdlist == NULL)
			return (0);
		found = 0;
		if (cmd_list_next(cmd_list_first(cmdlist)) == NULL) {
			found = palette_menu(p, depth, name, desc,
			    cmd_list_first(cmdlist));
		}
		cmd_list_free(cmdlist);
		return (found);
	}

	if (entry == &cmd_display_menu_entry) {
		/*
		 * As display-menu shows it: an empty name is a separator, and
		 * an item whose name expands to nothing or to -name (greyed
		 * out) is not offered.
		 */
		for (i = 0; i < count; /* nothing */) {
			value = args_string(args, i++);
			if (*value == '\0')
				continue;
			if (count - i < 2)
				break;
			command = palette_expand(p, value);
			palette_strip_styles(command);
			i++;
			if (*command == '\0' || *command == '-') {
				free(command);
				i++;
				continue;
			}
			items = xreallocarray(items, n + 2, sizeof *items);
			items[n++] = command;
			items[n++] = palette_expand(p, args_string(args, i++));
		}
		if (n == 0) {
			palette_add_command(p, depth, name, desc,
			    "display-message 'Nothing to choose from'");
			return (1);
		}
		palette_add(p, depth, name, desc, NULL);
		for (i = 0; i < n; i += 2) {
			if ((cmdlist = palette_parse(items[i + 1])) != NULL) {
				palette_node(p, depth + 1, items[i], "",
				    cmdlist, items[i + 1], 1);
				cmd_list_free(cmdlist);
			} else {
				palette_add_command(p, depth + 1, items[i], "",
				    items[i + 1]);
			}
			free(items[i]);
			free(items[i + 1]);
		}
		free(items);
		return (1);
	}

	if (entry == &cmd_choose_buffer_entry) {
		if (paste_is_empty()) {
			palette_add_command(p, depth, name, desc,
			    "display-message 'Nothing to choose from'");
			return (1);
		}
		template = count != 0 ? args_string(args, 0) :
		    "paste-buffer -p -b '%%'";
		palette_add(p, depth, name, desc, NULL);
		pb = NULL;
		while ((pb = paste_walk(pb)) != NULL) {
			sdesc = paste_make_sample(pb);
			palette_add(p, depth + 1, paste_buffer_name(pb), sdesc,
			    NULL)->command = palette_template(template,
			    paste_buffer_name(pb));
			free(sdesc);
		}
		return (1);
	}

	if (entry == &cmd_choose_tree_entry) {
		/* tmux's own default: switch to the chosen session or window. */
		template = count != 0 ? args_string(args, 0) :
		    "switch-client -Zt '%%'";
		palette_add(p, depth, name, desc, NULL);
		RB_FOREACH(s, sessions, &sessions) {
			xasprintf(&sdesc, "%u windows%s",
			    winlink_count(&s->windows),
			    s->attached != 0 ? " (attached)" : "");
			if (args_has(args, 's')) {
				xasprintf(&id, "$%u", s->id);
				palette_add(p, depth + 1, s->name, sdesc,
				    NULL)->command = palette_template(template,
				    id);
				free(id);
			} else {
				palette_add(p, depth + 1, s->name, sdesc, NULL);
				palette_add_windows(p, depth + 2, template, 0,
				    s);
			}
			free(sdesc);
		}
		return (1);
	}

	return (0);
}

/*
 * Add a row for a command-prompt that only asks for one line of text, with its
 * hint and template; 0 for one that can't be asked in the picker (a key to
 * press, several prompts, or, unless its formats were already expanded, a
 * template that is itself a format). A prompt for a window is a folder of the
 * windows.
 */
static int
palette_prompt(struct palette *p, u_int depth, const char *name,
    const char *desc, struct cmd *cmd, int expanded)
{
	struct args		*args = cmd_get_args(cmd);
	const struct cmd_entry	*entry;
	const char		*template, *prompt, *init, *type, *cp;
	char			*hint, *first, *now;
	size_t			 len;

	if (cmd_get_entry(cmd) != &cmd_command_prompt_entry)
		return (0);
	if (args_has(args, 'k') || args_has(args, '1'))
		return (0);
	if (args_has(args, 'F') && !expanded)
		return (0);
	if (args_count(args) != 1)
		return (0);
	template = args_string(args, 0);
	prompt = args_get(args, 'p');
	if (prompt != NULL && strchr(prompt, ',') != NULL)
		return (0);
	for (cp = strchr(template, '%'); cp != NULL; cp = strchr(cp + 1, '%')) {
		if (cp[1] >= '2' && cp[1] <= '9')
			return (0);
	}

	type = args_get(args, 'T');
	if (type != NULL && strcmp(type, "window-target") == 0) {
		palette_add(p, depth, name, desc, NULL);
		palette_add_windows(p, depth + 1, template, 1, p->fs->s);
		return (1);
	}

	if (prompt != NULL) {
		hint = xstrdup(prompt);
		len = strlen(hint);
		while (len != 0 && (hint[len - 1] == ':' ||
		    hint[len - 1] == ' '))
			hint[--len] = '\0';
	} else {
		first = xstrdup(template);
		first[strcspn(first, " ")] = '\0';
		entry = palette_find_entry(first);
		free(first);
		hint = NULL;
		if (entry != NULL)
			hint = palette_required(entry->usage);
		if (hint == NULL)
			hint = xstrdup("text");
	}

	init = args_get(args, 'I');
	if (init != NULL) {
		now = palette_expand(p, init);
		first = hint;
		xasprintf(&hint, "%s (now %s)", first, now);
		free(first);
		free(now);
	}

	palette_add(p, depth, name, desc, hint)->template = xstrdup(template);
	free(hint);
	return (1);
}

/*
 * Add a row for commands: one that asks for text, a folder of what a menu or
 * chooser offers, or one that runs. command, if given, is what runs instead of
 * the commands themselves (an alias runs by its name). expanded is set when
 * the commands' formats have already been expanded, as a menu item's are.
 */
static void
palette_node(struct palette *p, u_int depth, const char *name,
    const char *desc, struct cmd_list *cmdlist, const char *command,
    int expanded)
{
	struct cmd		*cmd = cmd_list_first(cmdlist);
	struct palette_entry	*pe;

	if (cmd != NULL && cmd_list_next(cmd) == NULL) {
		if (palette_prompt(p, depth, name, desc, cmd, expanded))
			return;
		if (palette_menu(p, depth, name, desc, cmd))
			return;
	}

	pe = palette_add(p, depth, name, desc, NULL);
	if (command != NULL)
		pe->command = xstrdup(command);
	else {
		pe->cmdlist = cmdlist;
		cmdlist->references++;
	}
}

/* Add a row for a command given as text. */
static void
palette_node_text(struct palette *p, const char *name, const char *desc,
    const char *text, const char *command)
{
	struct cmd_list	*cmdlist;

	if ((cmdlist = palette_parse(text)) == NULL) {
		palette_add_command(p, 0, name, desc, command);
		return;
	}
	palette_node(p, 0, name, desc, cmdlist, command, 0);
	cmd_list_free(cmdlist);
}

static void
palette_add_commands(struct palette *p)
{
	const struct cmd_entry	**loop;
	struct palette_entry	*pe;
	char			*required;
	const char		*name;

	for (loop = cmd_table; *loop != NULL; loop++) {
		name = (*loop)->name;
		required = palette_required((*loop)->usage);
		if (required == NULL) {
			palette_node_text(p, name, palette_description(*loop),
			    name, name);
			continue;
		}

		/*
		 * One argument is quoted, so a name with spaces stays one;
		 * several are typed as tmux syntax.
		 */
		pe = palette_add(p, 0, name, palette_description(*loop),
		    required);
		if (strchr(required, ' ') == NULL)
			xasprintf(&pe->template, "%s \"%%%%%%\"", name);
		else
			xasprintf(&pe->template, "%s %%%%", name);
		free(required);
	}
}

static int
palette_cmp_strings(const void *a, const void *b)
{
	return (strcmp(*(const char **)a, *(const char **)b));
}

static void
palette_add_aliases(struct palette *p)
{
	struct options_entry		*o;
	struct options_array_item	*a;
	const char			**list = NULL;
	char				*name, *target, *desc;
	u_int				 n = 0, i;

	o = options_get_only(global_options, "command-alias");
	if (o == NULL)
		return;
	for (a = options_array_first(o); a != NULL; a = options_array_next(a)) {
		list = xreallocarray(list, n + 1, sizeof *list);
		list[n++] = options_array_item_value(a)->string;
	}
	if (n != 0)
		qsort(list, n, sizeof *list, palette_cmp_strings);

	for (i = 0; i < n; i++) {
		name = xstrdup(list[i]);
		if ((target = strchr(name, '=')) == NULL) {
			free(name);
			continue;
		}
		*target++ = '\0';
		xasprintf(&desc, "alias for %s", target);
		palette_node_text(p, name, desc, target, name);
		free(desc);
		free(name);
	}
	free(list);
}

/*
 * The bindings in the prefix and root tables. No key reaches the prefix table
 * (the palette replaces the prefix), so its bindings are named by what they
 * do, not by their key.
 */
static void
palette_add_bindings(struct palette *p)
{
	static const char	*tables[] = { "prefix", "root" };
	struct key_table	*table;
	struct key_binding	*bd;
	char			*command, *name, *desc;
	size_t			 cut;
	u_int			 i;

	for (i = 0; i < nitems(tables); i++) {
		table = key_bindings_get_table(tables[i], 0);
		if (table == NULL)
			continue;
		bd = key_bindings_first(table);
		for (; bd != NULL; bd = key_bindings_next(table, bd)) {
			command = cmd_list_print(bd->cmdlist, 0);
			if (i == 1) {
				name = xstrdup(key_string_lookup_key(bd->key,
				    0));
				if (bd->note != NULL) {
					xasprintf(&desc, "%s  (%s)", bd->note,
					    command);
				} else
					desc = xstrdup(command);
			} else if (bd->note != NULL) {
				name = xstrdup(bd->note);
				desc = xstrdup(command);
			} else if (strlen(command) > PALETTE_NAME_MAX) {
				/*
				 * The picker sizes the name column to the
				 * longest name, so a long command would push
				 * every description out of the popup.
				 */
				cut = PALETTE_NAME_MAX - 1;
				while (cut != 0 &&
				    (command[cut] & 0xc0) == 0x80)
					cut--;
				xasprintf(&name, "%.*s\xe2\x80\xa6", (int)cut,
				    command);
				desc = xstrdup(command);
			} else {
				name = xstrdup(command);
				desc = xstrdup("key binding");
			}
			palette_node(p, 0, name, desc, bd->cmdlist, NULL, 0);
			free(name);
			free(desc);
			free(command);
		}
	}
}

static void
palette_free(struct palette *p)
{
	u_int	i;

	for (i = 0; i < p->count; i++) {
		if (p->entries[i].cmdlist != NULL)
			cmd_list_free(p->entries[i].cmdlist);
		free(p->entries[i].command);
		free(p->entries[i].template);
	}
	free(p->entries);
	evbuffer_free(p->rows);
	if (p->out != -1)
		close(p->out);
	server_client_unref(p->tc);
	free(p);
}

/* The picker's output; it has exited, so it is all there to read. */
static char *
palette_read_pick(struct palette *p)
{
	struct evbuffer	*evb;
	char		 buf[4096], *pick;
	int		 n;
#ifdef _WIN32
	HANDLE		 h = (HANDLE)_get_osfhandle(p->out);
	DWORD		 avail;
#endif

	evb = evbuffer_new();
	if (evb == NULL)
		fatalx("out of memory");
#ifdef _WIN32
	/* Peek first: a read of an empty pipe would block the server. */
	while (PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL) && avail != 0) {
		n = read(p->out, buf, avail < sizeof buf ? avail : sizeof buf);
		if (n <= 0)
			break;
		evbuffer_add(evb, buf, n);
	}
#else
	setblocking(p->out, 0);
	while ((n = read(p->out, buf, sizeof buf)) > 0)
		evbuffer_add(evb, buf, n);
#endif
	evbuffer_add(evb, "", 1);
	pick = xstrdup((char *)EVBUFFER_DATA(evb));
	evbuffer_free(evb);
	pick[strcspn(pick, "\r\n")] = '\0';
	return (pick);
}

/* Run the pick, if there is one, once the popup has closed. */
static void
cmd_display_palette_close(int status, void *arg)
{
	struct palette		*p = arg;
	struct cmdq_item	*item = p->item;
	struct palette_entry	*pe;
	const char		*text = "";
	char			*pick, *tab, *end, *command = NULL, *error;
	u_long			 id;

	if (status != 0) {
		palette_free(p);
		return;
	}

	pick = palette_read_pick(p);
	if ((tab = strchr(pick, '\t')) != NULL) {
		*tab = '\0';
		text = tab + 1;
	}
	id = strtoul(pick, &end, 10);
	if (*pick == '\0' || *end != '\0' || id >= p->count) {
		free(pick);
		palette_free(p);
		return;
	}
	pe = &p->entries[id];

	if (pe->cmdlist != NULL) {
		cmdq_insert_after(item, cmdq_get_command(pe->cmdlist,
		    cmdq_get_state(item)));
	} else {
		if (pe->template != NULL)
			command = palette_template(pe->template, text);
		else if (pe->command != NULL)
			command = xstrdup(pe->command);
		if (command != NULL && cmd_parse_and_insert(command, NULL, item,
		    cmdq_get_state(item), &error) == CMD_PARSE_ERROR) {
			cmdq_insert_after(item, cmdq_get_error(error));
			free(error);
		}
		free(command);
	}
	free(pick);
	palette_free(p);
}

/* Write the rows to the picker and close its input. */
static void
palette_send_rows(int fd, struct evbuffer *rows)
{
#ifdef _WIN32
	win32_write_close(fd, EVBUFFER_DATA(rows), EVBUFFER_LENGTH(rows));
#else
	const u_char	*data = EVBUFFER_DATA(rows);
	size_t		 len = EVBUFFER_LENGTH(rows), off = 0;
	ssize_t		 n;

	while (off < len) {
		n = write(fd, data + off, len - off);
		if (n <= 0)
			break;
		off += n;
	}
	close(fd);
#endif
}

static enum cmd_retval
cmd_display_palette_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = cmd_get_args(self);
	struct cmd_find_state	*target = cmdq_get_target(item);
	struct client		*tc = cmdq_get_target_client(item);
	struct tty		*tty = &tc->tty;
	struct palette		*p;
	char			*cause = NULL, **argv = NULL;
	int			 argc = 0, in[2], out[2];
	u_int			 w, h;

	if (tc->overlay_draw != NULL)
		return (CMD_RETURN_NORMAL);

	h = tty->sy / 2;
	if (args_has(args, 'h')) {
		h = args_percentage(args, 'h', 3, tty->sy, tty->sy, &cause);
		if (cause != NULL) {
			cmdq_error(item, "height %s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}
	}
	w = tty->sx / 2;
	if (args_has(args, 'w')) {
		w = args_percentage(args, 'w', 3, tty->sx, tty->sx, &cause);
		if (cause != NULL) {
			cmdq_error(item, "width %s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}
	}

	if (palette_pipe(in) != 0) {
		cmdq_error(item, "pipe failed: %s", strerror(errno));
		return (CMD_RETURN_ERROR);
	}
	if (palette_pipe(out) != 0) {
		cmdq_error(item, "pipe failed: %s", strerror(errno));
		close(in[0]);
		close(in[1]);
		return (CMD_RETURN_ERROR);
	}

	p = xcalloc(1, sizeof *p);
	p->item = item;
	p->tc = tc;
	tc->references++;
	p->fs = target;
	p->out = out[0];
	if ((p->rows = evbuffer_new()) == NULL)
		fatalx("out of memory");

	palette_add_commands(p);
	palette_add_aliases(p);
	palette_add_bindings(p);

	args_to_vector(args, &argc, &argv);
	if (popup_display(POPUP_CLOSEEXIT, BOX_LINES_DEFAULT, item,
	    (tty->sx - w) / 2, (tty->sy - h) / 2, w, h, NULL, NULL, argc, argv,
	    server_client_get_cwd(tc, target->s), NULL, tc, target->s, NULL,
	    NULL, cmd_display_palette_close, p, in[0], out[1]) != 0) {
		cmdq_error(item, "can't open a %ux%u popup running %s", w, h,
		    argv[0]);
		cmd_free_argv(argc, argv);
		close(in[0]);
		close(in[1]);
		close(out[1]);
		palette_free(p);
		return (CMD_RETURN_ERROR);
	}
	cmd_free_argv(argc, argv);
	close(in[0]);
	close(out[1]);

	palette_send_rows(in[1], p->rows);
	return (CMD_RETURN_WAIT);
}
