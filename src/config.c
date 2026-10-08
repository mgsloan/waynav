/*
 * Parse waynavrc into bindings.
 *
 * Syntax:  keysequence cmd1,cmd2,cmd3
 * Example: shift+h cut-left,warp
 */

#include "log.h"
#include "memory-util.h"
#include "string-util.h"
#include "waynav.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static xkb_keysym_t parse_keysym(const char *name) {
    xkb_keysym_t sym = xkb_keysym_from_name(name, 0);
    if (sym == XKB_KEY_NoSymbol)
        sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
    return sym;
}

/* Parse "shift+ctrl+h" into keysym + modifier mask.
 * Modifiers: shift, ctrl, alt, super.
 * Last token is the key name. */
static bool parse_modifier(const char *name, uint32_t *mods) {
    if (strcaseeq(name, "shift"))
        *mods |= MOD_SHIFT;
    else if (strcaseeq(name, "ctrl") || strcaseeq(name, "control"))
        *mods |= MOD_CTRL;
    else if (strcaseeq(name, "alt"))
        *mods |= MOD_ALT;
    else if (strcaseeq(name, "super"))
        *mods |= MOD_SUPER;
    else
        return false;
    return true;
}

/* Parse "shift+ctrl+h" into keysym + modifier mask.
 * Modifiers: shift, ctrl, alt, super.
 * Last token is the key name. */
static int parse_keysequence(const char *seq, xkb_keysym_t *sym,
                             uint32_t *mods) {
    *mods = 0;
    *sym = XKB_KEY_NoSymbol;

    const char *part = seq;
    while (true) {
        const char *plus = strchr(part, '+');
        size_t len = plus ? (size_t)(plus - part) : strlen(part);
        char token[64];
        if (len == 0 || len >= sizeof(token))
            return -1;
        memcpy(token, part, len);
        token[len] = '\0';

        if (!plus) {
            *sym = parse_keysym(token);
            return *sym == XKB_KEY_NoSymbol ? -1 : 0;
        }
        if (!parse_modifier(token, mods))
            return -1;
        part = plus + 1;
    }
}

/* Simple commands: keyword maps directly to type, no args. */
static const struct {
    const char *name;
    enum command_type type;
} simple_commands[] = {
    {"start", CMD_START},         {"end", CMD_END},
    {"cut-left", CMD_CUT_LEFT},   {"cut-right", CMD_CUT_RIGHT},
    {"cut-up", CMD_CUT_UP},       {"cut-down", CMD_CUT_DOWN},
    {"move-left", CMD_MOVE_LEFT}, {"move-right", CMD_MOVE_RIGHT},
    {"move-up", CMD_MOVE_UP},     {"move-down", CMD_MOVE_DOWN},
    {"warp", CMD_WARP},           {"history-back", CMD_HISTORY_BACK},
};

/* If str starts with keyword followed by a non-alphabetic boundary,
 * return a pointer just past the keyword; otherwise NULL. The boundary
 * guard stops "click" from matching "clicker".
 */
static const char *match_keyword(const char *str, const char *keyword) {
    size_t len = strlen(keyword);
    if (strncmp(str, keyword, len) == 0 && !isalpha((unsigned char)str[len]))
        return str + len;
    return NULL;
}

static bool try_simple_command(const char *str, struct command *cmd) {
    for (size_t i = 0; i < ARRAY_LEN(simple_commands); i++) {
        const char *name = simple_commands[i].name;
        const char *args = match_keyword(str, name);
        if (!args)
            continue;
        while (isspace((unsigned char)*args))
            args++;
        if (*args != '\0')
            return false;
        cmd->type = simple_commands[i].type;
        return true;
    }
    return false;
}

static void skip_spaces(const char **str) {
    while (isspace((unsigned char)**str))
        (*str)++;
}

