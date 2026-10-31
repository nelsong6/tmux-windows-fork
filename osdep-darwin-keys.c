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
 * The macOS keyboard of the desktop the server runs on: held modifiers for
 * switch-client -H, and the global key table. Neither needs a permission
 * (Input Monitoring or Accessibility): modifier state is read from the HID
 * system, and global keys are hotkeys registered with the system, one per
 * key bound in the global table.
 */

#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include <Carbon/Carbon.h>

#include <sys/types.h>

#include <ctype.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

#include "tmux.h"

/* Are all the modifiers in a key still physically held? */
int
desktop_modifiers_held(key_code modifiers)
{
	CGEventFlags	flags;

	flags = CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState);
	if ((modifiers & KEYC_CTRL) && !(flags & kCGEventFlagMaskControl))
		return (0);
	if ((modifiers & KEYC_META) && !(flags & kCGEventFlagMaskAlternate))
		return (0);
	if ((modifiers & KEYC_SHIFT) && !(flags & kCGEventFlagMaskShift))
		return (0);
	return (1);
}

/*
 * Hotkeys are registered, delivered and unregistered on one thread of their
 * own, which runs a Carbon event loop; the server's event loop is libevent on
 * the main thread. A press goes to the main thread through a pipe. The main
 * thread marks the keys it wants in desktop_keys, and the hotkey thread
 * registers and unregisters them between events.
 */
#define DESKTOP_KEYS_MAX 64

struct desktop_key {
	key_code	 key;
	EventHotKeyRef	 ref;
	int		 wanted;
};
static struct desktop_key	desktop_keys[DESKTOP_KEYS_MAX];
static pthread_mutex_t		desktop_keys_lock = PTHREAD_MUTEX_INITIALIZER;
static int			desktop_keys_pipe[2] = { -1, -1 };
static struct event		desktop_keys_event;

/* The key code on a US layout of each character a global key can type. */
static const struct {
	char	ch;
	UInt32	code;
} desktop_key_codes[] = {
	{ 'a', kVK_ANSI_A }, { 'b', kVK_ANSI_B }, { 'c', kVK_ANSI_C },
	{ 'd', kVK_ANSI_D }, { 'e', kVK_ANSI_E }, { 'f', kVK_ANSI_F },
	{ 'g', kVK_ANSI_G }, { 'h', kVK_ANSI_H }, { 'i', kVK_ANSI_I },
	{ 'j', kVK_ANSI_J }, { 'k', kVK_ANSI_K }, { 'l', kVK_ANSI_L },
	{ 'm', kVK_ANSI_M }, { 'n', kVK_ANSI_N }, { 'o', kVK_ANSI_O },
	{ 'p', kVK_ANSI_P }, { 'q', kVK_ANSI_Q }, { 'r', kVK_ANSI_R },
	{ 's', kVK_ANSI_S }, { 't', kVK_ANSI_T }, { 'u', kVK_ANSI_U },
	{ 'v', kVK_ANSI_V }, { 'w', kVK_ANSI_W }, { 'x', kVK_ANSI_X },
	{ 'y', kVK_ANSI_Y }, { 'z', kVK_ANSI_Z }, { '0', kVK_ANSI_0 },
	{ '1', kVK_ANSI_1 }, { '2', kVK_ANSI_2 }, { '3', kVK_ANSI_3 },
	{ '4', kVK_ANSI_4 }, { '5', kVK_ANSI_5 }, { '6', kVK_ANSI_6 },
	{ '7', kVK_ANSI_7 }, { '8', kVK_ANSI_8 }, { '9', kVK_ANSI_9 },
	{ '`', kVK_ANSI_Grave }, { '-', kVK_ANSI_Minus },
	{ '=', kVK_ANSI_Equal }, { '[', kVK_ANSI_LeftBracket },
	{ ']', kVK_ANSI_RightBracket }, { '\\', kVK_ANSI_Backslash },
	{ ';', kVK_ANSI_Semicolon }, { '\'', kVK_ANSI_Quote },
	{ ',', kVK_ANSI_Comma }, { '.', kVK_ANSI_Period },
	{ '/', kVK_ANSI_Slash },
};

/* The key code and Carbon modifiers for a key, or 0 if it can't be one. */
static int
desktop_key_hotkey(key_code key, UInt32 *code, UInt32 *mods)
{
	key_code	ch = key & KEYC_MASK_KEY;
	u_int		i;

	if (~key & KEYC_SUPER)
		return (0);
	for (i = 0; i < nitems(desktop_key_codes); i++) {
		if ((key_code)desktop_key_codes[i].ch == ch)
			break;
	}
	if (i == nitems(desktop_key_codes))
		return (0);
	*code = desktop_key_codes[i].code;
	*mods = cmdKey;
	if (key & KEYC_CTRL)
		*mods |= controlKey;
	if (key & KEYC_META)
		*mods |= optionKey;
	if (key & KEYC_SHIFT)
		*mods |= shiftKey;
	return (1);
}

