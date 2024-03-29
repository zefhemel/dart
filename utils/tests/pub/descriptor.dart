// Copyright (c) 2013, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

/// Pub-specific scheduled_test descriptors.
library descriptor;

import 'package:oauth2/oauth2.dart' as oauth2;
import 'package:scheduled_test/scheduled_server.dart';
import 'package:scheduled_test/scheduled_test.dart';
import 'package:scheduled_test/descriptor.dart';

import '../../pub/utils.dart';
import 'descriptor/git.dart';
import 'descriptor/tar.dart';
import 'test_pub.dart';

export 'package:scheduled_test/descriptor.dart';
export 'descriptor/git.dart';
export 'descriptor/tar.dart';

/// Creates a new [GitRepoDescriptor] with [name] and [contents].
GitRepoDescriptor git(String name, [Iterable<Descriptor> contents]) =>
    new GitRepoDescriptor(name, contents == null ? <Descriptor>[] : contents);

/// Creates a new [TarRepoDescriptor] with [name] and [contents].
TarFileDescriptor tar(String name, [Iterable<Descriptor> contents]) =>
    new TarFileDescriptor(name, contents == null ? <Descriptor>[] : contents);

/// Describes a package that passes all validation.
Descriptor get validPackage => dir(appPath, [
  libPubspec("test_pkg", "1.0.0"),
  file("LICENSE", "Eh, do what you want."),
  dir("lib", [
    file("test_pkg.dart", "int i = 1;")
  ])
]);

/// Describes a file named `pubspec.yaml` with the given YAML-serialized
/// [contents], which should be a serializable object.
///
/// [contents] may contain [Future]s that resolve to serializable objects,
/// which may in turn contain [Future]s recursively.
Descriptor pubspec(Map contents) {
  return async(awaitObject(contents).then((resolvedContents) =>
      file("pubspec.yaml", yaml(resolvedContents))));
}

/// Describes a file named `pubspec.yaml` for an application package with the
/// given [dependencies].
Descriptor appPubspec(List dependencies) {
  return pubspec({
    "name": "myapp",
    "dependencies": dependencyListToMap(dependencies)
  });
}

/// Describes a file named `pubspec.yaml` for a library package with the given
/// [name], [version], and [deps]. If "sdk" is given, then it adds an SDK
/// constraint on that version.
Descriptor libPubspec(String name, String version, {List deps, String sdk}) {
  var map = packageMap(name, version, deps);

  if (sdk != null) {
    map["environment"] = {
      "sdk": sdk
    };
  }

  return pubspec(map);
}

/// Describes a directory named `lib` containing a single dart file named
/// `<name>.dart` that contains a line of Dart code.
Descriptor libDir(String name, [String code]) {
  // Default to printing the name if no other code was given.
  if (code == null) {
    code = name;
  }

  return dir("lib", [
    file("$name.dart", 'main() => "$code";')
  ]);
}

/// Describes a directory for a package installed from the mock package server.
/// This directory is of the form found in the global package cache.
Descriptor packageCacheDir(String name, String version) {
  return dir("$name-$version", [
    libDir(name, '$name $version')
  ]);
}

/// Describes a directory for a Git package. This directory is of the form
/// found in the revision cache of the global package cache.
Descriptor gitPackageRevisionCacheDir(String name, [int modifier]) {
  var value = name;
  if (modifier != null) value = "$name $modifier";
  return pattern(new RegExp("$name${r'-[a-f0-9]+'}"),
      (dirName) => dir(dirName, [libDir(name, value)]));
}

/// Describes a directory for a Git package. This directory is of the form
/// found in the repo cache of the global package cache.
Descriptor gitPackageRepoCacheDir(String name) {
  return pattern(new RegExp("$name${r'-[a-f0-9]+'}"),
      (dirName) => dir(dirName, [
    dir('hooks'),
    dir('info'),
    dir('objects'),
    dir('refs')
  ]));
}

/// Describes the `packages/` directory containing all the given [packages],
/// which should be name/version pairs. The packages will be validated against
/// the format produced by the mock package server.
///
/// A package with a null version should not be installed.
Descriptor packagesDir(Map<String, String> packages) {
  var contents = <Descriptor>[];
  packages.forEach((name, version) {
    if (version == null) {
      contents.add(nothing(name));
    } else {
      contents.add(dir(name, [
        file("$name.dart", 'main() => "$name $version";')
      ]));
    }
  });
  return dir(packagesPath, contents);
}

/// Describes the global package cache directory containing all the given
/// [packages], which should be name/version pairs. The packages will be
/// validated against the format produced by the mock package server.
///
/// A package's value may also be a list of versions, in which case all
/// versions are expected to be installed.
Descriptor cacheDir(Map packages) {
  var contents = <Descriptor>[];
  packages.forEach((name, versions) {
    if (versions is! List) versions = [versions];
    for (var version in versions) {
      contents.add(packageCacheDir(name, version));
    }
  });
  return dir(cachePath, [
    dir('hosted', [
      async(port.then((p) => dir('localhost%58$p', contents)))
    ])
  ]);
}

/// Describes the file in the system cache that contains the client's OAuth2
/// credentials. The URL "/token" on [server] will be used as the token
/// endpoint for refreshing the access token.
Descriptor credentialsFile(
    ScheduledServer server,
    String accessToken,
    {String refreshToken,
     DateTime expiration}) {
  return async(server.url.then((url) {
    return dir(cachePath, [
      file('credentials.json', new oauth2.Credentials(
          accessToken,
          refreshToken,
          url.resolve('/token'),
          ['https://www.googleapis.com/auth/userinfo.email'],
          expiration).toJson())
    ]);
  }));
}

/// Describes the application directory, containing only a pubspec specifying
/// the given [dependencies].
DirectoryDescriptor appDir(List dependencies) =>
  dir(appPath, [appPubspec(dependencies)]);
