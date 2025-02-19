#include <assert.h>
#include <limits.h>
#include <strings.h>
#include <wlr/backend/multi.h>
#include <wlr/backend/session.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_idle.h>
#include <wlr/types/wlr_keyboard_group.h>
#include <xkbcommon/xkbcommon-names.h>
#include "sway/commands.h"
#include "sway/desktop/transaction.h"
#include "sway/input/input-manager.h"
#include "sway/input/keyboard.h"
#include "sway/input/seat.h"
#include "sway/ipc-server.h"
#include "log.h"

static struct modifier_key {
	char *name;
	uint32_t mod;
} modifiers[] = {
	{ XKB_MOD_NAME_SHIFT, WLR_MODIFIER_SHIFT },
	{ XKB_MOD_NAME_CAPS, WLR_MODIFIER_CAPS },
	{ XKB_MOD_NAME_CTRL, WLR_MODIFIER_CTRL },
	{ "Ctrl", WLR_MODIFIER_CTRL },
	{ XKB_MOD_NAME_ALT, WLR_MODIFIER_ALT },
	{ "Alt", WLR_MODIFIER_ALT },
	{ XKB_MOD_NAME_NUM, WLR_MODIFIER_MOD2 },
	{ "Mod3", WLR_MODIFIER_MOD3 },
	{ XKB_MOD_NAME_LOGO, WLR_MODIFIER_LOGO },
	{ "Mod5", WLR_MODIFIER_MOD5 },
};

uint32_t get_modifier_mask_by_name(const char *name) {
	int i;
	for (i = 0; i < (int)(sizeof(modifiers) / sizeof(struct modifier_key)); ++i) {
		if (strcasecmp(modifiers[i].name, name) == 0) {
			return modifiers[i].mod;
		}
	}

	return 0;
}

const char *get_modifier_name_by_mask(uint32_t modifier) {
	int i;
	for (i = 0; i < (int)(sizeof(modifiers) / sizeof(struct modifier_key)); ++i) {
		if (modifiers[i].mod == modifier) {
			return modifiers[i].name;
		}
	}

	return NULL;
}

int get_modifier_names(const char **names, uint32_t modifier_masks) {
	int length = 0;
	int i;
	for (i = 0; i < (int)(sizeof(modifiers) / sizeof(struct modifier_key)); ++i) {
		if ((modifier_masks & modifiers[i].mod) != 0) {
			names[length] = modifiers[i].name;
			++length;
			modifier_masks ^= modifiers[i].mod;
		}
	}

	return length;
}

/**
 * Remove all key ids associated to a keycode from the list of pressed keys
 */
static bool state_erase_key(struct sway_shortcut_state *state,
		uint32_t keycode) {
	bool found = false;
	size_t j = 0;
	for (size_t i = 0; i < state->npressed; ++i) {
		if (i > j) {
			state->pressed_keys[j] = state->pressed_keys[i];
			state->pressed_keycodes[j] = state->pressed_keycodes[i];
		}
		if (state->pressed_keycodes[i] != keycode) {
			++j;
		} else {
			found = true;
		}
	}
	while(state->npressed > j) {
		--state->npressed;
		state->pressed_keys[state->npressed] = 0;
		state->pressed_keycodes[state->npressed] = 0;
	}
	state->current_key = 0;
	return found;
}

/**
 * Add a key id (with associated keycode) to the list of pressed keys,
 * if the list is not full.
 */
static void state_add_key(struct sway_shortcut_state *state,
		uint32_t keycode, uint32_t key_id) {
	if (state->npressed >= SWAY_KEYBOARD_PRESSED_KEYS_CAP) {
		return;
	}
	size_t i = 0;
	while (i < state->npressed && state->pressed_keys[i] < key_id) {
		++i;
	}
	size_t j = state->npressed;
	while (j > i) {
		state->pressed_keys[j] = state->pressed_keys[j - 1];
		state->pressed_keycodes[j] = state->pressed_keycodes[j - 1];
		--j;
	}
	state->pressed_keys[i] = key_id;
	state->pressed_keycodes[i] = keycode;
	state->npressed++;
	state->current_key = key_id;
}

/**
 * Update the shortcut model state in response to new input
 */
