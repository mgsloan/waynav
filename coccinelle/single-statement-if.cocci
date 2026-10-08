@else_if@
expression a, b;
statement first, second;
position p;
@@
if (a) first else if@p (b) second

@disable drop_else@
expression condition, value;
position p != else_if.p;
@@
- if@p (condition) {
+ if (condition)
(
  value;
|
  return value;
)
- }
