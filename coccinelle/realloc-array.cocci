@has_stdlib@
@@
#include <stdlib.h>

@depends on has_stdlib@
expression pointer, count, element;
type T;
@@
(
- realloc(pointer, count * sizeof(element))
+ reallocarray(pointer, count, sizeof(element))
|
- realloc(pointer, count * sizeof(T))
+ reallocarray(pointer, count, sizeof(T))
)
