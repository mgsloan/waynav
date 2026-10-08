// Prefix comparisons in the config parser need boundary-aware matching.
// Report only: changing the comparison also requires using the returned args.
@prefix@
identifier fn;
identifier compare =~ "^(strncmp|strncasecmp)$";
position p;
@@
fn(...) {
<...
compare@p(...)
...>
}

@script:python@
p << prefix.p;
fn << prefix.fn;
@@
import os
if os.path.basename(p[0].file) == "config.c" and fn != "match_keyword":
    coccilib.report.print_report(p[0], "keyword-prefix: use match_keyword() and its returned argument pointer")
