// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// NOAUTOUPDATE

package ExplorerTest api;

interface IFace {
  // CHECK:STDERR: COMPILATION ERROR: fail_use_assoc_const_before_decl.carbon:[[@LINE+1]]: 'C' has not been declared yet
  let B:! C;
  let C:! IFace;
}

fn Main() -> i32 {
  // return T;
}
