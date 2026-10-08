@has_header@
@@
#include "string-util.h"

@has_string@
@@
#include <string.h>

@has_strings@
@@
#include <strings.h>

@has_local@
@@
#include "..."

@equality depends on has_header || has_string || has_strings@
identifier fn !~ "^(streq|strcaseeq)$";
expression a, b;
@@
fn(...) {
<+...
(
- strcmp(a, b) == 0
+ streq(a, b)
|
- strcmp(a, b) != 0
+ !streq(a, b)
|
- strcasecmp(a, b) == 0
+ strcaseeq(a, b)
|
- strcasecmp(a, b) != 0
+ !strcaseeq(a, b)
)
...+>
}

@depends on equality && !has_header && has_local@
@@
+ #include "string-util.h"
  #include "..."

@depends on equality && !has_header && !has_local && has_string@
@@
+ #include "string-util.h"
+
  #include <string.h>

@depends on equality && !has_header && !has_local && !has_string && has_strings@
@@
+ #include "string-util.h"
+
  #include <strings.h>
