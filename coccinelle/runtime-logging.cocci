// Do not guess a log level or rewrite CLI output automatically.
@stdio@
identifier fn;
identifier output =~ "^(printf|fprintf|vprintf|vfprintf|puts|fputs|putchar|fputc|perror)$";
position p;
@@
fn(...) {
<...
output@p(...)
...>
}

@script:python@
p << stdio.p;
fn << stdio.fn;
@@
import os
name = os.path.basename(p[0].file)
if name != "log.c" and not (name == "main.c" and fn in ("print_usage", "print_version")):
    coccilib.report.print_report(p[0], "runtime-logging: use log_* for diagnostics; reserve stdio for CLI help/version")
