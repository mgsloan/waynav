@has_header@
@@
#include "memory-util.h"

@has_string@
@@
#include <string.h>

@has_local@
@@
#include "..."

@zero depends on has_header || has_string@
identifier fn, object;
@@
fn(...) {
<+...
(
- memset(object, 0, sizeof(*object));
+ ZERO_OBJECT(*object);
|
- memset(&object, 0, sizeof(object));
+ ZERO_OBJECT(object);
)
...+>
}

@depends on zero && !has_header && has_local@
@@
+ #include "memory-util.h"
  #include "..."

@depends on zero && !has_header && !has_local && has_string@
@@
+ #include "memory-util.h"
+
  #include <string.h>
