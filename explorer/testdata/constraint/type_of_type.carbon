// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// NOAUTOUPDATE

package ExplorerTest api;

interface I {}

fn Main() -> i32 {
  var a: auto = I where .Self == i32;
  // OK: type of `I where ...` is `type`.
  a = i32;
  return 0;
}

// CHECK:STDOUT: result: 0
