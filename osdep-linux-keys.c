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

/*
 * The Linux keyboard of the desktop the server runs on: held modifiers for
 * switch-client -H, and the global key table. Both read the keyboards'
 * evdev devices (/dev/input/event*), which needs the user in the input group
 * and works the same under X11 and Wayland. Without access, no modifier is
 * ever held (a chord ends on its first check) and the global table sees
 * nothing.
 */

#include <sys/types.h>
#include <sys/ioctl.h>

#include <linux/input.h>

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tmux.h"

#define DESKTOP_KEYBOARDS_MAX 32
#define DESKTOP_RESCAN_SECONDS 2

#define BIT_SET(bits, n) \
	((bits)[(n) / (8 * sizeof *(bits))] & \
	    (1UL << ((n) % (8 * sizeof *(bits)))))

struct desktop_keyboard {
	char		*path;
	int		 fd;
	struct event	 event;
	int		 event_added;
};
static struct desktop_keyboard	desktop_keyboards[DESKTOP_KEYBOARDS_MAX];
static u_int			desktop_nkeyboards;
static time_t			desktop_scanned;
static int			desktop_global_started;
static struct event		desktop_rescan_timer;

/*
 * The modifier keys held, in the order the global table's keyboards reported
 * them: a key read in a batch is checked against the modifiers of its own
 * moment, not those held when the batch is read.
 */
static const u_short	desktop_modifier_codes[] = {
	KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT,
	KEY_LEFTSHIFT, KEY_RIGHTSHIFT, KEY_LEFTMETA, KEY_RIGHTMETA
};
static u_long		desktop_global_held[
			    KEY_MAX / (8 * sizeof (u_long)) + 1];

static void	desktop_keyboard_read(int, short, void *);

/* Is this device a keyboard: does it have Ctrl and letters? */
static int
desktop_is_keyboard(int fd)
{
	u_long	bits[KEY_MAX / (8 * sizeof (u_long)) + 1];

	memset(bits, 0, sizeof bits);
	if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0)
		return (0);
	return (BIT_SET(bits, KEY_LEFTCTRL) && BIT_SET(bits, KEY_A));
}

/* Watch a keyboard for the global table. */
static void
desktop_keyboard_watch(struct desktop_keyboard *kb)
{
	if (kb->event_added)
		return;
	event_set(&kb->event, kb->fd, EV_READ|EV_PERSIST,
	    desktop_keyboard_read, kb);
	event_add(&kb->event, NULL);
	kb->event_added = 1;
}

/* Close a keyboard that has gone, moving the last one into its slot. */
static void
desktop_keyboard_close(u_int i)
{
	struct desktop_keyboard	*kb = &desktop_keyboards[i];
	struct desktop_keyboard	*last;

	if (kb->event_added)
		event_del(&kb->event);
	close(kb->fd);
	free(kb->path);
	last = &desktop_keyboards[--desktop_nkeyboards];
	if (kb != last) {
		*kb = *last;
		if (kb->event_added) {
			event_del(&last->event);
			event_set(&kb->event, kb->fd, EV_READ|EV_PERSIST,
			    desktop_keyboard_read, kb);
			event_add(&kb->event, NULL);
		}
	}
	memset(last, 0, sizeof *last);
}

/*
 * Open keyboards not already open. Keyboards come and go, so this looks again
 * when the last look is more than a couple of seconds old.
 */