static bool update_shortcut_state(struct sway_shortcut_state *state,
		struct wlr_event_keyboard_key *event, uint32_t new_key,
		uint32_t raw_modifiers) {
	bool last_key_was_a_modifier = raw_modifiers != state->last_raw_modifiers;
	state->last_raw_modifiers = raw_modifiers;

	if (last_key_was_a_modifier && state->last_keycode) {
		// Last pressed key before this one was a modifier
		state_erase_key(state, state->last_keycode);
	}

	if (event->state == WLR_KEY_PRESSED) {
		// Add current key to set; there may be duplicates
		state_add_key(state, event->keycode, new_key);
		state->last_keycode = event->keycode;
	} else {
		return state_erase_key(state, event->keycode);
	}

	return false;
}

/**
 * If one exists, finds a binding which matches the shortcut model state,
 * current modifiers, release state, and locked state.
 */
static void get_active_binding(const struct sway_shortcut_state *state,
		list_t *bindings, struct sway_binding **current_binding,
		uint32_t modifiers, bool release, bool locked, const char *input,
		bool exact_input, xkb_layout_index_t group) {
	for (int i = 0; i < bindings->length; ++i) {
		struct sway_binding *binding = bindings->items[i];
		bool binding_locked = (binding->flags & BINDING_LOCKED) != 0;
		bool binding_release = binding->flags & BINDING_RELEASE;

		if (modifiers ^ binding->modifiers ||
				release != binding_release ||
				locked > binding_locked ||
				(binding->group != XKB_LAYOUT_INVALID &&
				 binding->group != group) ||
				(strcmp(binding->input, input) != 0 &&
				 (strcmp(binding->input, "*") != 0 || exact_input))) {
			continue;
		}

		bool match = false;
		if (state->npressed == (size_t)binding->keys->length) {
			match = true;
			for (size_t j = 0; j < state->npressed; j++) {
				uint32_t key = *(uint32_t *)binding->keys->items[j];
				if (key != state->pressed_keys[j]) {
					match = false;
					break;
				}
			}
		} else if (binding->keys->length == 1) {
			/*
			 * If no multiple-key binding has matched, try looking for
			 * single-key bindings that match the newly-pressed key.
			 */
			match = state->current_key == *(uint32_t *)binding->keys->items[0];
		}
		if (!match) {
			continue;
		}

		if (*current_binding) {
			if (*current_binding == binding) {
				continue;
			}

			bool current_locked =
				((*current_binding)->flags & BINDING_LOCKED) != 0;
			bool current_input = strcmp((*current_binding)->input, input) == 0;
			bool current_group_set =
				(*current_binding)->group != XKB_LAYOUT_INVALID;
			bool binding_input = strcmp(binding->input, input) == 0;
			bool binding_group_set = binding->group != XKB_LAYOUT_INVALID;

			if (current_input == binding_input
					&& current_locked == binding_locked
					&& current_group_set == binding_group_set) {
				sway_log(SWAY_DEBUG,
						"Encountered conflicting bindings %d and %d",
						(*current_binding)->order, binding->order);
				continue;
			}

			if (current_input && !binding_input) {
				continue; // Prefer the correct input
			}

			if (current_input == binding_input &&
				   (*current_binding)->group == group) {
				continue; // Prefer correct group for matching inputs
			}

			if (current_input == binding_input &&
					current_group_set == binding_group_set &&
					current_locked == locked) {
				continue; // Prefer correct lock state for matching input+group
			}
		}

		*current_binding = binding;
		if (strcmp((*current_binding)->input, input) == 0 &&
				(((*current_binding)->flags & BINDING_LOCKED) == locked) &&
				(*current_binding)->group == group) {
			return; // If a perfect match is found, quit searching
		}
	}
}

/**
 * Execute a built-in, hardcoded compositor binding. These are triggered from a
 * single keysym.
 *
 * Returns true if the keysym was handled by a binding and false if the event
 * should be propagated to clients.
 */
static bool keyboard_execute_compositor_binding(struct sway_keyboard *keyboard,
		const xkb_keysym_t *pressed_keysyms, uint32_t modifiers, size_t keysyms_len) {
	for (size_t i = 0; i < keysyms_len; ++i) {
		xkb_keysym_t keysym = pressed_keysyms[i];
		if (keysym >= XKB_KEY_XF86Switch_VT_1 &&
				keysym <= XKB_KEY_XF86Switch_VT_12) {
			if (wlr_backend_is_multi(server.backend)) {
				struct wlr_session *session =
					wlr_backend_get_session(server.backend);
				if (session) {
					unsigned vt = keysym - XKB_KEY_XF86Switch_VT_1 + 1;
					wlr_session_change_vt(session, vt);
				}
			}
			return true;
		}
	}

	return false;
}

