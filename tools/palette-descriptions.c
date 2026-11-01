/*
 * Writes the first sentence of each command's description in tmux.1, and the
 * tags of the section it is in, as a C table for display-palette, run at build
 * time:
 *
 *	palette-descriptions tmux.1 palette-descriptions.h
 *
 * An entry is ".It Xo Ic name", ".It Ic name", or ".It Xo" with ".Ic name"
 * next; its text runs until the next .It, .El, .Pp, .Bl or .Bd. mdoc macros are
 * reduced to their words: Nm is tmux, Fl x is -x, Xr name n is name, and the
 * other inline macros are dropped.
 *
 * A section's tags are the words of its .Sh title in lower case, without
 * "and", plurals made singular: CLIENTS AND SESSIONS is "client session".
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_LINES 20000
#define MAX_TEXT 4096

static char	*lines[MAX_LINES];
static int	 nlines;

static const char *dropped[] = {
	"Ar", "Ic", "Ql", "Em", "Cm", "Ev", "Pa", "Va", "Xr", "Dq", "Pq", "Sq",
	"Op", "Ns", "No", "Li", NULL
};

static int
is_dropped(const char *word)
{
	int	i;

	for (i = 0; dropped[i] != NULL; i++) {
		if (strcmp(word, dropped[i]) == 0)
			return (1);
	}
	return (0);
}

static int
starts_macro(const char *line, const char *macro)
{
	size_t	len = strlen(macro);

	return (line[0] == '.' && strncmp(line + 1, macro, len) == 0 &&
	    (line[len + 1] == '\0' || isspace((unsigned char)line[len + 1])));
}

/* Append a line to the text, reducing a macro line to its words. */
static void
append_line(char *text, const char *line)
{
	char	 copy[MAX_TEXT], *word, *next;
	size_t	 len;

	if (*text != '\0')
		strncat(text, " ", MAX_TEXT - strlen(text) - 1);
	if (line[0] != '.') {
		strncat(text, line, MAX_TEXT - strlen(text) - 1);
		return;
	}

	snprintf(copy, sizeof copy, "%s", line + 1);
	for (word = strtok(copy, " \t"); word != NULL; word = next) {
		next = strtok(NULL, " \t");
		if (strcmp(word, "Nm") == 0)
			word = "tmux";
		else if (strcmp(word, "Fl") == 0 && next != NULL) {
			len = strlen(text);
			if (len != 0 && text[len - 1] != ' ')
				strncat(text, " ", MAX_TEXT - len - 1);
			strncat(text, "-", MAX_TEXT - strlen(text) - 1);
			strncat(text, next, MAX_TEXT - strlen(text) - 1);
			next = strtok(NULL, " \t");
			continue;
		} else if (strcmp(word, "Xr") == 0 && next != NULL) {
			word = next;
			next = strtok(NULL, " \t");
			if (next != NULL && isdigit((unsigned char)*next))
				next = strtok(NULL, " \t");
		} else if (is_dropped(word))
			continue;

		/* Punctuation closes up to the word before it. */
		len = strlen(text);
		if (len != 0 && text[len - 1] != ' ' &&
		    strchr(".,:;)", *word) == NULL)
			strncat(text, " ", MAX_TEXT - len - 1);
		else if (len != 0 && text[len - 1] == ' ' &&
		    strchr(".,:;)", *word) != NULL)
			text[len - 1] = '\0';
		strncat(text, word, MAX_TEXT - strlen(text) - 1);
	}
}

/* The first sentence of the text starting at line j, or NULL. */
static int
first_sentence(int j, char *text)
{
	char	*end;

	*text = '\0';
	for (; j < nlines; j++) {
		if (starts_macro(lines[j], "D1") &&
		    strncmp(lines[j], ".D1 Pq alias", 12) == 0)
			continue;
		if (starts_macro(lines[j], "It") || starts_macro(lines[j], "El") ||
		    starts_macro(lines[j], "Pp") || starts_macro(lines[j], "Bl") ||
		    starts_macro(lines[j], "Bd"))
			return (0);
		append_line(text, lines[j]);
		for (end = text; (end = strchr(end, '.')) != NULL; end++) {
			if (end[1] == '\0' || isspace((unsigned char)end[1])) {
				end[1] = '\0';
				return (1);
			}
		}
	}
	return (0);
}

