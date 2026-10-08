@shadow@
type T, local_type;
identifier fn, parameter;
position p;
@@
fn@p(..., T *parameter, ...) {
<+...
local_type parameter;
...+>
}

@disable optional_qualifier@
type T;
identifier fn, parameter, callee;
position p != shadow.p;
@@
fn@p(..., T *parameter, ...) {
<...
- (void)parameter;
...
callee(..., parameter, ...)
...>
}