/**
 * Get keysyms and modifiers from the keyboard as xkb sees them.
 *
 * This uses the xkb keysyms translation based on pressed modifiers and clears
 * the consumed modifiers from the list of modifiers passed to keybind
 * detection.
 *
 * On US layout, pressing Alt+Shift+2 will trigger Alt+@.
 */
static size_t keyboard_keysyms_translated(struct sway_keyboard *keyboard,
		xkb_keycode_t keycode, const xkb_keysym_t **keysyms,
		uint32_t *modifiers) {
	struct wlr_input_device *device =
		keyboard->seat_device->input_device->wlr_device;
	*modifiers = wlr_keyboard_get_modifiers(device->keyboard);
	xkb_mod_mask_t consumed = xkb_state_key_get_consumed_mods2(
		device->keyboard->xkb_state, keycode, XKB_CONSUMED_MODE_XKB);
	*modifiers = *modifiers & ~consumed;

	return xkb_state_key_get_syms(device->keyboard->xkb_state,
		keycode, keysyms);
}

/**
 * Get keysyms and modifiers from the keyboard as if modifiers didn't change
 * keysyms.
 *
 * This avoids the xkb keysym translation based on modifiers considered pressed
 * in the state.
 *
 * This will trigger keybinds such as Alt+Shift+2.
 */
static size_t keyboard_keysyms_raw(struct sway_keyboard *keyboard,
		xkb_keycode_t keycode, const xkb_keysym_t **keysyms,
		uint32_t *modifiers) {
	struct wlr_input_device *device =
		keyboard->seat_device->input_device->wlr_device;
	*modifiers = wlr_keyboard_get_modifiers(device->keyboard);

	xkb_layout_index_t layout_index = xkb_state_key_get_layout(
		device->keyboard->xkb_state, keycode);
	return xkb_keymap_key_get_syms_by_level(device->keyboard->keymap,
		keycode, layout_index, 0, keysyms);
}

void sway_keyboard_disarm_key_repeat(struct sway_keyboard *keyboard) {
	if (!keyboard) {
		return;
	}
	keyboard->repeat_binding = NULL;
	if (wl_event_source_timer_update(keyboard->key_repeat_source, 0) < 0) {
		sway_log(SWAY_DEBUG, "failed to disarm key repeat timer");
	}
}

