// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// NOAUTOUPDATE

package ExplorerTest api;

class C {
  fn F() {}
}

class D {
  // CHECK:STDERR: COMPILATION ERROR: fail_extend_final_class.carbon:[[@LINE+1]]: Base class `C` is `final` and cannot be inherited. Add the `base` or `abstract` class prefix to `C` to allow it to be inherited
  extend base: C;
  fn G() {}
}

fn Main() -> i32 {
  return 0;
}
