// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// A native method prevents other members from having that name, including
// fields.  However, native fields keep their name.  The implication: a getter
// for the field must be based on the field's name, not the field's jsname.

import "package:expect/expect.dart";
import 'native_metadata.dart';

interface I {
  int key;
}

@Native("*A")
class A implements I  {
  int key;                    //  jsname is 'key'
  int getKey() => key;
}

class B implements I {
  int key;                    //  jsname is not 'key'
  B([this.key = 222]);
  int getKey() => key;
}

@Native("*X")
class X {
  @Native('key') int native_key_method();
  // This should cause B.key to be renamed, but not A.key.
  @Native('key') int key();
}

@native A makeA();
@native X makeX();


@Native("""
// This code is all inside 'setup' and so not accesible from the global scope.
function A(){ this.key = 111; }
A.prototype.getKey = function(){return this.key;};

function X(){}
X.prototype.key = function(){return 666;};

makeA = function(){return new A};
makeX = function(){return new X};
""")
void setup();

testDynamic() {
  var things = [makeA(), new B(), makeX()];
  var a = things[0];
  var b = things[1];
  var x = things[2];

  Expect.equals(111, a.key);
  Expect.equals(222, b.key);
  Expect.equals(111, a.getKey());
  Expect.equals(222, b.getKey());

  Expect.equals(666, x.native_key_method());
  Expect.equals(666, x.key());
  var fn = x.key;
  Expect.equals(666, fn());
}

testPartial() {
  var things = [makeA(), new B(), makeX()];
  I a = things[0];
  I b = things[1];

  // All subtypes of I have a field 'key'. The compiler might be tempted to
  // generate a direct property access, but that will fail since one of the
  // fields is renamed.  A getter call is required here.
  Expect.equals(111, a.key);
  Expect.equals(222, b.key);
}

testTyped() {
  A a = makeA();
  B b = new B();
  X x = makeX();

  Expect.equals(666, x.native_key_method());
  Expect.equals(111, a.key);
  Expect.equals(222, b.key);
  Expect.equals(111, a.getKey());
  Expect.equals(222, b.getKey());
}

main() {
  setup();

  testTyped();
  testPartial();
  testDynamic();
}