static bool parse_int_value(const char **str, int *value) {
    errno = 0;
    char *end = NULL;
    long parsed = strtol(*str, &end, 10);
    if (end == *str || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
        return false;
    *value = (int)parsed;
    *str = end;
    return true;
}

static bool parse_positive_int(const char **str, int *value) {
    return parse_int_value(str, value) && *value > 0;
}

static bool arguments_finished(const char *str) {
    skip_spaces(&str);
    return *str == '\0';
}

static bool has_argument_separator(const char *args) {
    return isspace((unsigned char)*args);
}

static bool parse_grid_args(const char *args, int *cols, int *rows) {
    skip_spaces(&args);
    if (!parse_positive_int(&args, cols))
        return false;
    skip_spaces(&args);

    if (*args == 'x') {
        args++;
        skip_spaces(&args);
        if (!parse_positive_int(&args, rows))
            return false;
    } else {
        *rows = *cols;
    }

    return arguments_finished(args) && *cols <= GRID_DIMENSION_MAX &&
           *rows <= GRID_DIMENSION_MAX && *cols <= INT_MAX / *rows;
}

static bool parse_size_args(const char *args, int *w, int *h) {
    skip_spaces(&args);
    if (!parse_positive_int(&args, w))
        return false;

    if (*args == '\0') {
        *h = *w;
        return true;
    }
    if (!isspace((unsigned char)*args))
        return false;
    skip_spaces(&args);
    if (*args == '\0') {
        *h = *w;
        return true;
    }
    if (!parse_positive_int(&args, h))
        return false;
    return arguments_finished(args);
}

static int parse_grid_command(const char *args, struct command *cmd) {
    cmd->type = CMD_GRID;
    if (!parse_grid_args(args, &cmd->arg.grid.cols, &cmd->arg.grid.rows))
        return -1;
    return 0;
}

static int parse_cell_select_command(const char *args, struct command *cmd) {
    cmd->type = CMD_CELL_SELECT;
    skip_spaces(&args);
    if (!parse_positive_int(&args, &cmd->arg.cell) || !arguments_finished(args))
        return -1;
    return 0;
}

static int parse_button_command(const char *args, struct command *cmd,
                                enum command_type type, int button_max) {
    cmd->type = type;
    skip_spaces(&args);
    if (!parse_positive_int(&args, &cmd->arg.button) ||
        !arguments_finished(args) || cmd->arg.button > button_max)
        return -1;
    return 0;
}

static int parse_click_command(const char *args, struct command *cmd) {
    return parse_button_command(args, cmd, CMD_CLICK, CLICK_BUTTON_MAX);
}

static int parse_drag_command(const char *args, struct command *cmd) {
    return parse_button_command(args, cmd, CMD_DRAG, DRAG_BUTTON_MAX);
}

static int parse_cursorzoom_command(const char *args, struct command *cmd) {
    cmd->type = CMD_CURSORZOOM;
    if (!parse_size_args(args, &cmd->arg.zoom.w, &cmd->arg.zoom.h))
        return -1;
    return 0;
}

/* Parse the argument following a "shell"/"sh" keyword. */
static int parse_shell_command(const char *args, struct command *cmd) {
    cmd->type = CMD_SHELL;
    skip_spaces(&args);
    size_t len = strlen(args);
    if (len == 0)
        return -1;
    if (args[0] == '\'' && args[len - 1] == '\'') {
        if (len <= 2)
            return -1;
        cmd->arg.shell_cmd = strndup(args + 1, len - 2);
    } else {
        cmd->arg.shell_cmd = strdup(args);
    }
    return 0;
}

typedef int (*command_parser)(const char *, struct command *);

static const struct {
    const char *name;
    command_parser parse;
} argument_commands[] = {
    {"grid", parse_grid_command},
    {"cell-select", parse_cell_select_command},
    {"click", parse_click_command},
    {"drag", parse_drag_command},
    {"cursorzoom", parse_cursorzoom_command},
    {"shell", parse_shell_command},
    {"sh", parse_shell_command},
};

/* Parse a single command string like "click 1" or "grid 4x4"
 * into a struct command. Returns 0 on success. */
static int parse_command(const char *str, struct command *cmd) {
    while (isspace((unsigned char)*str))
        str++;

    if (try_simple_command(str, cmd))
        return 0;

    for (size_t i = 0; i < ARRAY_LEN(argument_commands); i++) {
        const char *args = match_keyword(str, argument_commands[i].name);
        if (!args)
            continue;
        if (!has_argument_separator(args))
            return -1;
        return argument_commands[i].parse(args, cmd);
    }
    return -1;
}

/* Parse a comma-separated command chain into a binding's
 * command array. Returns the number of commands parsed. */
static void free_shell_commands(const struct command *cmds, int count) {
    for (int i = 0; i < count; i++) {
        if (cmds[i].type == CMD_SHELL)
            free(cmds[i].arg.shell_cmd);
    }
}

static void free_binding_shell_commands(const struct binding *bindings,
                                        int count) {
    for (int i = 0; i < count; i++)
        free_shell_commands(bindings[i].commands, bindings[i].num_commands);
}

static int parse_command_chain(const char *chain, struct command *cmds,
                               int max) {
    int count = 0;
    const char *part = chain;

    while (true) {
        const char *comma = strchr(part, ',');
        size_t len = comma ? (size_t)(comma - part) : strlen(part);
        char token[1024];
        if (len == 0 || len >= sizeof(token) || count >= max) {
            free_shell_commands(cmds, count);
            return -1;
        }

        memcpy(token, part, len);
        token[len] = '\0';
        if (parse_command(token, &cmds[count]) != 0) {
            free_shell_commands(cmds, count);
            return -1;
        }
        count++;

        if (!comma)
            break;
        part = comma + 1;
    }
    return count;
}

static void store_start_commands(struct config *cfg, const struct command *cmds,
                                 int ncmds) {
    free_shell_commands(cfg->start_commands, cfg->num_start_commands);
    cfg->num_start_commands = 0;
    for (int i = 1; i < ncmds; i++)
        cfg->start_commands[cfg->num_start_commands++] = cmds[i];
    log_debug("start binding: %d chained commands", cfg->num_start_commands);
}

static int store_binding(struct config *cfg, const char *path, int lineno,
                         xkb_keysym_t sym, uint32_t mods,
                         const struct command *cmds, int ncmds) {
    if (cfg->num_bindings >= MAX_BINDINGS) {
        free_shell_commands(cmds, ncmds);
        log_warn("%s:%d: too many bindings (max %d)", path, lineno,
                 MAX_BINDINGS);
        return -1;
    }

    struct binding *b = &cfg->bindings[cfg->num_bindings];
    b->keysym = sym;
    b->keycode = XKB_KEYCODE_INVALID;
    b->mods = mods;
    b->num_commands = ncmds;
    memcpy(b->commands, cmds, ncmds * sizeof(struct command));
    cfg->num_bindings++;

    log_debug("bind: sym=0x%x mods=0x%x cmds=%d", sym, mods, ncmds);
    return 0;
}
static int hex_digit_value(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* Parse "rgb", "rrggbb", or "rrggbbaa" (bare hex, since a '#' would
 * start a comment) into a packed 0xRRGGBBAA color. A missing alpha
 * channel defaults to fully opaque (0xff). Returns 0 on success, -1
 * on a malformed value. */
static int parse_hex_color(const char *str, uint32_t *color) {
    while (isspace((unsigned char)*str))
        str++;

    uint32_t packed = 0;
    int digit_count = 0;
    int digit;
    while ((digit = hex_digit_value(str[digit_count])) >= 0) {
        if (digit_count >= 8)
            return -1;
        packed = (packed << 4) | (uint32_t)digit;
        digit_count++;
    }

    const char *terminator = str + digit_count;
    while (isspace((unsigned char)*terminator))
        terminator++;
    if (digit_count == 0 || *terminator != '\0')
        return -1;
    if (digit_count != 3 && digit_count != 6 && digit_count != 8)
        return -1;

    if (digit_count == 3) {
        /* CSS shorthand: each nibble doubles, so #abc -> #aabbcc. */
        uint32_t r = (packed >> 8) & 0xf;
        uint32_t g = (packed >> 4) & 0xf;
        uint32_t b = packed & 0xf;
        packed = (r << 4 | r) << 16 | (g << 4 | g) << 8 | (b << 4 | b);
    }
    if (digit_count == 3 || digit_count == 6)
        packed = (packed << 8) | 0xffu;

    *color = packed;
    return 0;
}

/* If line is "<keyword> <hex>", parse the color into *out and
 * return true; otherwise return false and leave *out untouched. */
static bool try_color_directive(const char *line, const char *keyword,
                                const char *path, int lineno, uint32_t *out) {
    const char *args = match_keyword(line, keyword);
    if (!args)
        return false;
    if (!has_argument_separator(args))
        return true;
    uint32_t parsed;
    if (parse_hex_color(args, &parsed) == 0)
        *out = parsed;
    else
        log_warn("%s:%d: invalid %s value", path, lineno, keyword);
    return true;
}

static bool try_line_width_directive(const char *line, const char *path,
                                     int lineno, double *out) {
    const char *args = match_keyword(line, "line-width");
    if (!args || !isspace((unsigned char)*args))
        return false;

    double parsed = 0.0;
    char extra = '\0';
    if (sscanf(args, " %lf %c", &parsed, &extra) == 1 && isfinite(parsed) &&
        parsed > 0.0) {
        *out = parsed;
    } else {
        log_warn("%s:%d: invalid line-width", path, lineno);
    }
    return true;
}

/* If line is "idle-timeout <seconds>", parse it into *out. 0 turns the
 * timeout off, which is what a config that never mentions it also does. */
static bool try_idle_timeout_directive(const char *line, const char *path,
                                       int lineno, int *out) {
    const char *args = match_keyword(line, "idle-timeout");
    if (!args || !isspace((unsigned char)*args))
        return false;

    int parsed = 0;
    if (parse_int_value(&args, &parsed) && parsed >= 0 &&
        arguments_finished(args)) {
        *out = parsed;
        log_debug("idle-timeout: %ds", parsed);
    } else {
        log_warn("%s:%d: invalid idle-timeout", path, lineno);
    }
    return true;
}

static int parse_line(struct config *cfg, const char *path, int lineno,
                      char *line) {
    char *comment = strchr(line, '#');
    if (comment)
        *comment = '\0';

    size_t line_len = strlen(line);
    while (line_len > 0 && isspace((unsigned char)line[line_len - 1]))
        line[--line_len] = '\0';
    while (isspace((unsigned char)*line))
        line++;

    if (*line == '\0')
        return 0;

    if (streq(line, "clear")) {
        free_binding_shell_commands(cfg->bindings, cfg->num_bindings);
        cfg->num_bindings = 0;
        log_debug("clear: reset bindings");
        return 0;
    }

    if (try_color_directive(line, "grid-color", path, lineno,
                            &cfg->grid_color) ||
        try_color_directive(line, "region-bg", path, lineno, &cfg->region_bg))
        return 0;

    if (try_line_width_directive(line, path, lineno, &cfg->line_width))
        return 0;

    if (try_idle_timeout_directive(line, path, lineno, &cfg->idle_timeout))
        return 0;

    char *space = line;
    while (*space && !isspace((unsigned char)*space))
        space++;
    if (*space == '\0')
        return 0;

    *space = '\0';
    const char *keyseq = line;
    const char *chain = space + 1;

    xkb_keysym_t sym;
    uint32_t mods;
    if (parse_keysequence(keyseq, &sym, &mods) != 0) {
        log_warn("%s:%d: unknown key '%s'", path, lineno, keyseq);
        return -1;
    }

    struct command cmds[MAX_COMMANDS] = {0};
    int ncmds = parse_command_chain(chain, cmds, MAX_COMMANDS);
    if (ncmds < 0)
        return -1;
    if (ncmds == 0)
        return 0;

    if (cmds[0].type == CMD_START) {
        store_start_commands(cfg, cmds, ncmds);
        return 0;
    }

    return store_binding(cfg, path, lineno, sym, mods, cmds, ncmds);
}

int config_load(struct config *cfg, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        log_err("cannot open %s", path);
        return -1;
    }

    ZERO_OBJECT(*cfg);
    cfg->grid_color = GRID_COLOR_DEFAULT;
    cfg->region_bg = REGION_BG_DEFAULT;
    cfg->line_width = GRID_LINE_WIDTH_DEFAULT;

    enum { CONFIG_LINE_MAX = 1023 };
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length;
    int lineno = 0;
    while ((line_length = getline(&line, &line_capacity, f)) != -1) {
        lineno++;
        while (line_length > 0 &&
               (line[line_length - 1] == '\n' || line[line_length - 1] == '\r'))
            line[--line_length] = '\0';
        if (line_length > CONFIG_LINE_MAX) {
            log_warn("%s:%d: line too long", path, lineno);
            continue;
        }

        if (parse_line(cfg, path, lineno, line) != 0)
            log_warn("parse error at %s:%d", path, lineno);
    }

    free(line);
    if (ferror(f)) {
        log_err("failed to read %s", path);
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

xkb_keycode_t config_keycode_for_keysym(struct xkb_keymap *keymap,
                                        xkb_keysym_t sym) {
    if (!keymap || sym == XKB_KEY_NoSymbol)
        return XKB_KEYCODE_INVALID;

    xkb_keycode_t min = xkb_keymap_min_keycode(keymap);
    xkb_keycode_t max = xkb_keymap_max_keycode(keymap);
    xkb_layout_index_t layouts = xkb_keymap_num_layouts(keymap);
    for (xkb_layout_index_t layout = 0; layout < layouts; layout++) {
        for (xkb_keycode_t keycode = min; keycode <= max; keycode++) {
            if (layout >= xkb_keymap_num_layouts_for_key(keymap, keycode))
                continue;
            xkb_level_index_t levels =
                xkb_keymap_num_levels_for_key(keymap, keycode, layout);
            for (xkb_level_index_t level = 0; level < levels; level++) {
                const xkb_keysym_t *syms = NULL;
                int count = xkb_keymap_key_get_syms_by_level(
                    keymap, keycode, layout, level, &syms);
                for (int i = 0; i < count; i++) {
                    if (syms[i] == sym)
                        return keycode;
                }
            }
        }
    }

    return XKB_KEYCODE_INVALID;
}

void config_resolve_keycodes(struct config *cfg, struct xkb_keymap *keymap) {
    for (int i = 0; i < cfg->num_bindings; i++) {
        struct binding *binding = &cfg->bindings[i];
        char name[64] = {0};
        xkb_keysym_get_name(binding->keysym, name, sizeof(name));

        binding->keycode = config_keycode_for_keysym(keymap, binding->keysym);
        if (binding->keycode == XKB_KEYCODE_INVALID) {
            log_warn("no key on this keymap produces '%s': binding ignored",
                     name);
            continue;
        }

        for (int j = 0; j < i; j++) {
            if (cfg->bindings[j].keycode == binding->keycode &&
                cfg->bindings[j].mods == binding->mods) {
                log_warn("'%s' overrides an earlier binding on the same key",
                         name);
                break;
            }
        }

        log_debug("resolve: sym=0x%x -> keycode=%u", binding->keysym,
                  binding->keycode);
    }
}

const struct binding *config_find_binding(const struct config *cfg,
                                          xkb_keycode_t keycode,
                                          uint32_t mods) {
    if (keycode == XKB_KEYCODE_INVALID)
        return NULL;

    const struct binding *found = NULL;
    for (int i = 0; i < cfg->num_bindings; i++) {
        if (cfg->bindings[i].keycode == keycode &&
            cfg->bindings[i].mods == mods)
            found = &cfg->bindings[i];
    }
    return found;
}
