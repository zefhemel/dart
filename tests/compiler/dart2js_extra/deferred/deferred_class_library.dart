// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Imported by deferred_class_test.dart.

library deferred_class_library;

class MyClass {
  const MyClass();

  foo(x) {
    print('MyClass.foo($x)');
    return (x - 3) ~/ 2;
  }
}
