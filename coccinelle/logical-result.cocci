@disable isnt_zero@
expression a, b;
@@
(
- return (a && b) != 0;
+ return a && b;
|
- return (a || b) != 0;
+ return a || b;
)