static void handle_key_event(struct sway_keyboard *keyboard,
		struct wlr_event_keyboard_key *event) {
	struct sway_seat* seat = keyboard->seat_device->sway_seat;
	struct wlr_seat *wlr_seat = seat->wlr_seat;
	struct wlr_input_device *wlr_device =
		keyboard->seat_device->input_device->wlr_device;
	char *device_identifier = input_device_get_identifier(wlr_device);
	bool exact_identifier = wlr_device->keyboard->group != NULL;
	seat_idle_notify_activity(seat, IDLE_SOURCE_KEYBOARD);
	bool input_inhibited = seat->exclusive_client != NULL;

	// Identify new keycode, raw keysym(s), and translated keysym(s)
	xkb_keycode_t keycode = event->keycode + 8;

	const xkb_keysym_t *raw_keysyms;
	uint32_t raw_modifiers;
	size_t raw_keysyms_len =
		keyboard_keysyms_raw(keyboard, keycode, &raw_keysyms, &raw_modifiers);

	const xkb_keysym_t *translated_keysyms;
	uint32_t translated_modifiers;
	size_t translated_keysyms_len =
		keyboard_keysyms_translated(keyboard, keycode, &translated_keysyms,
			&translated_modifiers);

	uint32_t code_modifiers = wlr_keyboard_get_modifiers(wlr_device->keyboard);

	// Update shortcut model state
	update_shortcut_state(&keyboard->state_keycodes, event,
			(uint32_t)keycode, code_modifiers);
	for (size_t i = 0; i < raw_keysyms_len; ++i) {
		update_shortcut_state(&keyboard->state_keysyms_raw,
				event, (uint32_t)raw_keysyms[i],
				code_modifiers);
	}
	for (size_t i = 0; i < translated_keysyms_len; ++i) {
		update_shortcut_state(&keyboard->state_keysyms_translated,
				event, (uint32_t)translated_keysyms[i],
				code_modifiers);
	}

	bool handled = false;
	// Identify active release binding
	struct sway_binding *binding_released = NULL;
	get_active_binding(&keyboard->state_keycodes,
			config->current_mode->keycode_bindings, &binding_released,
			code_modifiers, true, input_inhibited, device_identifier,
			exact_identifier, keyboard->effective_layout);
	get_active_binding(&keyboard->state_keysyms_raw,
			config->current_mode->keysym_bindings, &binding_released,
			raw_modifiers, true, input_inhibited, device_identifier,
			exact_identifier, keyboard->effective_layout);
	get_active_binding(&keyboard->state_keysyms_translated,
			config->current_mode->keysym_bindings, &binding_released,
			translated_modifiers, true, input_inhibited, device_identifier,
			exact_identifier, keyboard->effective_layout);

	// Execute stored release binding once no longer active
	if (keyboard->held_binding && binding_released != keyboard->held_binding &&
			event->state == WLR_KEY_RELEASED) {
		seat_execute_command(seat, keyboard->held_binding);
		handled = true;
	}
	if (binding_released != keyboard->held_binding) {
		keyboard->held_binding = NULL;
	}
	if (binding_released && event->state == WLR_KEY_PRESSED) {
		keyboard->held_binding = binding_released;
	}

	// Identify and execute active pressed binding
	struct sway_binding *binding = NULL;
	if (event->state == WLR_KEY_PRESSED) {
		get_active_binding(&keyboard->state_keycodes,
				config->current_mode->keycode_bindings, &binding,
				code_modifiers, false, input_inhibited, device_identifier,
				exact_identifier, keyboard->effective_layout);
		get_active_binding(&keyboard->state_keysyms_raw,
				config->current_mode->keysym_bindings, &binding,
				raw_modifiers, false, input_inhibited, device_identifier,
				exact_identifier, keyboard->effective_layout);
		get_active_binding(&keyboard->state_keysyms_translated,
				config->current_mode->keysym_bindings, &binding,
				translated_modifiers, false, input_inhibited,
				device_identifier, exact_identifier,
				keyboard->effective_layout);
	}

	// Set up (or clear) keyboard repeat for a pressed binding. Since the
	// binding may remove the keyboard, the timer needs to be updated first
	if (binding && wlr_device->keyboard->repeat_info.delay > 0) {
		keyboard->repeat_binding = binding;
		if (wl_event_source_timer_update(keyboard->key_repeat_source,
				wlr_device->keyboard->repeat_info.delay) < 0) {
			sway_log(SWAY_DEBUG, "failed to set key repeat timer");
		}
	} else if (keyboard->repeat_binding) {
		sway_keyboard_disarm_key_repeat(keyboard);
	}

	if (binding) {
		seat_execute_command(seat, binding);
		handled = true;
	}

	if (!handled && wlr_device->keyboard->group) {
		// Only handle device specific bindings for keyboards in a group
		free(device_identifier);
		return;
	}

	// Compositor bindings
	if (!handled && event->state == WLR_KEY_PRESSED) {
		handled = keyboard_execute_compositor_binding(
				keyboard, translated_keysyms, translated_modifiers,
				translated_keysyms_len);
	}
	if (!handled && event->state == WLR_KEY_PRESSED) {
		handled = keyboard_execute_compositor_binding(
				keyboard, raw_keysyms, raw_modifiers,
				raw_keysyms_len);
	}

	if (!handled || event->state == WLR_KEY_RELEASED) {
		bool pressed_sent = update_shortcut_state(
				&keyboard->state_pressed_sent, event, (uint32_t)keycode, 0);
		if (pressed_sent || event->state == WLR_KEY_PRESSED) {
			wlr_seat_set_keyboard(wlr_seat, wlr_device);
			wlr_seat_keyboard_notify_key(wlr_seat, event->time_msec,
					event->keycode, event->state);
		}
	}

	transaction_commit_dirty();

	free(device_identifier);
}

static void handle_keyboard_key(struct wl_listener *listener, void *data) {
	struct sway_keyboard *keyboard =
		wl_container_of(listener, keyboard, keyboard_key);
	handle_key_event(keyboard, data);
}

static void handle_keyboard_group_key(struct wl_listener *listener,
		void *data) {
	struct sway_keyboard_group *sway_group =
		wl_container_of(listener, sway_group, keyboard_key);
	handle_key_event(sway_group->seat_device->keyboard, data);
}

static int handle_keyboard_repeat(void *data) {
	struct sway_keyboard *keyboard = (struct sway_keyboard *)data;
	struct wlr_keyboard *wlr_device =
			keyboard->seat_device->input_device->wlr_device->keyboard;
	if (keyboard->repeat_binding) {
		if (wlr_device->repeat_info.rate > 0) {
			// We queue the next event first, as the command might cancel it
			if (wl_event_source_timer_update(keyboard->key_repeat_source,
					1000 / wlr_device->repeat_info.rate) < 0) {
				sway_log(SWAY_DEBUG, "failed to update key repeat timer");
			}
		}

		seat_execute_command(keyboard->seat_device->sway_seat,
				keyboard->repeat_binding);
		transaction_commit_dirty();
	}
	return 0;
}