static void
write_string(FILE *f, const char *s)
{
	fputc('"', f);
	for (; *s != '\0'; s++) {
		if (*s == '"' || *s == '\\')
			fputc('\\', f);
		fputc(*s, f);
	}
	fputc('"', f);
}

/* The tags of a .Sh line's title. */
static void
section_tags(const char *line, char *tags, size_t size)
{
	char	 copy[MAX_TEXT], *word;
	size_t	 len;

	*tags = '\0';
	snprintf(copy, sizeof copy, "%s", line + 3);
	for (word = strtok(copy, " \t"); word != NULL;
	    word = strtok(NULL, " \t")) {
		for (len = 0; word[len] != '\0'; len++)
			word[len] = tolower((unsigned char)word[len]);
		if (strcmp(word, "and") == 0)
			continue;
		/* clients and panes are plural; status and miscellaneous aren't. */
		if (len > 3 && word[len - 1] == 's' && word[len - 2] != 's' &&
		    word[len - 2] != 'u')
			word[len - 1] = '\0';
		if (*tags != '\0')
			strncat(tags, " ", size - strlen(tags) - 1);
		strncat(tags, word, size - strlen(tags) - 1);
	}
}

int
main(int argc, char **argv)
{
	FILE	*in, *out;
	char	 buf[MAX_TEXT], text[MAX_TEXT], name[256], *seen[MAX_LINES];
	char	 section[MAX_TEXT] = "";
	int	 i, j, k, xo, nseen = 0, dup;
	size_t	 len;

	if (argc != 3) {
		fprintf(stderr, "usage: %s tmux.1 output.h\n", argv[0]);
		return (1);
	}
	if ((in = fopen(argv[1], "r")) == NULL) {
		perror(argv[1]);
		return (1);
	}
	while (nlines < MAX_LINES && fgets(buf, sizeof buf, in) != NULL) {
		len = strcspn(buf, "\r\n");
		buf[len] = '\0';
		lines[nlines++] = strdup(buf);
	}
	fclose(in);

	if ((out = fopen(argv[2], "w")) == NULL) {
		perror(argv[2]);
		return (1);
	}
	fprintf(out, "/* Generated from tmux.1 by tools/palette-descriptions.c. */\n\n");
	fprintf(out, "static const struct palette_description {\n");
	fprintf(out, "\tconst char\t*name;\n\tconst char\t*text;\n");
	fprintf(out, "\tconst char\t*tags;\n");
	fprintf(out, "} palette_descriptions[] = {\n");

	for (i = 0; i < nlines; i++) {
		if (starts_macro(lines[i], "Sh")) {
			section_tags(lines[i], section, sizeof section);
			continue;
		}
		if (strcmp(lines[i], ".It Xo") == 0 && i + 1 < nlines &&
		    sscanf(lines[i + 1], ".Ic %255s", name) == 1)
			xo = 1;
		else if (sscanf(lines[i], ".It Xo Ic %255s", name) == 1)
			xo = 1;
		else if (sscanf(lines[i], ".It Ic %255s", name) == 1)
			xo = 0;
		else
			continue;

		dup = 0;
		for (k = 0; k < nseen; k++) {
			if (strcmp(seen[k], name) == 0)
				dup = 1;
		}
		if (dup)
			continue;

		j = i + 1;
		if (xo) {
			while (j < nlines && strcmp(lines[j], ".Xc") != 0)
				j++;
			j++;
		}
		if (!first_sentence(j, text))
			continue;
		seen[nseen++] = strdup(name);

		fputc('\t', out);
		fputc('{', out);
		write_string(out, name);
		fputs(", ", out);
		write_string(out, text);
		fputs(", ", out);
		write_string(out, section);
		fputs(" },\n", out);
	}

	fprintf(out, "\t{ NULL, NULL, NULL }\n};\n");
	if (fclose(out) != 0) {
		perror(argv[2]);
		return (1);
	}
	return (0);
}
