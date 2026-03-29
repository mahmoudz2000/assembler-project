; Example assembly file demonstrating macro usage and error handling.

mcro M1
mov r3,r4
mcroend

; Invoke the macro
M1

; Declare an external symbol that will be referenced but never defined.
.extern X

; Declare an entry symbol that is never defined
.entry Y

; Invalid instruction with too many operands (should trigger an error)
add r1,r2,r3

stop
