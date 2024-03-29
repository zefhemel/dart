// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

part of dart.collection;

/**
 * An unmodifiable [List] view of another List.
 *
 * The source of the elements may be a [List] or any [Iterable] with
 * efficient [Iterable.length] and [Iterable.elementAt].
 */
class UnmodifiableListView<E> extends UnmodifiableListBase<E> {
  Iterable<E> _source;
  /** Create an unmodifiable list backed by [source]. */
  UnmodifiableListView(Iterable<E> source) : _source = source;
  int get length => _source.length;
  E operator[](int index) => _source.elementAt(index);
}
