// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "package:expect/expect.dart";

// Additional Dart code may be 'placed on' hidden native classes.

class A native "*A" {

  var _field;

  int get X => _field;
  void set X(int x) { _field = x; }

  int method(int z) => _field + z;
}

A makeA() native { return new A(); }

void setup() native """
function A() {}
makeA = function(){return new A;};
""";


main() {
  setup();

  var a = makeA();

  a.X = 100;
  Expect.equals(100, a.X);
  Expect.equals(150, a.method(50));
}
