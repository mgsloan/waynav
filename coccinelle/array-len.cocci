// Use the shared array count helper in waynav translation units.
@project@
@@
#include "waynav.h"

@depends on project@
identifier fn;
expression array;
@@
fn(...) {
<...
- sizeof(array) / sizeof(array[0])
+ ARRAY_LEN(array)
...>
}