static void determine_bar_visibility(uint32_t modifiers) {
	for (int i = 0; i < config->bars->length; ++i) {
		struct bar_config *bar = config->bars->items[i];
		if (bar->modifier == 0) {
			continue;
		}

		bool vis_by_mod = (~modifiers & bar->modifier) == 0;
		if (bar->visible_by_modifier != vis_by_mod) {
			// If visible by modifier is set, send that it is no longer visible
			// by modifier (regardless of bar mode and state). Otherwise, only
			// send the visible by modifier status if mode and state are hide
			if (bar->visible_by_modifier
					|| strcmp(bar->mode, bar->hidden_state) == 0) {
				bar->visible_by_modifier = vis_by_mod;
				ipc_event_bar_state_update(bar);
			}
		}
	}
}

static void handle_modifier_event(struct sway_keyboard *keyboard) {
	struct wlr_input_device *wlr_device =
		keyboard->seat_device->input_device->wlr_device;
	if (!wlr_device->keyboard->group) {
		struct wlr_seat *wlr_seat = keyboard->seat_device->sway_seat->wlr_seat;
		wlr_seat_set_keyboard(wlr_seat, wlr_device);
		wlr_seat_keyboard_notify_modifiers(wlr_seat,
				&wlr_device->keyboard->modifiers);

		uint32_t modifiers = wlr_keyboard_get_modifiers(wlr_device->keyboard);
		determine_bar_visibility(modifiers);
	}

	if (wlr_device->keyboard->modifiers.group != keyboard->effective_layout &&
			!wlr_keyboard_group_from_wlr_keyboard(wlr_device->keyboard)) {
		keyboard->effective_layout = wlr_device->keyboard->modifiers.group;
		ipc_event_input("xkb_layout", keyboard->seat_device->input_device);
	}
}

static void handle_keyboard_modifiers(struct wl_listener *listener,
		void *data) {
	struct sway_keyboard *keyboard =
		wl_container_of(listener, keyboard, keyboard_modifiers);
	handle_modifier_event(keyboard);
}

static void handle_keyboard_group_modifiers(struct wl_listener *listener,
		void *data) {
	struct sway_keyboard_group *group =
		wl_container_of(listener, group, keyboard_modifiers);
	handle_modifier_event(group->seat_device->keyboard);
}

struct sway_keyboard *sway_keyboard_create(struct sway_seat *seat,
		struct sway_seat_device *device) {
	struct sway_keyboard *keyboard =
		calloc(1, sizeof(struct sway_keyboard));
	if (!sway_assert(keyboard, "could not allocate sway keyboard")) {
		return NULL;
	}

	keyboard->seat_device = device;
	device->keyboard = keyboard;

	wl_list_init(&keyboard->keyboard_key.link);
	wl_list_init(&keyboard->keyboard_modifiers.link);

	keyboard->key_repeat_source = wl_event_loop_add_timer(server.wl_event_loop,
			handle_keyboard_repeat, keyboard);

	return keyboard;
}

static void handle_xkb_context_log(struct xkb_context *context,
		enum xkb_log_level level, const char *format, va_list args) {
	va_list args_copy;
	va_copy(args_copy, args);
	size_t length = vsnprintf(NULL, 0, format, args_copy) + 1;
	va_end(args_copy);

	char *error = malloc(length);
	if (!error) {
		sway_log(SWAY_ERROR, "Failed to allocate libxkbcommon log message");
		return;
	}

	va_copy(args_copy, args);
	vsnprintf(error, length, format, args_copy);
	va_end(args_copy);

	if (error[length - 2] == '\n') {
		error[length - 2] = '\0';
	}

	sway_log_importance_t importance = SWAY_DEBUG;
	if (level <= XKB_LOG_LEVEL_ERROR) { // Critical and Error
		importance = SWAY_ERROR;
	} else if (level <= XKB_LOG_LEVEL_INFO) { // Warning and Info
		importance = SWAY_INFO;
	}
	sway_log(importance, "[xkbcommon] %s", error);

	char **data = xkb_context_get_user_data(context);
	if (importance == SWAY_ERROR && data && !*data) {
		*data = error;
	} else {
		free(error);
	}
}

