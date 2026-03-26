; REQUIRES: x86
; RUN: rm -rf %t.dir && mkdir -p %t.dir && cd %t.dir
; RUN: opt -thinlto-bc -o main.obj %s
; RUN: opt -thinlto-bc -o foo.obj %S/Inputs/lto-dep.ll
; RUN: rm -rf cache && mkdir cache
; RUN: lld-link /lldltocache:cache /lldsavetemps:prelink /out:main.exe /entry:main /subsystem:console main.obj foo.obj
; RUN: rm main.exe.lto.main.obj main.exe.lto.foo.obj
; RUN: lld-link /lldltocache:cache /lldsavetemps:prelink /out:main.exe /entry:main /subsystem:console main.obj foo.obj
; RUN: llvm-readobj --file-header main.exe.lto.main.obj | FileCheck %s --check-prefix=OBJ
; RUN: llvm-readobj --file-header main.exe.lto.foo.obj | FileCheck %s --check-prefix=OBJ

; OBJ: Format: COFF-x86-64

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc"

define i32 @main() {
  call void @foo()
  ret i32 0
}

declare void @foo()
