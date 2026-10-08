/*
 * waynav — Wayland keynav: grid-based keyboard mouse navigation.
 *
 * Reads waynavrc, shows a grid overlay via layer-shell,
 * warps the pointer and clicks via wlr-virtual-pointer.
 */

#include "log.h"
#include "waynav.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/file.h>
#include <unistd.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s [OPTIONS]\n"
            "\n"
            "Options:\n"
            "  -c, --config PATH        config file "
            "(default: ~/.config/waynav/waynavrc)\n"
            "  -l, --log LEVEL          "
            "error, warn, info (default), debug\n"
            "  -i, --idle-timeout SECS  exit after SECS with no keyboard "
            "input, 0 for never;\n"
            "                           overrides the config directive of the "
            "same name\n"
            "  -v, --version            print version and exit\n"
            "  -h, --help               show this help\n",
            prog);
}

static void print_version(void) {
    fprintf(stderr, "waynav %s\n", VERSION);
}

static const char *find_config_path(void) {
    static char buf[512];

    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg) {
        snprintf(buf, sizeof(buf), "%s/waynav/waynavrc", xdg);
        FILE *f = fopen(buf, "r");
        if (f) {
            fclose(f);
            return buf;
        }
    }

    const char *home = getenv("HOME");
    if (home) {
        snprintf(buf, sizeof(buf), "%s/.config/waynav/waynavrc", home);
        FILE *f = fopen(buf, "r");
        if (f) {
            fclose(f);
            return buf;
        }
    }

    return NULL;
}

/* Try to acquire an exclusive lock. Returns fd on success,
 * -1 if another instance is running, or -2 on an I/O error. */
static int acquire_lock(void) {
    static char path[512];
    const char *run = getenv("XDG_RUNTIME_DIR");
    if (!run)
        run = "/tmp";
    snprintf(path, sizeof(path), "%s/waynav.lock", run);

    int fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd < 0) {
        log_err("cannot open lock %s", path);
        return -2;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
        int error = errno;
        close(fd);
        errno = error;
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            return -1;
        log_err("cannot lock %s", path);
        return -2;
    }
    return fd;
}

int main(int argc, char **argv) {
    const char *config_path = NULL;
    const char *log_level_str = NULL;
    /* Negative means the command line did not say; the config decides then,
     * and a config that is also silent leaves the timeout off. */
    int idle_timeout = -1;

    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"log", required_argument, 0, 'l'},
        {"idle-timeout", required_argument, 0, 'i'},
        {"version", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {NULL, 0, 0, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:l:i:vh", long_options, NULL)) !=
           -1) {
        switch (opt) {
        case 'c':
            config_path = optarg;
            break;
        case 'l':
            log_level_str = optarg;
            break;
        case 'i': {
            char *rest = NULL;
            long secs = strtol(optarg, &rest, 10);
            if (rest == optarg || *rest != '\0' || secs < 0 || secs > INT_MAX) {
                log_err("invalid --idle-timeout: %s", optarg);
                return 1;
            }
            idle_timeout = (int)secs;
            break;
        }
        case 'v':
            print_version();
            return 0;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    int lock_fd = acquire_lock();
    if (lock_fd < 0)
        return lock_fd == -1 ? 0 : 1;

    /* Init logging. CLI flag overrides env var. */
    if (log_level_str)
        setenv("WAYNAV_LOG", log_level_str, 1);
    log_init();

    if (!config_path)
        config_path = find_config_path();
    if (!config_path) {
        log_err("no waynavrc found");
        return 1;
    }

    struct config cfg;
    if (config_load(&cfg, config_path) != 0) {
        log_err("failed to load %s", config_path);
        return 1;
    }

    log_info("loaded %d bindings from %s", cfg.num_bindings, config_path);
    log_debug("start commands: %d", cfg.num_start_commands);

    struct overlay *ov = overlay_create();
    if (!ov) {
        log_err("failed to create overlay");
        return 1;
    }

    /* An exclusive keyboard grab is only as dismissable as the input reaching
     * it, and from in here a keyboard that stopped reporting and a user who
     * stopped typing look the same. Ending on the quiet one costs an overlay
     * that can be brought straight back; not ending costs the session its
     * keyboard. Which trade that is worth is the config's call, so waynav
     * makes it only when asked. */
    if (idle_timeout < 0)
        idle_timeout = cfg.idle_timeout;
    overlay_set_idle_timeout(ov, idle_timeout);

    int scr_w = overlay_get_width(ov);
    int scr_h = overlay_get_height(ov);
    log_info("screen: %dx%d", scr_w, scr_h);

    struct region_state rs;
    region_init(&rs, scr_w, scr_h);

    if (cfg.num_start_commands > 0)
        execute_startup_commands(ov, &rs, cfg.start_commands,
                                 cfg.num_start_commands);

    int ret = overlay_run(ov, &cfg, &rs);
    bool idled = overlay_idled(ov);

    overlay_destroy(ov);
    log_info("exiting");
    if (ret < 0)
        return 1;
    return idled ? EXIT_IDLE : 0;
}
