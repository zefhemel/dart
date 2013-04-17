// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.
// VMOptions=--enable_type_checks

// Section 9, type variables are not valid in identifier expressions

class A {
  static func() { return "class A"; }
}

class B<T> {
  doFunc() {
    T.func(); // illegal to use type variable as an identifier expression
  }
}

main() {
  try {
    var buf = new B<A>().doFunc();
    print(buf);
  } on Exception catch (e) {
    // should be an uncatchable compile-time error,
  }
}