/* A hotkey was pressed: its id is its slot. Runs on the hotkey thread. */
static OSStatus
desktop_keys_pressed(__unused EventHandlerCallRef handler, EventRef event,
    __unused void *data)
{
	EventHotKeyID	id;
	key_code	key = KEYC_NONE;

	if (GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
	    NULL, sizeof id, NULL, &id) != noErr)
		return (noErr);
	pthread_mutex_lock(&desktop_keys_lock);
	if (id.id < DESKTOP_KEYS_MAX && desktop_keys[id.id].ref != NULL)
		key = desktop_keys[id.id].key;
	pthread_mutex_unlock(&desktop_keys_lock);
	if (key != KEYC_NONE)
		write(desktop_keys_pipe[1], &key, sizeof key);
	return (noErr);
}

/* Register and unregister hotkeys to match the wanted keys. */
static void
desktop_keys_update(void)
{
	struct desktop_key	*dk;
	EventHotKeyID		 id = { .signature = 'tmux' };
	UInt32			 code, mods;
	u_int			 i;

	pthread_mutex_lock(&desktop_keys_lock);
	for (i = 0; i < DESKTOP_KEYS_MAX; i++) {
		dk = &desktop_keys[i];
		if (!dk->wanted && dk->ref != NULL) {
			UnregisterEventHotKey(dk->ref);
			dk->ref = NULL;
			dk->key = KEYC_NONE;
		} else if (dk->wanted && dk->ref == NULL) {
			id.id = i;
			if (!desktop_key_hotkey(dk->key, &code, &mods) ||
			    RegisterEventHotKey(code, mods, id,
			    GetEventDispatcherTarget(), 0, &dk->ref) != noErr) {
				log_debug("%s: can't register %s", __func__,
				    key_string_lookup_key(dk->key, 0));
				dk->ref = NULL;
				dk->wanted = 0;
				dk->key = KEYC_NONE;
			}
		}
	}
	pthread_mutex_unlock(&desktop_keys_lock);
}

static void *
desktop_keys_thread(__unused void *arg)
{
	EventTypeSpec	spec = { kEventClassKeyboard, kEventHotKeyPressed };
	EventRef	event;

	InstallEventHandler(GetEventDispatcherTarget(), desktop_keys_pressed, 1,
	    &spec, NULL, NULL);
	for (;;) {
		desktop_keys_update();
		if (ReceiveNextEvent(0, NULL, 0.1, true, &event) == noErr) {
			SendEventToEventTarget(event, GetEventDispatcherTarget());
			ReleaseEvent(event);
		}
	}
	return (NULL);
}

static void
desktop_keys_read(int fd, __unused short events, __unused void *arg)
{
	key_code	key;

	if (read(fd, &key, sizeof key) == sizeof key)
		key_bindings_global(key);
}

/* Start the hotkey thread, once, when the global table first gets a key. */
static void
desktop_keys_start(void)
{
	pthread_t	thread;

	if (desktop_keys_pipe[0] != -1)
		return;
	if (pipe(desktop_keys_pipe) != 0)
		return;
	setblocking(desktop_keys_pipe[0], 0);
	event_set(&desktop_keys_event, desktop_keys_pipe[0],
	    EV_READ|EV_PERSIST, desktop_keys_read, NULL);
	event_add(&desktop_keys_event, NULL);
	if (pthread_create(&thread, NULL, desktop_keys_thread, NULL) == 0)
		pthread_detach(thread);
}

/* A key was bound in the global table: register it as a hotkey. */
void
desktop_global_key_add(key_code key)
{
	u_int	i, free_slot = DESKTOP_KEYS_MAX;

	if (!desktop_key_hotkey(key, &(UInt32){0}, &(UInt32){0}))
		return;
	desktop_keys_start();
	pthread_mutex_lock(&desktop_keys_lock);
	for (i = 0; i < DESKTOP_KEYS_MAX; i++) {
		if (desktop_keys[i].wanted && desktop_keys[i].key == key)
			break;
		if (!desktop_keys[i].wanted && desktop_keys[i].ref == NULL &&
		    free_slot == DESKTOP_KEYS_MAX)
			free_slot = i;
	}
	if (i == DESKTOP_KEYS_MAX && free_slot != DESKTOP_KEYS_MAX) {
		desktop_keys[free_slot].key = key;
		desktop_keys[free_slot].wanted = 1;
	}
	pthread_mutex_unlock(&desktop_keys_lock);
}

/* A key left the global table: unregister its hotkey. */
void
desktop_global_key_remove(key_code key)
{
	u_int	i;

	pthread_mutex_lock(&desktop_keys_lock);
	for (i = 0; i < DESKTOP_KEYS_MAX; i++) {
		if (desktop_keys[i].wanted && desktop_keys[i].key == key)
			desktop_keys[i].wanted = 0;
	}
	pthread_mutex_unlock(&desktop_keys_lock);
}
