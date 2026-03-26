# REQUIRES: x86
# RUN: split-file %s %t.dir && cd %t.dir && \
# RUN:   llvm-mc -filetype=obj -triple=x86_64-windows-gnu main.s -o main.obj && \
# RUN:   llvm-mc -filetype=obj -triple=x86_64-windows-gnu imp.s -o imp.obj && \
# RUN:   not lld-link -lldmingw /entry:main /subsystem:console /out:%t.exe main.obj imp.obj 2>&1 | FileCheck %s

# CHECK: warning: unable to automatically import foo from __imp_foo from <internal>; unexpected symbol type
# CHECK: error: undefined symbol: foo

#--- main.s
.data
.quad foo

.text
.globl main
main:
  xorl %eax, %eax
  retq

#--- imp.s
.globl __imp_foo
.set __imp_foo, 1
