// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

import 'package:pathos/path.dart' as path;
import 'package:scheduled_test/scheduled_test.dart';

import '../../../pub/entrypoint.dart';
import '../../../pub/validator.dart';
import '../../../pub/validator/directory.dart';
import '../descriptor.dart' as d;
import '../test_pub.dart';
import 'utils.dart';

Validator directory(Entrypoint entrypoint) =>
  new DirectoryValidator(entrypoint);

main() {
  initConfig();

  group('should consider a package valid if it', () {
    setUp(d.validPackage.create);

    integration('looks normal', () => expectNoValidationError(directory));

    integration('has a nested directory named "tools"', () {
      d.dir(appPath, [
        d.dir("foo", [d.dir("tools")])
      ]).create();
      expectNoValidationError(directory);
    });
  });

  group('should consider a package invalid if it has a top-level directory '
      'named', () {
    setUp(d.validPackage.create);

    var names = ["tools", "tests", "docs", "examples", "sample", "samples"];
    for (var name in names) {
      integration('"$name"', () {
        d.dir(appPath, [d.dir(name)]).create();
        expectValidationWarning(directory);
      });
    }
  });
}
