// Copyright (c) 2011, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of js_backend;

/**
 * Assigns JavaScript identifiers to Dart variables, class-names and members.
 */
class MinifyNamer extends Namer {
  MinifyNamer(Compiler compiler) : super(compiler) {
    reserveBackendNames();
  }

  String get isolateName => 'I';
  String get isolatePropertiesName => 'p';
  bool get shouldMinify => true;

  final String getterPrefix = 'g';
  final String setterPrefix = 's';

  const ALPHABET_CHARACTERS = 52;  // a-zA-Z.
  const ALPHANUMERIC_CHARACTERS = 62;  // a-zA-Z0-9.

  // You can pass an invalid identifier to this and unlike its non-minifying
  // counterpart it will never return the proposedName as the new fresh name.
  String getFreshName(String proposedName,
                      Set<String> usedNames,
                      Map<String, String> suggestedNames,
                      {bool ensureSafe: true}) {
    var freshName;
    var suggestion = suggestedNames[proposedName];
    if (suggestion != null && !usedNames.contains(suggestion)) {
      freshName = suggestion;
    } else {
      freshName = _getUnusedName(proposedName, usedNames);
    }
    usedNames.add(freshName);
    return freshName;
  }

  SourceString getClosureVariableName(SourceString name, int id) {
    if (id < ALPHABET_CHARACTERS) {
      return new SourceString(new String.fromCharCodes([_letterNumber(id)]));
    }
    return new SourceString("${getMappedInstanceName('closure')}_$id");
  }

  void reserveBackendNames() {
    // From issue 7554.  These should not be used on objects (as instance
    // variables) because they clash with names from the DOM.
    const reservedNativeProperties = const <String>[
        'Q', 'a', 'b', 'c', 'd', 'e', 'f', 'r', 'x', 'y', 'z',
        // 2-letter:
        'ch', 'cx', 'cy', 'db', 'dx', 'dy', 'fr', 'fx', 'fy', 'go', 'id', 'k1',
        'k2', 'k3', 'k4', 'r1', 'r2', 'rx', 'ry', 'x1', 'x2', 'y1', 'y2',
        // 3-letter:
        'add', 'all', 'alt', 'arc', 'CCW', 'cmp', 'dir', 'end', 'get', 'in1',
        'in2', 'INT', 'key', 'log', 'low', 'm11', 'm12', 'm13', 'm14', 'm21',
        'm22', 'm23', 'm24', 'm31', 'm32', 'm33', 'm34', 'm41', 'm42', 'm43',
        'm44', 'max', 'min', 'now', 'ONE', 'put', 'red', 'rel', 'rev', 'RGB',
        'sdp', 'set', 'src', 'tag', 'top', 'uid', 'uri', 'url', 'URL',
        // 4-letter:
        'abbr', 'atob', 'Attr', 'axes', 'axis', 'back', 'BACK', 'beta', 'bias',
        'Blob', 'blue', 'blur', 'BLUR', 'body', 'BOOL', 'BOTH', 'btoa', 'BYTE',
        'cite', 'clip', 'code', 'cols', 'cues', 'data', 'DECR', 'DONE', 'face',
        'file', 'File', 'fill', 'find', 'font', 'form', 'gain', 'hash', 'head',
        'high', 'hint', 'host', 'href', 'HRTF', 'IDLE', 'INCR', 'info', 'INIT',
        'isId', 'item', 'KEEP', 'kind', 'knee', 'lang', 'left', 'LESS', 'line',
        'link', 'list', 'load', 'loop', 'mode', 'name', 'Node', 'None', 'NONE',
        'only', 'open', 'OPEN', 'ping', 'play', 'port', 'rect', 'Rect', 'refX',
        'refY', 'RGBA', 'root', 'rows', 'save', 'seed', 'seek', 'self', 'send',
        'show', 'SINE', 'size', 'span', 'stat', 'step', 'stop', 'tags', 'text',
        'Text', 'time', 'type', 'view', 'warn', 'wrap', 'ZERO'];

    for (var name in reservedNativeProperties) {
      if (name.length < 2) {
        instanceNameMap[name] = name;
      }
      usedInstanceNames.add(name);
      // Getter and setter names are autogenerated by prepending 'g' and 's' to
      // field names.  Therefore there are some field names we don't want to
      // use.  It is implicit in the next line that the banned prefix is
      // only one character.
      if (_hasBannedPrefix(name)) usedInstanceNames.add(name.substring(1));
    }

    // These popular names are present in most programs and deserve
    // single character minified names.  We could determine the popular names
    // individually per program, but that would mean that the output of the
    // minifier was less stable from version to version of the program being
    // minified.
    _populateSuggestedNames(
        suggestedInstanceNames,
        usedInstanceNames,
        const <String>[
            r'$add', r'add$1', r'$and', r'codeUnitAt$1', r'$or',
            r'current', r'$shr', r'$eq', r'$ne',
            r'getPrototypeOf', r'hasOwnProperty', r'$index', r'$indexSet',
            r'$isJavaScriptIndexingBehavior', r'$xor',
            r'iterator', r'length', r'$lt', r'$gt', r'$le', r'$ge',
            r'moveNext$0', r'node', r'on', r'$negate', r'push', r'self',
            r'start', r'target', r'$shl', r'value', r'width', r'style',
            r'noSuchMethod$1', r'$mul', r'$div', r'$sub', r'$not', r'$mod',
            r'$tdiv']);

    _populateSuggestedNames(
        suggestedGlobalNames,
        usedGlobalNames,
        const <String>[
            r'Object', 'wrapException', r'$eq', r'S', r'ioore',
            r'UnsupportedError$', r'length', r'$sub',
            r'getInterceptor$JSArrayJSString', r'$add',
            r'$gt', r'$ge', r'$lt', r'$le', r'add', r'getInterceptor$JSNumber',
            r'iterator', r'$index', r'iae', r'getInterceptor$JSArray',
            r'ArgumentError$', r'BoundClosure', r'StateError$',
            r'getInterceptor', r'max', r'$mul', r'List_List', r'Map_Map',
            r'getInterceptor$JSString', r'$div', r'$indexSet',
            r'List_List$from', r'Set_Set$from', r'toString', r'toInt', r'min',
            r'StringBuffer_StringBuffer', r'contains1', r'WhereIterable$',
            r'RangeError$value', r'JSString', r'JSNumber',
            r'JSArray', r'createInvocationMirror'
            ]);
  }