struct xkb_keymap *sway_keyboard_compile_keymap(struct input_config *ic,
		char **error) {
	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!sway_assert(context, "cannot create XKB context")) {
		return NULL;
	}
	xkb_context_set_user_data(context, error);
	xkb_context_set_log_fn(context, handle_xkb_context_log);

	struct xkb_keymap *keymap = NULL;

	if (ic && ic->xkb_file) {
		FILE *keymap_file = fopen(ic->xkb_file, "r");
		if (!keymap_file) {
			sway_log_errno(SWAY_ERROR, "cannot read xkb file %s", ic->xkb_file);
			if (error) {
				size_t len = snprintf(NULL, 0, "cannot read xkb file %s: %s",
						ic->xkb_file, strerror(errno)) + 1;
				*error = malloc(len);
				if (*error) {
					snprintf(*error, len, "cannot read xkb_file %s: %s",
							ic->xkb_file, strerror(errno));
				}
			}
			goto cleanup;
		}

		keymap = xkb_keymap_new_from_file(context, keymap_file,
					XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);

		if (fclose(keymap_file) != 0) {
			sway_log_errno(SWAY_ERROR, "Failed to close xkb file %s",
					ic->xkb_file);
		}
	} else {
		struct xkb_rule_names rules = {0};
		if (ic) {
			input_config_fill_rule_names(ic, &rules);
		}
		keymap = xkb_keymap_new_from_names(context, &rules,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
	}

cleanup:
	xkb_context_set_user_data(context, NULL);
	xkb_context_unref(context);
	return keymap;
}

static bool keymaps_match(struct xkb_keymap *km1, struct xkb_keymap *km2) {
	char *km1_str = xkb_keymap_get_as_string(km1, XKB_KEYMAP_FORMAT_TEXT_V1);
	char *km2_str = xkb_keymap_get_as_string(km2, XKB_KEYMAP_FORMAT_TEXT_V1);
	bool result = strcmp(km1_str, km2_str) == 0;
	free(km1_str);
	free(km2_str);
	return result;
}

static void sway_keyboard_group_remove(struct sway_keyboard *keyboard) {
	struct sway_input_device *device = keyboard->seat_device->input_device;
	struct wlr_keyboard *wlr_keyboard = device->wlr_device->keyboard;
	struct wlr_keyboard_group *wlr_group = wlr_keyboard->group;

	sway_log(SWAY_DEBUG, "Removing keyboard %s from group %p",
			device->identifier, wlr_group);

	wlr_keyboard_group_remove_keyboard(wlr_keyboard->group, wlr_keyboard);

	if (wl_list_empty(&wlr_group->devices)) {
		sway_log(SWAY_DEBUG, "Destroying empty keyboard group %p",
				wlr_group);
		struct sway_keyboard_group *sway_group = wlr_group->data;
		wlr_group->data = NULL;
		wl_list_remove(&sway_group->link);
		sway_keyboard_destroy(sway_group->seat_device->keyboard);
		free(sway_group->seat_device->input_device);
		free(sway_group->seat_device);
		free(sway_group);
		wlr_keyboard_group_destroy(wlr_group);
	}
}

static void sway_keyboard_group_remove_invalid(struct sway_keyboard *keyboard) {
	struct sway_input_device *device = keyboard->seat_device->input_device;
	struct wlr_keyboard *wlr_keyboard = device->wlr_device->keyboard;
	if (!wlr_keyboard->group) {
		return;
	}

	struct sway_seat *seat = keyboard->seat_device->sway_seat;
	struct seat_config *sc = seat_get_config(seat);
	if (!sc) {
		sc = seat_get_config_by_name("*");
	}

	switch (sc ? sc->keyboard_grouping : KEYBOARD_GROUP_DEFAULT) {
	case KEYBOARD_GROUP_NONE:
		sway_keyboard_group_remove(keyboard);
		break;
	case KEYBOARD_GROUP_DEFAULT: /* fallthrough */
	case KEYBOARD_GROUP_KEYMAP:;
		struct wlr_keyboard_group *group = wlr_keyboard->group;
		if (!keymaps_match(keyboard->keymap, group->keyboard.keymap)) {
			sway_keyboard_group_remove(keyboard);
		}
		break;
	}
}