static void
desktop_keyboards_scan(void)
{
	struct desktop_keyboard	*kb;
	glob_t			 g;
	time_t			 now = time(NULL);
	size_t			 i;
	u_int			 j;
	int			 fd;

	if (desktop_scanned != 0 &&
	    now - desktop_scanned < DESKTOP_RESCAN_SECONDS)
		return;
	desktop_scanned = now;

	if (glob("/dev/input/event*", 0, NULL, &g) != 0)
		return;
	for (i = 0; i < g.gl_pathc; i++) {
		if (desktop_nkeyboards == DESKTOP_KEYBOARDS_MAX)
			break;
		for (j = 0; j < desktop_nkeyboards; j++) {
			if (strcmp(desktop_keyboards[j].path,
			    g.gl_pathv[i]) == 0)
				break;
		}
		if (j != desktop_nkeyboards)
			continue;
		fd = open(g.gl_pathv[i], O_RDONLY|O_NONBLOCK|O_CLOEXEC);
		if (fd == -1)
			continue;
		if (!desktop_is_keyboard(fd)) {
			close(fd);
			continue;
		}
		log_debug("%s: keyboard %s", __func__, g.gl_pathv[i]);
		kb = &desktop_keyboards[desktop_nkeyboards++];
		kb->path = xstrdup(g.gl_pathv[i]);
		kb->fd = fd;
		kb->event_added = 0;
		if (desktop_global_started)
			desktop_keyboard_watch(kb);
	}
	globfree(&g);
}

/* The keys held on every keyboard together. */
static void
desktop_keys_held(u_long *bits, size_t size)
{
	u_long	one[KEY_MAX / (8 * sizeof (u_long)) + 1];
	u_int	i, n;

	memset(bits, 0, size);
	desktop_keyboards_scan();
	for (i = 0; i < desktop_nkeyboards; /* nothing */) {
		memset(one, 0, sizeof one);
		if (ioctl(desktop_keyboards[i].fd, EVIOCGKEY(sizeof one),
		    one) < 0) {
			if (errno == ENODEV) {
				desktop_keyboard_close(i);
				continue;
			}
		} else {
			for (n = 0; n < nitems(one); n++)
				bits[n] |= one[n];
		}
		i++;
	}
}

/* The tmux modifiers among the held keys. */
static key_code
desktop_modifiers(const u_long *bits)
{
	key_code	modifiers = 0;

	if (BIT_SET(bits, KEY_LEFTCTRL) || BIT_SET(bits, KEY_RIGHTCTRL))
		modifiers |= KEYC_CTRL;
	if (BIT_SET(bits, KEY_LEFTALT) || BIT_SET(bits, KEY_RIGHTALT))
		modifiers |= KEYC_META;
	if (BIT_SET(bits, KEY_LEFTSHIFT) || BIT_SET(bits, KEY_RIGHTSHIFT))
		modifiers |= KEYC_SHIFT;
	if (BIT_SET(bits, KEY_LEFTMETA) || BIT_SET(bits, KEY_RIGHTMETA))
		modifiers |= KEYC_SUPER;
	return (modifiers);
}

/* Are all the modifiers in a key still physically held? */
int
desktop_modifiers_held(key_code modifiers)
{
	u_long	bits[KEY_MAX / (8 * sizeof (u_long)) + 1];

	modifiers &= KEYC_CTRL|KEYC_META|KEYC_SHIFT;
	desktop_keys_held(bits, sizeof bits);
	return ((desktop_modifiers(bits) & modifiers) == modifiers);
}

/* The character each key types on a US layout. */
static const struct {
	u_short	code;
	char	ch;
} desktop_key_chars[] = {
	{ KEY_A, 'a' }, { KEY_B, 'b' }, { KEY_C, 'c' }, { KEY_D, 'd' },
	{ KEY_E, 'e' }, { KEY_F, 'f' }, { KEY_G, 'g' }, { KEY_H, 'h' },
	{ KEY_I, 'i' }, { KEY_J, 'j' }, { KEY_K, 'k' }, { KEY_L, 'l' },
	{ KEY_M, 'm' }, { KEY_N, 'n' }, { KEY_O, 'o' }, { KEY_P, 'p' },
	{ KEY_Q, 'q' }, { KEY_R, 'r' }, { KEY_S, 's' }, { KEY_T, 't' },
	{ KEY_U, 'u' }, { KEY_V, 'v' }, { KEY_W, 'w' }, { KEY_X, 'x' },
	{ KEY_Y, 'y' }, { KEY_Z, 'z' }, { KEY_0, '0' }, { KEY_1, '1' },
	{ KEY_2, '2' }, { KEY_3, '3' }, { KEY_4, '4' }, { KEY_5, '5' },
	{ KEY_6, '6' }, { KEY_7, '7' }, { KEY_8, '8' }, { KEY_9, '9' },
	{ KEY_GRAVE, '`' }, { KEY_MINUS, '-' }, { KEY_EQUAL, '=' },
	{ KEY_LEFTBRACE, '[' }, { KEY_RIGHTBRACE, ']' },
	{ KEY_BACKSLASH, '\\' }, { KEY_SEMICOLON, ';' },
	{ KEY_APOSTROPHE, '\'' }, { KEY_COMMA, ',' }, { KEY_DOT, '.' },
	{ KEY_SLASH, '/' },
};