  void _populateSuggestedNames(Map<String, String> suggestionMap,
                               Set<String> used,
                               List<String> suggestions) {
    int c = $a - 1;
    String letter;
    for (String name in suggestions) {
      do {
        assert(c != $Z);
        c = (c == $z) ? $A : c + 1;
        letter = new String.fromCharCodes([c]);
      } while (used.contains(letter));
      assert(suggestionMap[name] == null);
      suggestionMap[name] = letter;
    }
  }


  // This gets a minified name based on a hash of the proposed name.  This
  // is slightly less efficient than just getting the next name in a series,
  // but it means that small changes in the input program will give smallish
  // changes in the output, which can be useful for diffing etc.
  String _getUnusedName(String proposedName, Set<String> usedNames) {
    int hash = _calculateHash(proposedName);
    // Avoid very small hashes that won't try many names.
    hash = hash < 1000 ? hash * 314159 : hash;  // Yes, it's prime.

    // Try other n-character names based on the hash.  We try one to three
    // character identifiers.  For each length we try around 10 different names
    // in a predictable order determined by the proposed name.  This is in order
    // to make the renamer stable: small changes in the input should nornally
    // result in relatively small changes in the output.
    for (var n = 2; n <= 3; n++) {
      int h = hash;
      while (h > 10) {
        var codes = <int>[_letterNumber(h)];
        int h2 = h ~/ ALPHABET_CHARACTERS;
        for (var i = 1; i < n; i++) {
          codes.add(_alphaNumericNumber(h2));
          h2 ~/= ALPHANUMERIC_CHARACTERS;
        }
        final candidate = new String.fromCharCodes(codes);
        if (!usedNames.contains(candidate) &&
            !jsReserved.contains(candidate) &&
            !_hasBannedPrefix(candidate)) {
          return candidate;
        }
        // Try again with a slightly different hash.  After around 10 turns
        // around this loop h is zero and we try a longer name.
        h ~/= 7;
      }
    }

    // If we can't find a hash based name in the three-letter space, then base
    // the name on a letter and a counter.
    var startLetter = new String.fromCharCodes([_letterNumber(hash)]);
    var i = 0;
    while (usedNames.contains("$startLetter$i")) {
      i++;
    }
    // We don't need to check for banned prefix because the name is in the form
    // xnnn, where nnn is a number.  There can be no getter or setter called
    // gnnn since that would imply a numeric field name.
    return "$startLetter$i";
  }

  /// Instance members starting with g and s are reserved for getters and
  /// setters.
  bool _hasBannedPrefix(String name) {
    int code = name.codeUnitAt(0);
    return code == $g || code == $s;
  }

  int _calculateHash(String name) {
    int h = 0;
    for (int i = 0; i < name.length; i++) {
      h += name.codeUnitAt(i);
      h &= 0xffffffff;
      h += h << 10;
      h &= 0xffffffff;
      h ^= h >> 6;
      h &= 0xffffffff;
    }
    return h;
  }

  int _letterNumber(int x) {
    if (x >= ALPHABET_CHARACTERS) x %= ALPHABET_CHARACTERS;
    if (x < 26) return $a + x;
    return $A + x - 26;
  }

  int _alphaNumericNumber(int x) {
    if (x >= ALPHANUMERIC_CHARACTERS) x %= ALPHANUMERIC_CHARACTERS;
    if (x < 26) return $a + x;
    if (x < 52) return $A + x - 26;
    return $0 + x - 52;
  }

}