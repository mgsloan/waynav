/*
 * Tests for config parser.
 */

#include "../src/waynav.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct xkb_keymap *keymap = NULL;

static void write_tmp_config(const char *content, const char *path) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void build_keymap(void) {
    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(context);
    struct xkb_rule_names names = {
        .layout = "us",
    };
    keymap =
        xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(keymap);
    xkb_context_unref(context);
}

static const struct binding *find_binding_by_keysym(const struct config *cfg,
                                                    xkb_keysym_t sym,
                                                    uint32_t mods) {
    xkb_keycode_t keycode = config_keycode_for_keysym(keymap, sym);
    return config_find_binding(cfg, keycode, mods);
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
    assert(cfg.line_width == GRID_LINE_WIDTH_DEFAULT);
    assert(cfg.idle_timeout == 0);

    /* start binding is stored separately. */
    assert(cfg.num_start_commands == 1);
    assert(cfg.start_commands[0].type == CMD_GRID);
    assert(cfg.start_commands[0].arg.grid.cols == 4);
    assert(cfg.start_commands[0].arg.grid.rows == 4);

    /* 4 normal bindings: h, shift+h, space, semicolon */
    assert(cfg.num_bindings == 4);

    /* h -> move-left, warp */
    const struct binding *b = find_binding_by_keysym(&cfg, XKB_KEY_h, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_MOVE_LEFT);
    assert(b->commands[1].type == CMD_WARP);

    /* shift+h -> cut-left, warp */
    b = find_binding_by_keysym(&cfg, XKB_KEY_h, MOD_SHIFT);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_CUT_LEFT);
    assert(b->commands[1].type == CMD_WARP);

    /* space -> warp, click 1 */
    b = find_binding_by_keysym(&cfg, XKB_KEY_space, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_WARP);
    assert(b->commands[1].type == CMD_CLICK);
    assert(b->commands[1].arg.button == 1);

    /* semicolon -> end */
    b = find_binding_by_keysym(&cfg, XKB_KEY_semicolon, 0);
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