static void sway_keyboard_group_add(struct sway_keyboard *keyboard) {
	struct sway_input_device *device = keyboard->seat_device->input_device;
	struct wlr_keyboard *wlr_keyboard = device->wlr_device->keyboard;
	struct sway_seat *seat = keyboard->seat_device->sway_seat;
	struct seat_config *sc = seat_get_config(seat);
	if (!sc) {
		sc = seat_get_config_by_name("*");
	}

	if (sc && sc->keyboard_grouping == KEYBOARD_GROUP_NONE) {
		// Keyboard grouping is disabled for the seat
		return;
	}

	struct sway_keyboard_group *group;
	wl_list_for_each(group, &seat->keyboard_groups, link) {
		switch (sc ? sc->keyboard_grouping : KEYBOARD_GROUP_DEFAULT) {
		case KEYBOARD_GROUP_NONE:
			// Nothing to do. This shouldn't even be reached
			return;
		case KEYBOARD_GROUP_DEFAULT: /* fallthrough */
		case KEYBOARD_GROUP_KEYMAP:;
			struct wlr_keyboard_group *wlr_group = group->wlr_group;
			if (keymaps_match(keyboard->keymap, wlr_group->keyboard.keymap)) {
				sway_log(SWAY_DEBUG, "Adding keyboard %s to group %p",
						device->identifier, wlr_group);
				wlr_keyboard_group_add_keyboard(wlr_group, wlr_keyboard);
				return;
			}
			break;
		}
	}

	struct sway_keyboard_group *sway_group =
		calloc(1, sizeof(struct sway_keyboard_group));
	if (!sway_group) {
		sway_log(SWAY_ERROR, "Failed to allocate sway_keyboard_group");
		return;
	}

	sway_group->wlr_group = wlr_keyboard_group_create();
	if (!sway_group->wlr_group) {
		sway_log(SWAY_ERROR, "Failed to create keyboard group");
		goto cleanup;
	}
	sway_group->wlr_group->data = sway_group;
	wlr_keyboard_set_keymap(&sway_group->wlr_group->keyboard, keyboard->keymap);
	sway_log(SWAY_DEBUG, "Created keyboard group %p", sway_group->wlr_group);

	sway_group->seat_device = calloc(1, sizeof(struct sway_seat_device));
	if (!sway_group->seat_device) {
		sway_log(SWAY_ERROR, "Failed to allocate sway_seat_device for group");
		goto cleanup;
	}
	sway_group->seat_device->sway_seat = seat;

	sway_group->seat_device->input_device =
		calloc(1, sizeof(struct sway_input_device));
	if (!sway_group->seat_device->input_device) {
		sway_log(SWAY_ERROR, "Failed to allocate sway_input_device for group");
		goto cleanup;
	}
	sway_group->seat_device->input_device->wlr_device =
		sway_group->wlr_group->input_device;

	if (!sway_keyboard_create(seat, sway_group->seat_device)) {
		sway_log(SWAY_ERROR, "Failed to allocate sway_keyboard for group");
		goto cleanup;
	}

	sway_log(SWAY_DEBUG, "Adding keyboard %s to group %p",
			device->identifier, sway_group->wlr_group);
	wlr_keyboard_group_add_keyboard(sway_group->wlr_group, wlr_keyboard);

	wl_list_insert(&seat->keyboard_groups, &sway_group->link);

	wl_signal_add(&sway_group->wlr_group->keyboard.events.key,
			&sway_group->keyboard_key);
	sway_group->keyboard_key.notify = handle_keyboard_group_key;

	wl_signal_add(&sway_group->wlr_group->keyboard.events.modifiers,
			&sway_group->keyboard_modifiers);
	sway_group->keyboard_modifiers.notify = handle_keyboard_group_modifiers;
	return;

cleanup:
	if (sway_group && sway_group->wlr_group) {
		wlr_keyboard_group_destroy(sway_group->wlr_group);
	}
	free(sway_group->seat_device->keyboard);
	free(sway_group->seat_device->input_device);
	free(sway_group->seat_device);
	free(sway_group);
}

