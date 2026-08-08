/*
 * Tests for config parser.
 */

#include "../src/waynav.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Bindings match by keycode, so the lookups below need a real keymap.
 * Built once in main from the default rules and a "us" layout. */
static struct xkb_keymap *keymap;

static struct xkb_keymap *build_keymap(void) {
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(ctx);

    struct xkb_rule_names names = {
        .rules = NULL,
        .model = NULL,
        .layout = "us",
        .variant = NULL,
        .options = NULL,
    };
    struct xkb_keymap *km =
        xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(km);

    /* The keymap holds its own reference to the context. */
    xkb_context_unref(ctx);
    return km;
}

static void write_tmp_config(const char *content, const char *path) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void test_basic_parse(void) {
    const char *path = "/tmp/waynav_test_config";
    write_tmp_config("clear\n"
                     "super+semicolon start,grid 4x4\n"
                     "h move-left,warp\n"
                     "shift+h cut-left,warp\n"
                     "space warp,click 1\n"
                     "semicolon end\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    xkb_keycode_t kc_h = config_keycode_for_keysym(keymap, XKB_KEY_h);
    xkb_keycode_t kc_space = config_keycode_for_keysym(keymap, XKB_KEY_space);
    xkb_keycode_t kc_semicolon =
        config_keycode_for_keysym(keymap, XKB_KEY_semicolon);
    assert(cfg.line_width == GRID_LINE_WIDTH_DEFAULT);

    /* start binding is stored separately. */
    assert(cfg.num_start_commands == 1);
    assert(cfg.start_commands[0].type == CMD_GRID);
    assert(cfg.start_commands[0].arg.grid.cols == 4);
    assert(cfg.start_commands[0].arg.grid.rows == 4);

    /* 4 normal bindings: h, shift+h, space, semicolon */
    assert(cfg.num_bindings == 4);

    /* h -> move-left, warp */
    const struct binding *b = config_find_binding(&cfg, kc_h, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_MOVE_LEFT);
    assert(b->commands[1].type == CMD_WARP);

    /* shift+h -> cut-left, warp */
    b = config_find_binding(&cfg, kc_h, MOD_SHIFT);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_CUT_LEFT);
    assert(b->commands[1].type == CMD_WARP);

    /* space -> warp, click 1 */
    b = config_find_binding(&cfg, kc_space, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_WARP);
    assert(b->commands[1].type == CMD_CLICK);
    assert(b->commands[1].arg.button == 1);

    /* semicolon -> end */
    b = config_find_binding(&cfg, kc_semicolon, 0);
    assert(b);
    assert(b->num_commands == 1);
    assert(b->commands[0].type == CMD_END);

    unlink(path);
}

