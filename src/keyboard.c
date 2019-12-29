/*
 * Copyright (c) 2019 Andri Yngvason
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 *
 * Acknowledgements: Reading Josef Gajdusek's wvnc code helped me understand
 * how to use the xkbcommon API to interface with the wayland virtual keyboard
 * interface.
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <string.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>
#include <wayland-client.h>

#include "virtual-keyboard-unstable-v1.h"
#include "keyboard.h"
#include "shm.h"

struct table_entry {
	xkb_keysym_t symbol;
	xkb_keycode_t code;
};

static void append_entry(struct keyboard* self, xkb_keysym_t symbol,
                         xkb_keycode_t code)
{
	if (self->lookup_table_size <= self->lookup_table_length) {
		size_t new_size = self->lookup_table_size * 2;
		struct table_entry* table =
			realloc(self->lookup_table, new_size * sizeof(*table));
		if (!table)
			return; // TODO: Report this

		self->lookup_table_size = new_size;
		self->lookup_table = table;
	}

	struct table_entry* entry =
		&self->lookup_table[self->lookup_table_length++];

	entry->symbol = symbol;
	entry->code = code;
}

static void key_iter(struct xkb_keymap* map, xkb_keycode_t code, void* userdata)
{
	struct keyboard* self = userdata;

	size_t n_levels = xkb_keymap_num_levels_for_key(map, code, 0);

	for (size_t level = 0; level < n_levels; level++) {
		const xkb_keysym_t* symbols;
		size_t n_syms = xkb_keymap_key_get_syms_by_level(map, code, 0,
				                                 level,
				                                 &symbols);

		for (size_t sym_idx = 0; sym_idx < n_syms; sym_idx++)
			append_entry(self, symbols[sym_idx], code);
	}
}

static int compare_symbols(const void* a, const void* b)
{
	const struct table_entry* x = a;
	const struct table_entry* y = b;

	return x->symbol < y->symbol ? -1 : x->symbol > y->symbol;
}

static int create_lookup_table(struct keyboard* self)
{
	self->lookup_table_length = 0;
	self->lookup_table_size = 128;

	self->lookup_table =
		malloc(self->lookup_table_size * sizeof(*self->lookup_table));
	if (!self->lookup_table)
		return -1;

	xkb_keymap_key_for_each(self->keymap, key_iter, self);

	qsort(self->lookup_table, self->lookup_table_length,
	      sizeof(*self->lookup_table), compare_symbols);

	return 0;
}

void keyboard_dump_lookup_table(const struct keyboard* self)
{
	for (size_t i = 0; i < self->lookup_table_length; i++) {
		struct table_entry* entry = &self->lookup_table[i];

		const char* code_name =
			xkb_keymap_key_get_name(self->keymap, entry->code);

		printf("%x\t%s\n", entry->symbol, code_name);
	}
}

int keyboard_init(struct keyboard* self)
{
	self->context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!self->context)
		return -1;

	// TODO: Allow the user to specify the layout
	self->keymap = xkb_keymap_new_from_names(self->context, NULL, 0);
	if (!self->keymap)
		goto keymap_failure;

	self->state = xkb_state_new(self->keymap);
	if (!self->state)
		goto state_failure;

	if (create_lookup_table(self) < 0)
		goto table_failure;

	char* keymap_string =
		xkb_keymap_get_as_string(self->keymap,
		                         XKB_KEYMAP_FORMAT_TEXT_V1);
	if (!keymap_string)
		goto keymap_string_failure;

	size_t keymap_len = strlen(keymap_string);

	int keymap_fd = shm_alloc_fd(0);
	if (keymap_fd < 0)
		goto fd_failure;

	// TODO: Check that write finished writing everything
	write(keymap_fd, keymap_string, keymap_len);

	free(keymap_string);

	zwp_virtual_keyboard_v1_keymap(self->virtual_keyboard,
	                               WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1,
	                               keymap_fd, keymap_len);

	close(keymap_fd);

	return 0;

fd_failure:
	free(keymap_string);
keymap_string_failure:
	free(self->lookup_table);
table_failure:
	xkb_state_unref(self->state);
state_failure:
	xkb_keymap_unref(self->keymap);
keymap_failure:
	xkb_context_unref(self->context);
	return -1;
}

void keyboard_destroy(struct keyboard* self)
{
	free(self->lookup_table);
	xkb_state_unref(self->state);
	xkb_keymap_unref(self->keymap);
	xkb_context_unref(self->context);
}

xkb_keycode_t keyboard_find_keycode(const struct keyboard* self,
                                    xkb_keysym_t symbol)
{
	struct table_entry* entry =
		bsearch(&symbol, self->lookup_table, self->lookup_table_length,
                        sizeof(*self->lookup_table), compare_symbols);

	return entry ? entry->code : XKB_KEYCODE_INVALID;
}

static void keyboard_apply_mods(struct keyboard* self, xkb_keycode_t code,
                                bool is_pressed)
{
	enum xkb_state_component comp, compmask;
	xkb_mod_mask_t depressed, latched, locked, group;

	comp = xkb_state_update_key(self->state, code,
	                            is_pressed ? XKB_KEY_DOWN : XKB_KEY_UP);

	compmask = XKB_STATE_MODS_DEPRESSED |
	           XKB_STATE_MODS_LATCHED |
	           XKB_STATE_MODS_LOCKED |
	           XKB_STATE_MODS_EFFECTIVE;

	if (!(comp & compmask))
		return;

	depressed = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_DEPRESSED);
	latched = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_LATCHED);
	locked = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_LOCKED);
	group = xkb_state_serialize_mods(self->state, XKB_STATE_MODS_EFFECTIVE);

	// TODO: Handle errors
	zwp_virtual_keyboard_v1_modifiers(self->virtual_keyboard, depressed,
	                                  latched, locked, group);
}

void keyboard_feed(struct keyboard* self, xkb_keysym_t symbol, bool is_pressed)
{
	xkb_keycode_t code = keyboard_find_keycode(self, symbol);
	if (code == XKB_KEYCODE_INVALID)
		return; // TODO: Notify the user about this

	// TODO: Handle errors
	zwp_virtual_keyboard_v1_key(self->virtual_keyboard, 0, code - 8,
	                            is_pressed ? WL_KEYBOARD_KEY_STATE_PRESSED
	                                       : WL_KEYBOARD_KEY_STATE_RELEASED);

	// TODO: This could cause some synchronisation problems with other
	// keyboards in the seat.
	keyboard_apply_mods(self, code, is_pressed);
}