void sway_keyboard_configure(struct sway_keyboard *keyboard) {
	struct input_config *input_config =
		input_device_get_config(keyboard->seat_device->input_device);
	struct wlr_input_device *wlr_device =
		keyboard->seat_device->input_device->wlr_device;

	struct xkb_keymap *keymap = sway_keyboard_compile_keymap(input_config, NULL);
	if (!keymap) {
		sway_log(SWAY_ERROR, "Failed to compile keymap. Attempting defaults");
		keymap = sway_keyboard_compile_keymap(NULL, NULL);
		if (!keymap) {
			sway_log(SWAY_ERROR,
					"Failed to compile default keymap. Aborting configure");
			return;
		}
	}

	bool keymap_changed =
		keyboard->keymap ? !keymaps_match(keyboard->keymap, keymap) : true;
	bool effective_layout_changed = keyboard->effective_layout != 0;

	if (keymap_changed || config->reloading) {
		xkb_keymap_unref(keyboard->keymap);
		keyboard->keymap = keymap;
		keyboard->effective_layout = 0;

		sway_keyboard_group_remove_invalid(keyboard);

		wlr_keyboard_set_keymap(wlr_device->keyboard, keyboard->keymap);

		if (!wlr_device->keyboard->group) {
			sway_keyboard_group_add(keyboard);
		}

		xkb_mod_mask_t locked_mods = 0;
		if (input_config && input_config->xkb_numlock > 0) {
			xkb_mod_index_t mod_index = xkb_map_mod_get_index(keymap,
					XKB_MOD_NAME_NUM);
			if (mod_index != XKB_MOD_INVALID) {
			       locked_mods |= (uint32_t)1 << mod_index;
			}
		}
		if (input_config && input_config->xkb_capslock > 0) {
			xkb_mod_index_t mod_index = xkb_map_mod_get_index(keymap,
					XKB_MOD_NAME_CAPS);
			if (mod_index != XKB_MOD_INVALID) {
			       locked_mods |= (uint32_t)1 << mod_index;
			}
		}
		if (locked_mods) {
			wlr_keyboard_notify_modifiers(wlr_device->keyboard, 0, 0,
					locked_mods, 0);
			uint32_t leds = 0;
			for (uint32_t i = 0; i < WLR_LED_COUNT; ++i) {
				if (xkb_state_led_index_is_active(
						wlr_device->keyboard->xkb_state,
						wlr_device->keyboard->led_indexes[i])) {
					leds |= (1 << i);
				}
			}
			if (wlr_device->keyboard->group) {
				wlr_keyboard_led_update(
						&wlr_device->keyboard->group->keyboard, leds);
			} else {
				wlr_keyboard_led_update(wlr_device->keyboard, leds);
			}
		}
	} else {
		xkb_keymap_unref(keymap);
		sway_keyboard_group_remove_invalid(keyboard);
		if (!wlr_device->keyboard->group) {
			sway_keyboard_group_add(keyboard);
		}
	}

	int repeat_rate = 25;
	if (input_config && input_config->repeat_rate != INT_MIN) {
		repeat_rate = input_config->repeat_rate;
	}
	int repeat_delay = 600;
	if (input_config && input_config->repeat_delay != INT_MIN) {
		repeat_delay = input_config->repeat_delay;
	}
	wlr_keyboard_set_repeat_info(wlr_device->keyboard, repeat_rate,
			repeat_delay);

	struct wlr_seat *seat = keyboard->seat_device->sway_seat->wlr_seat;
	wlr_seat_set_keyboard(seat, wlr_device);

	wl_list_remove(&keyboard->keyboard_key.link);
	wl_signal_add(&wlr_device->keyboard->events.key, &keyboard->keyboard_key);
	keyboard->keyboard_key.notify = handle_keyboard_key;

	wl_list_remove(&keyboard->keyboard_modifiers.link);
	wl_signal_add(&wlr_device->keyboard->events.modifiers,
		&keyboard->keyboard_modifiers);
	keyboard->keyboard_modifiers.notify = handle_keyboard_modifiers;

	if (keymap_changed) {
		ipc_event_input("xkb_keymap",
			keyboard->seat_device->input_device);
	} else if (effective_layout_changed) {
		ipc_event_input("xkb_layout",
			keyboard->seat_device->input_device);
	}
}

void sway_keyboard_destroy(struct sway_keyboard *keyboard) {
	if (!keyboard) {
		return;
	}
	if (keyboard->seat_device->input_device->wlr_device->keyboard->group) {
		sway_keyboard_group_remove(keyboard);
	}
	struct wlr_seat *wlr_seat = keyboard->seat_device->sway_seat->wlr_seat;
	struct sway_input_device *device = keyboard->seat_device->input_device;
	if (wlr_seat_get_keyboard(wlr_seat) == device->wlr_device->keyboard) {
		wlr_seat_set_keyboard(wlr_seat, NULL);
	}
	if (keyboard->keymap) {
		xkb_keymap_unref(keyboard->keymap);
	}
	wl_list_remove(&keyboard->keyboard_key.link);
	wl_list_remove(&keyboard->keyboard_modifiers.link);
	sway_keyboard_disarm_key_repeat(keyboard);
	wl_event_source_remove(keyboard->key_repeat_source);
	free(keyboard);
}