static void test_line_width(void) {
    const char *path = "/tmp/waynav_test_config_line_width";
    write_tmp_config("clear\n"
                     "line-width 3.5\n"
                     "semicolon end\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.line_width == 3.5);
    assert(cfg.num_bindings == 1);

    unlink(path);
}

static void test_invalid_line_width_keeps_previous_value(void) {
    const char *path = "/tmp/waynav_test_config_invalid_line_width";
    write_tmp_config("line-width 2.5\n"
                     "line-width 0\n"
                     "line-width -1\n"
                     "line-width nan\n"
                     "line-width inf\n"
                     "line-width 1e309\n"
                     "line-width 3px\n"
                     "line-width2\n"
                     "semicolon end\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.line_width == 2.5);
    assert(cfg.num_bindings == 1);

    unlink(path);
}

static void test_cell_select(void) {
    const char *path = "/tmp/waynav_test_config2";
    write_tmp_config("clear\n"
                     "1 cell-select 1,warp\n"
                     "v cell-select 16,warp\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    xkb_keycode_t kc_1 = config_keycode_for_keysym(keymap, XKB_KEY_1);
    xkb_keycode_t kc_v = config_keycode_for_keysym(keymap, XKB_KEY_v);
    assert(cfg.num_bindings == 2);

    const struct binding *b = config_find_binding(&cfg, kc_1, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CELL_SELECT);
    assert(b->commands[0].arg.cell == 1);

    b = config_find_binding(&cfg, kc_v, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CELL_SELECT);
    assert(b->commands[0].arg.cell == 16);

    unlink(path);
}

static void test_shell_command(void) {
    const char *path = "/tmp/waynav_test_config3";
    write_tmp_config("clear\n"
                     "grave shell 'notify deprecated'\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    xkb_keycode_t kc_grave = config_keycode_for_keysym(keymap, XKB_KEY_grave);
    assert(cfg.num_bindings == 1);

    const struct binding *b = config_find_binding(&cfg, kc_grave, 0);
    assert(b);
    assert(b->commands[0].type == CMD_SHELL);
    assert(strcmp(b->commands[0].arg.shell_cmd, "notify deprecated") == 0);

    free(b->commands[0].arg.shell_cmd);
    unlink(path);
}

static void test_cursorzoom(void) {
    const char *path = "/tmp/waynav_test_config4";
    write_tmp_config("clear\n"
                     "i grid 1x1,cursorzoom 10 10\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    xkb_keycode_t kc_i = config_keycode_for_keysym(keymap, XKB_KEY_i);
    assert(cfg.num_bindings == 1);

    const struct binding *b = config_find_binding(&cfg, kc_i, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_GRID);
    assert(b->commands[0].arg.grid.cols == 1);
    assert(b->commands[0].arg.grid.rows == 1);
    assert(b->commands[1].type == CMD_CURSORZOOM);
    assert(b->commands[1].arg.zoom.w == 10);
    assert(b->commands[1].arg.zoom.h == 10);

    unlink(path);
}

static void test_drag(void) {
    const char *path = "/tmp/waynav_test_config5";
    write_tmp_config("clear\n"
                     "shift+space warp,drag 1\n"
                     "shift+minus warp,drag 3\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    xkb_keycode_t kc_space = config_keycode_for_keysym(keymap, XKB_KEY_space);
    xkb_keycode_t kc_minus = config_keycode_for_keysym(keymap, XKB_KEY_minus);
    assert(cfg.num_bindings == 2);

    const struct binding *b = config_find_binding(&cfg, kc_space, MOD_SHIFT);
    assert(b);
    assert(b->commands[1].type == CMD_DRAG);
    assert(b->commands[1].arg.button == 1);

    b = config_find_binding(&cfg, kc_minus, MOD_SHIFT);
    assert(b);
    assert(b->commands[1].type == CMD_DRAG);
    assert(b->commands[1].arg.button == 3);

    unlink(path);
}

static void test_rejects_command_prefix_extension(void) {
    /* A command whose name merely extends a real keyword (clicker,
     * dragon) must not be mis-parsed as that keyword. The chain then
     * yields no commands, so the binding is dropped. */
    const char *path = "/tmp/waynav_test_config_prefix";
    write_tmp_config("clear\n"
                     "x clicker 1\n"
                     "y dragon 2\n"
                     "z click 1\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    xkb_keycode_t kc_z = config_keycode_for_keysym(keymap, XKB_KEY_z);

    /* Only the well-formed "click" binding survives. */
    assert(cfg.num_bindings == 1);
    const struct binding *b = config_find_binding(&cfg, kc_z, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CLICK);
    assert(b->commands[0].arg.button == 1);

    unlink(path);
}

static void test_colors(void) {
    const char *path = "/tmp/waynav_test_config_colors";
    write_tmp_config("grid-color ff0000\n"
                     "region-bg 11223344\n"
                     "grid-color abc        # shorthand re-sets grid_color\n"
                     "line-width 2.5\n"
                     "grid-color nope       # malformed: leaves prior value\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);

    /* ff0000 is overwritten by valid shorthand abc -> aabbccff;
     * the malformed value after it is rejected and changes nothing. */
    assert(cfg.grid_color == 0xaabbccff);
    assert(cfg.region_bg == 0x11223344);
    assert(cfg.line_width == 2.5);

    unlink(path);
}

static void test_color_defaults(void) {
    const char *path = "/tmp/waynav_test_config_defaults";
    write_tmp_config("clear\n", path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.grid_color == GRID_COLOR_DEFAULT);
    assert(cfg.region_bg == REGION_BG_DEFAULT);
    assert(cfg.line_width == GRID_LINE_WIDTH_DEFAULT);

    unlink(path);
}

/* The bug this guards against: xkb resolves shift+h to XKB_KEY_H and
 * shift+1 to XKB_KEY_exclam, so matching a binding by the keysym the
 * modifiers produce could never fire a binding written the keynav way.
 * Both spellings have to reach the same binding, and the unmodified key
 * has to stay distinct from the shifted one. */
static void test_shifted_bindings_match_the_physical_key(void) {
    const char *path = "/tmp/waynav_test_config_shift";
    write_tmp_config("clear\n"
                     "h cut-left,warp\n"
                     "shift+h move-left,warp\n"
                     "shift+1 click 1\n"
                     "shift+asterisk drag 2\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    assert(cfg.num_bindings == 4);

    /* What the compositor would report for each of those keys. */
    struct xkb_state *state = xkb_state_new(keymap);
    assert(state);
    xkb_mod_index_t shift =
        xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
    assert(shift != XKB_MOD_INVALID);
    xkb_state_update_mask(state, 1u << shift, 0, 0, 0, 0, 0);

    xkb_keycode_t kc_h = config_keycode_for_keysym(keymap, XKB_KEY_h);
    xkb_keycode_t kc_1 = config_keycode_for_keysym(keymap, XKB_KEY_1);
    xkb_keycode_t kc_8 = config_keycode_for_keysym(keymap, XKB_KEY_8);
    assert(kc_h != XKB_KEYCODE_INVALID);

    /* Holding shift changes the symbol but not the key. */
    assert(xkb_state_key_get_one_sym(state, kc_h) == XKB_KEY_H);
    assert(xkb_state_key_get_one_sym(state, kc_1) == XKB_KEY_exclam);

    const struct binding *b = config_find_binding(&cfg, kc_h, MOD_SHIFT);
    assert(b);
    assert(b->commands[0].type == CMD_MOVE_LEFT);

    /* Same key without shift is still its own binding. */
    b = config_find_binding(&cfg, kc_h, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CUT_LEFT);

    b = config_find_binding(&cfg, kc_1, MOD_SHIFT);
    assert(b);
    assert(b->commands[0].type == CMD_CLICK);

    /* Naming the shifted symbol instead resolves to the same key, so
     * "shift+asterisk" and "shift+8" are one binding. */
    b = config_find_binding(&cfg, kc_8, MOD_SHIFT);
    assert(b);
    assert(b->commands[0].type == CMD_DRAG);
    assert(b->commands[0].arg.button == 2);

    xkb_state_unref(state);
    unlink(path);
}

/* A later line beats an earlier one on the same key, which only becomes
 * observable once two names collapse onto one keycode. */
static void test_later_binding_overrides_earlier(void) {
    const char *path = "/tmp/waynav_test_config_override";
    write_tmp_config("clear\n"
                     "shift+period click 1\n"
                     "shift+greater click 3\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    xkb_keycode_t kc_period = config_keycode_for_keysym(keymap, XKB_KEY_period);
    assert(cfg.num_bindings == 2);

    const struct binding *b = config_find_binding(&cfg, kc_period, MOD_SHIFT);
    assert(b);
    assert(b->commands[0].type == CMD_CLICK);
    assert(b->commands[0].arg.button == 3);

    unlink(path);
}

/* A name no key on the keymap produces resolves to nothing and matches
 * nothing, rather than colliding with other unresolved bindings. */
static void test_unresolvable_keysym_matches_nothing(void) {
    const char *path = "/tmp/waynav_test_config_unresolvable";
    write_tmp_config("clear\n"
                     "Hangul_J_YeorinHieuh click 1\n"
                     "Hangul_J_KiyeogSios click 2\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    assert(cfg.num_bindings == 2);

    assert(cfg.bindings[0].keycode == XKB_KEYCODE_INVALID);
    assert(config_find_binding(&cfg, XKB_KEYCODE_INVALID, 0) == NULL);

    unlink(path);
}

int main(void) {
    keymap = build_keymap();

    test_basic_parse();
    test_line_width();
    test_invalid_line_width_keeps_previous_value();
    test_cell_select();
    test_shell_command();
    test_cursorzoom();
    test_drag();
    test_rejects_command_prefix_extension();
    test_colors();
    test_color_defaults();
    test_shifted_bindings_match_the_physical_key();
    test_later_binding_overrides_earlier();
    test_unresolvable_keysym_matches_nothing();

    xkb_keymap_unref(keymap);

    printf("All config tests passed.\n");
    return 0;
}