static void test_idle_timeout(void) {
    const char *path = "/tmp/waynav_test_config_idle_timeout";
    write_tmp_config("clear\n"
                     "idle-timeout 45\n"
                     "semicolon end\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.idle_timeout == 45);
    assert(cfg.num_bindings == 1);

    /* Explicitly off, which is also what a config that never mentions it
     * gets: nothing bounds the grab unless something asks. */
    write_tmp_config("idle-timeout 0\n", path);
    assert(config_load(&cfg, path) == 0);
    assert(cfg.idle_timeout == 0);

    write_tmp_config("semicolon end\n", path);
    assert(config_load(&cfg, path) == 0);
    assert(cfg.idle_timeout == 0);

    unlink(path);
}

static void test_invalid_idle_timeout_keeps_previous_value(void) {
    const char *path = "/tmp/waynav_test_config_invalid_idle_timeout";
    write_tmp_config("idle-timeout 30\n"
                     "idle-timeout -1\n"
                     "idle-timeout 1.5\n"
                     "idle-timeout 30s\n"
                     "idle-timeout forever\n"
                     "idle-timeout 99999999999999999999\n"
                     "idle-timeout2\n"
                     "semicolon end\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    assert(cfg.idle_timeout == 30);
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
    assert(cfg.num_bindings == 2);

    const struct binding *b = find_binding_by_keysym(&cfg, XKB_KEY_1, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CELL_SELECT);
    assert(b->commands[0].arg.cell == 1);

    b = find_binding_by_keysym(&cfg, XKB_KEY_v, 0);
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
    assert(cfg.num_bindings == 1);

    const struct binding *b = find_binding_by_keysym(&cfg, XKB_KEY_grave, 0);
    assert(b);
    assert(b->commands[0].type == CMD_SHELL);
    assert(strcmp(b->commands[0].arg.shell_cmd, "notify deprecated") == 0);

    free(b->commands[0].arg.shell_cmd);
    unlink(path);
}

static void test_cursorzoom(void) {
    const char *path = "/tmp/waynav_test_config4";
    write_tmp_config("clear\n"
                     "i grid 1x1,cursorzoom 10 10\n"
                     "v cursorzoom 10 ,warp\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    assert(cfg.num_bindings == 2);

    const struct binding *b = find_binding_by_keysym(&cfg, XKB_KEY_i, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_GRID);
    assert(b->commands[0].arg.grid.cols == 1);
    assert(b->commands[0].arg.grid.rows == 1);
    assert(b->commands[1].type == CMD_CURSORZOOM);
    assert(b->commands[1].arg.zoom.w == 10);
    assert(b->commands[1].arg.zoom.h == 10);

    b = find_binding_by_keysym(&cfg, XKB_KEY_v, 0);
    assert(b);
    assert(b->num_commands == 2);
    assert(b->commands[0].type == CMD_CURSORZOOM);
    assert(b->commands[0].arg.zoom.w == 10);
    assert(b->commands[0].arg.zoom.h == 10);
    assert(b->commands[1].type == CMD_WARP);

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
    assert(cfg.num_bindings == 2);

    const struct binding *b =
        find_binding_by_keysym(&cfg, XKB_KEY_space, MOD_SHIFT);
    assert(b);
    assert(b->commands[1].type == CMD_DRAG);
    assert(b->commands[1].arg.button == 1);

    b = find_binding_by_keysym(&cfg, XKB_KEY_minus, MOD_SHIFT);
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

    /* Only the well-formed "click" binding survives. */
    assert(cfg.num_bindings == 1);
    const struct binding *b = find_binding_by_keysym(&cfg, XKB_KEY_z, 0);
    assert(b);
    assert(b->commands[0].type == CMD_CLICK);
    assert(b->commands[0].arg.button == 1);

    unlink(path);
}

static void test_rejects_malformed_command_chains(void) {
    const char *path = "/tmp/waynav_test_config_malformed_chain";
    write_tmp_config("clear\n"
                     "a cut-left,edn,click 1\n"
                     "b grid 0x3\n"
                     "c grid 2x\n"
                     "d drag 4\n"
                     "e click 6\n"
                     "f cursorzoom 0 10\n"
                     "g cell-select 0\n"
                     "h end now\n"
                     "i shell true,edn\n"
                     "j ,click 1\n"
                     "k click 1,,warp\n"
                     "l click 1,\n"
                     "m click1\n"
                     "n drag2\n"
                     "o grid2x3\n"
                     "p cursorzoom10 20\n"
                     "q cell-select4\n"
                     "r shell:true\n"
                     "s shell    \n"
                     "t shell ''\n"
                     "u cursorzoom 10+20\n"
                     "z click 1\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);

    assert(cfg.num_bindings == 1);
    const struct binding *b = find_binding_by_keysym(&cfg, XKB_KEY_z, 0);
    assert(b);
    assert(b->num_commands == 1);
    assert(b->commands[0].type == CMD_CLICK);

    unlink(path);
}

static void test_rejects_overlong_lines(void) {
    const char *path = "/tmp/waynav_test_config_overlong";
    char content[3000];
    int prefix = snprintf(content, sizeof(content), "a shell ");
    assert(prefix > 0);
    memset(content + prefix, 'x', 2000);
    content[prefix + 2000] = '\0';
    strcat(content, "\nz click 1\n");
    write_tmp_config(content, path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);

    assert(cfg.num_bindings == 1);
    assert(find_binding_by_keysym(&cfg, XKB_KEY_z, 0));
    unlink(path);
}

static void write_line_boundary_config(const char *path, int content_length) {
    char content[1200];
    const char *prefix = "a shell ";
    int prefix_length = snprintf(content, sizeof(content), "%s", prefix);
    memset(content + prefix_length, 'x',
           (size_t)(content_length - prefix_length));
    content[content_length] = '\0';
    strcat(content, "\nz click 1\n");
    write_tmp_config(content, path);
}

static void test_line_length_boundaries(void) {
    const char *accepted_path = "/tmp/waynav_test_config_line_max";
    write_line_boundary_config(accepted_path, 1023);
    struct config cfg;
    assert(config_load(&cfg, accepted_path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    assert(cfg.num_bindings == 2);
    unlink(accepted_path);

    const char *rejected_path = "/tmp/waynav_test_config_line_over_max";
    write_line_boundary_config(rejected_path, 1024);
    struct config rejected_cfg;
    assert(config_load(&rejected_cfg, rejected_path) == 0);
    config_resolve_keycodes(&rejected_cfg, keymap);
    assert(rejected_cfg.num_bindings == 1);
    assert(find_binding_by_keysym(&rejected_cfg, XKB_KEY_z, 0));
    unlink(rejected_path);
}

static void test_command_count_boundaries(void) {
    const char *path = "/tmp/waynav_test_config_command_count";
    write_tmp_config(
        "clear\n"
        "a click 1,click 1,click 1,click 1,click 1,click 1,click 1,click 1\n"
        "b click 1,click 1,click 1,click 1,click 1,click 1,click 1,click "
        "1,click 1\n"
        "z end\n",
        path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);

    assert(cfg.num_bindings == 2);
    assert(find_binding_by_keysym(&cfg, XKB_KEY_a, 0));
    assert(find_binding_by_keysym(&cfg, XKB_KEY_z, 0));
    unlink(path);
}

static void test_grid_dimension_boundaries(void) {
    const char *path = "/tmp/waynav_test_config_grid_boundaries";
    write_tmp_config("clear\n"
                     "a grid 16384x1\n"
                     "b grid 16385x1\n"
                     "c grid 65536x65536\n"
                     "d grid 2147483648x1\n"
                     "e grid 999999999999999999999x1\n"
                     "z end\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);

    assert(cfg.num_bindings == 2);
    assert(find_binding_by_keysym(&cfg, XKB_KEY_a, 0));
    assert(find_binding_by_keysym(&cfg, XKB_KEY_z, 0));
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

static void test_rejects_malformed_keysequences(void) {
    const char *path = "/tmp/waynav_test_config_bad_keysequences";
    write_tmp_config("clear\n"
                     "a+b end\n"
                     "shift++h end\n"
                     "+h end\n"
                     "shift+ end\n"
                     "z click 1\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);

    assert(cfg.num_bindings == 1);
    assert(find_binding_by_keysym(&cfg, XKB_KEY_z, 0));
    unlink(path);
}

static void test_directives_use_complete_lines(void) {
    const char *path = "/tmp/waynav_test_config_strict_directives";
    write_tmp_config("a end\n"
                     "clear # reset bindings\n"
                     "grid-color ff0000 garbage\n"
                     "grid-color123\n"
                     "z click 1\n",
                     path);

    struct config cfg;
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);

    assert(cfg.num_bindings == 1);
    assert(find_binding_by_keysym(&cfg, XKB_KEY_z, 0));
    assert(cfg.grid_color == GRID_COLOR_DEFAULT);
    unlink(path);
}

static void test_shifted_bindings_match_the_physical_key(void) {
    const char *path = "/tmp/waynav_test_config_shifted_bindings";
    write_tmp_config("h cut-left,warp\n"
                     "j cut-right,warp\n"
                     "shift+h move-left,warp\n"
                     "shift+1 click 1\n"
                     "shift+asterisk drag 2\n",
                     path);

    struct config cfg = {0};
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);

    xkb_keycode_t keycode_h = config_keycode_for_keysym(keymap, XKB_KEY_h);
    xkb_keycode_t keycode_1 = config_keycode_for_keysym(keymap, XKB_KEY_1);
    xkb_keycode_t keycode_shift =
        config_keycode_for_keysym(keymap, XKB_KEY_Shift_L);
    struct xkb_state *state = xkb_state_new(keymap);
    assert(keycode_h != XKB_KEYCODE_INVALID);
    assert(keycode_1 != XKB_KEYCODE_INVALID);
    assert(keycode_shift != XKB_KEYCODE_INVALID);
    assert(state);
    xkb_state_update_key(state, keycode_shift, XKB_KEY_DOWN);
    assert(xkb_state_key_get_one_sym(state, keycode_h) == XKB_KEY_H);
    assert(xkb_state_key_get_one_sym(state, keycode_1) == XKB_KEY_exclam);

    const struct binding *binding =
        find_binding_by_keysym(&cfg, XKB_KEY_h, MOD_SHIFT);
    assert(binding);
    assert(binding->commands[0].type == CMD_MOVE_LEFT);

    /* Shifted aliases resolve to the same physical key and modifiers. */
    binding = find_binding_by_keysym(&cfg, XKB_KEY_H, MOD_SHIFT);
    assert(binding);
    assert(binding->commands[0].type == CMD_MOVE_LEFT);

    binding = find_binding_by_keysym(&cfg, XKB_KEY_h, 0);
    assert(binding);
    assert(binding->commands[0].type == CMD_CUT_LEFT);
    assert(!find_binding_by_keysym(&cfg, XKB_KEY_j, MOD_SHIFT));

    binding = find_binding_by_keysym(&cfg, XKB_KEY_1, MOD_SHIFT);
    assert(binding);
    assert(binding->commands[0].type == CMD_CLICK);
    assert(binding->commands[0].arg.button == 1);

    binding = find_binding_by_keysym(&cfg, XKB_KEY_exclam, MOD_SHIFT);
    assert(binding);
    assert(binding->commands[0].type == CMD_CLICK);
    assert(binding->commands[0].arg.button == 1);

    binding = find_binding_by_keysym(&cfg, XKB_KEY_asterisk, MOD_SHIFT);
    assert(binding);
    assert(binding->commands[0].type == CMD_DRAG);
    assert(binding->commands[0].arg.button == 2);
    binding = find_binding_by_keysym(&cfg, XKB_KEY_8, MOD_SHIFT);
    assert(binding);
    assert(binding->commands[0].type == CMD_DRAG);
    assert(binding->commands[0].arg.button == 2);

    xkb_state_unref(state);
    unlink(path);
}

static xkb_keycode_t find_keysym_in_layout(struct xkb_keymap *map,
                                           xkb_keysym_t sym,
                                           xkb_layout_index_t wanted_layout) {
    xkb_keycode_t min = xkb_keymap_min_keycode(map);
    xkb_keycode_t max = xkb_keymap_max_keycode(map);
    for (xkb_keycode_t keycode = min; keycode <= max; keycode++) {
        if (wanted_layout >= xkb_keymap_num_layouts_for_key(map, keycode))
            continue;
        xkb_level_index_t levels =
            xkb_keymap_num_levels_for_key(map, keycode, wanted_layout);
        for (xkb_level_index_t level = 0; level < levels; level++) {
            const xkb_keysym_t *syms = NULL;
            int count = xkb_keymap_key_get_syms_by_level(
                map, keycode, wanted_layout, level, &syms);
            for (int i = 0; i < count; i++) {
                if (syms[i] == sym)
                    return keycode;
            }
        }
    }
    return XKB_KEYCODE_INVALID;
}

static void test_binding_matches_keycode_across_layouts(void) {
    const char *path = "/tmp/waynav_test_config_multi_layout";
    write_tmp_config("h click 1\nc click 3\n", path);

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    assert(context);
    struct xkb_rule_names names = {
        .layout = "us,us",
        .variant = ",dvorak",
    };
    struct xkb_keymap *multi_keymap =
        xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    assert(multi_keymap);

    struct config cfg = {0};
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, multi_keymap);

    xkb_keycode_t resolved = config_keycode_for_keysym(multi_keymap, XKB_KEY_h);
    xkb_keycode_t dvorak_keycode =
        find_keysym_in_layout(multi_keymap, XKB_KEY_h, 1);
    xkb_keycode_t resolved_c =
        config_keycode_for_keysym(multi_keymap, XKB_KEY_c);
    xkb_keycode_t primary_c = find_keysym_in_layout(multi_keymap, XKB_KEY_c, 0);
    assert(resolved != XKB_KEYCODE_INVALID);
    assert(dvorak_keycode != XKB_KEYCODE_INVALID);
    assert(resolved != dvorak_keycode);
    assert(resolved_c == primary_c);
    assert(resolved_c != find_keysym_in_layout(multi_keymap, XKB_KEY_c, 1));

    struct xkb_state *state = xkb_state_new(multi_keymap);
    assert(state);
    xkb_state_update_mask(state, 0, 0, 0, 0, 0, 1);
    assert(xkb_state_key_get_one_sym(state, dvorak_keycode) == XKB_KEY_h);
    assert(xkb_state_key_get_one_sym(state, resolved) != XKB_KEY_h);
    assert(config_find_binding(&cfg, resolved, 0));
    assert(!config_find_binding(&cfg, dvorak_keycode, 0));

    xkb_state_unref(state);
    xkb_keymap_unref(multi_keymap);
    xkb_context_unref(context);
    unlink(path);
}

static void test_later_binding_overrides_earlier(void) {
    const char *path = "/tmp/waynav_test_config_binding_override";
    write_tmp_config("shift+period click 1\n"
                     "shift+greater click 3\n",
                     path);

    struct config cfg = {0};
    assert(config_load(&cfg, path) == 0);
    config_resolve_keycodes(&cfg, keymap);
    assert(cfg.num_bindings == 2);

    const struct binding *binding =
        find_binding_by_keysym(&cfg, XKB_KEY_period, MOD_SHIFT);
    assert(binding);
    assert(binding->commands[0].type == CMD_CLICK);
    assert(binding->commands[0].arg.button == 3);

    unlink(path);
}

int main(void) {
    build_keymap();

    test_basic_parse();
    test_line_width();
    test_invalid_line_width_keeps_previous_value();
    test_idle_timeout();
    test_invalid_idle_timeout_keeps_previous_value();
    test_cell_select();
    test_shell_command();
    test_cursorzoom();
    test_drag();
    test_rejects_command_prefix_extension();
    test_rejects_malformed_command_chains();
    test_rejects_overlong_lines();
    test_line_length_boundaries();
    test_command_count_boundaries();
    test_grid_dimension_boundaries();
    test_rejects_malformed_keysequences();
    test_directives_use_complete_lines();
    test_colors();
    test_color_defaults();
    test_shifted_bindings_match_the_physical_key();
    test_binding_matches_keycode_across_layouts();
    test_later_binding_overrides_earlier();

    xkb_keymap_unref(keymap);
    printf("All config tests passed.\n");
    return 0;
}
