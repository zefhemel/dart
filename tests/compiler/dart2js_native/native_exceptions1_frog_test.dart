// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "package:expect/expect.dart";

// Test that hidden native exception classes can be marked as existing.
//
// To infer which native hidden types exist, we need
//   (1) return types of native methods and getters
//   (2) argument types of callbacks
//   (3) exceptions thrown by the operation.
//
// (1) and (2) can be achieved by having nicely typed native methods, but there
// is no place in the Dart language to communicate (3).  So we use the following
// fake body technique.


// The exception type.
class E native "*E" {
  E._used() native;  // Bogus native constructor, called only from fake body.

  final int code;
}

// Type with exception-throwing methods.
class A native "*A" {
  op(int x) native {
    // Fake body calls constructor to mark the exception class (E) as used.
    throw new E._used();
  }
}

// This class is here just so that a dynamic context is polymorphic.
class B {
  int get code => 666;
  op(String x) => 123;
}

makeA() native;

void setup1() native """
// Ensure we are not relying on global names 'A' and 'E'.
A = null;
E = null;
""";

void setup2() native """
// This code is all inside 'setup2' and so not accesible from the global scope.
function E(x){ this.code = x; }

function A(){}
A.prototype.op = function (x) {
  if (x & 1) throw new E(100);
  return  x / 2;
};
makeA = function(){return new A};
""";

int inscrutable(int x) => x == 0 ? 0 : x | inscrutable(x & (x - 1));

main() {
  setup1();
  setup2();

  var things = [makeA(), new B()];
  var a = things[inscrutable(0)];
  var b = things[inscrutable(1)];

  Expect.equals(25, a.op(50));
  Expect.equals(123, b.op('hello'));
  Expect.equals(666, b.code);

  bool threw = false;
  try {
    var x = a.op(51);
  } catch (e) {
    threw = true;
    Expect.equals(100, e.code);
    Expect.isTrue(e is E);
  }
  Expect.isTrue(threw);

  // Again, but with statically typed receivers.
  A aa = a;
  B bb = b;

  Expect.equals(25, aa.op(50));
  Expect.equals(123, bb.op('hello'));
  Expect.equals(666, bb.code);

  threw = false;
  try {
    var x = aa.op(51);
  } on E catch (e) {
    threw = true;
    Expect.equals(100, e.code);
    Expect.isTrue(e is E);
  }
  Expect.isTrue(threw);
}