/*
 * A keyboard has events: a key pressed with the Super key held goes to the
 * global table. Reading does not take the key from the desktop, so the
 * focused program sees it too. Repeats (value 2) are skipped.
 */
static void
desktop_keyboard_read(int fd, __unused short events, __unused void *arg)
{
	struct input_event	ev[64];
	key_code		key, modifiers;
	ssize_t			n;
	size_t			i, j;
	u_short			code;
	u_long			bit, *held;

	n = read(fd, ev, sizeof ev);
	if (n <= 0) {
		if (n == -1 && errno == ENODEV) {
			for (i = 0; i < desktop_nkeyboards; i++) {
				if (desktop_keyboards[i].fd == fd) {
					desktop_keyboard_close(i);
					break;
				}
			}
		}
		return;
	}
	for (i = 0; i < (size_t)n / sizeof *ev; i++) {
		if (ev[i].type != EV_KEY || ev[i].code > KEY_MAX)
			continue;
		code = ev[i].code;
		for (j = 0; j < nitems(desktop_modifier_codes); j++) {
			if (desktop_modifier_codes[j] == code)
				break;
		}
		if (j != nitems(desktop_modifier_codes)) {
			held = &desktop_global_held[code / (8 * sizeof *held)];
			bit = 1UL << (code % (8 * sizeof *held));
			if (ev[i].value == 0)
				*held &= ~bit;
			else
				*held |= bit;
			continue;
		}
		if (ev[i].value != 1)
			continue;
		for (j = 0; j < nitems(desktop_key_chars); j++) {
			if (desktop_key_chars[j].code == ev[i].code)
				break;
		}
		if (j == nitems(desktop_key_chars))
			continue;
		modifiers = desktop_modifiers(desktop_global_held);
		if (~modifiers & KEYC_SUPER)
			continue;
		key = (key_code)desktop_key_chars[j].ch | modifiers;
		key_bindings_global(key, -1);
	}
}

/* Look for keyboards plugged in since the last look. */
static void
desktop_rescan(__unused int fd, __unused short events, __unused void *arg)
{
	struct timeval	tv = { .tv_sec = DESKTOP_RESCAN_SECONDS };

	desktop_keyboards_scan();
	evtimer_add(&desktop_rescan_timer, &tv);
}

/*
 * Watch the keyboards, once, when the global table first gets a key. Every
 * key is seen, so there is nothing to do for each one.
 */
void
desktop_global_key_add(__unused key_code key)
{
	u_int	i;

	if (desktop_global_started)
		return;
	desktop_global_started = 1;
	desktop_keys_held(desktop_global_held, sizeof desktop_global_held);
	for (i = 0; i < desktop_nkeyboards; i++)
		desktop_keyboard_watch(&desktop_keyboards[i]);
	evtimer_set(&desktop_rescan_timer, desktop_rescan, NULL);
	desktop_rescan(-1, 0, NULL);
}

/* Every key is seen, so a key leaving the global table changes nothing. */
void
desktop_global_key_remove(__unused key_code key)
{
}
