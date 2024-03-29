// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import "package:expect/expect.dart";

// Verify that native fields on classes are not renamed by the minifier.
class A native "*A" {
  int myLongPropertyName;
  int getValue;

  int method(int z) => myLongPropertyName;
}


void setup() native r"""
function getter() {
  return ++this.getValue;
}

function setter(x) {
  this.getValue += 10;
}
  
function A(){
  var a = Object.create(
      { constructor: { name: 'A'}},
      { myLongPropertyName: { get: getter,
                              set: setter,
                              configurable: false,
                              writeable: false
                            }
      });
  a.getValue = 0;
  return a;
}

makeA = function(){return new A;};
""";

A makeA() native { return new A(); }

main() {
  setup();
  var a = makeA();
  a.myLongPropertyName = 21;
  int gotten = a.myLongPropertyName;
  Expect.equals(11, gotten);

  var a2 = makeA();
  if (a2 is A) {
    // Inside this 'if' the compiler knows that a2 is an A, so it is tempted
    // to access myLongPropertyName directly, using its minified name.  But
    // renaming of native properties can only work using getters and setters
    // that access the original name.
    a2.myLongPropertyName = 21;
    int gotten = a2.myLongPropertyName;
    Expect.equals(11, gotten);
  }
}

