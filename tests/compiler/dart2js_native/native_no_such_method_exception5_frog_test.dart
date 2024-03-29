// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "package:expect/expect.dart";

class GetName {
  foo(x, [y]) => "foo";
  baz(x, y, z) => "baz";
}

String getName(im) => im.invokeOn(new GetName());

class A native "*A" {
  bar() => 42;
  noSuchMethod(x) => "native(${getName(x)}:${x.positionalArguments})";
}

class B native "*B" {
  baz() => 42;
}

class C {
  static create() => new C();
  noSuchMethod(x) => "${getName(x)}:${x.positionalArguments}";
}

makeA() native;

setup() native """
  function A() {}
  makeA = function() { return new A; }
""";

main() {
  setup();
  var a = makeA();
  a.bar();
  Expect.equals("native(foo:[1, 2])", a.foo(1, 2));
  Expect.equals("native(baz:[3, 4, 5])", a.baz(3, 4, 5));
  var c = C.create();
  Expect.equals("foo:[6]", c.foo(6));
}
