// Part of the Carbon Language project, under the Apache License v2.0 with LLVM
// Exceptions. See /LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// NOAUTOUPDATE

package ExplorerTest api;

base class Point {
  var x: i32;
  var y: i32;
}

class Point3D {
  extend base: Point;
  var z: i32;
}

fn Main() -> i32 {
  var p: Point3D = {.base={.x = 1, .y = 2}, .z = 3};
  // CHECK:STDERR: COMPILATION ERROR: fail_field_assign_mismatch_base.carbon:[[@LINE+1]]: class Point3D does not have a field named a
  p.a = 0;
  return 0;
}
