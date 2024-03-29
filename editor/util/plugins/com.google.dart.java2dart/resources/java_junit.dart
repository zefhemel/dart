library java.junit;

import 'package:unittest/unittest.dart' hide fail;
import 'package:unittest/unittest.dart' as _ut show fail;


class JUnitTestCase {
  void setUp() {}
  void tearDown() {}
  static void fail(String msg) {
    _ut.fail(msg);
  }
  static void assertTrue(bool x) {
    expect(x, isTrue);
  }
  static void assertTrueMsg(String msg, bool x) {
    expect(x, isTrueMsg(msg));
  }
  static void assertFalse(bool x) {
    expect(x, isFalse);
  }
  static void assertFalseMsg(String msg, bool x) {
    expect(x, isFalseMsg(msg));
  }
  static void assertNull(x) {
    expect(x, isNull);
  }
  static void assertNotNull(x) {
    expect(x, isNotNull);
  }
  static void assertNotNullMsg(String msg, x) {
    expect(x, isNotNullMsg(msg));
  }
  static void assertEquals(expected, actual) {
    expect(actual, equals(expected));
  }
  static void assertEqualsMsg(String msg, expected, actual) {
    expect(actual, equalsMsg(msg, expected));
  }
  static void assertSame(expected, actual) {
    expect(actual, same(expected));
  }
}

runJUnitTest(testInstance, Function testFunction) {
  testInstance.setUp();
  try {
    testFunction();
  } finally {
    testInstance.tearDown();
  }
}

/**
 * Returns a matches that matches if the value is not the same instance
 * as [object] (`!==`).
 */
Matcher notSame(expected) => new _IsNotSameAs(expected);

class _IsNotSameAs extends BaseMatcher {
  final _expected;
  const _IsNotSameAs(this._expected);
  bool matches(item, MatchState matchState) => !identical(item, _expected);
  Description describe(Description description) =>
      description.add('not same instance as ').addDescriptionOf(_expected);
}

Matcher equalsMsg(String msg, expected) => new _EqualsWithMessage(msg, expected);
class _EqualsWithMessage extends BaseMatcher {
  final String msg;
  final expectedValue;
  const _EqualsWithMessage(this.msg, this.expectedValue);
  bool matches(item, MatchState matchState) {
    return item == expectedValue;
  }
  Description describe(Description mismatchDescription) {
    return mismatchDescription.replace(msg);
  }
  Description describeMismatch(item, Description mismatchDescription,
                               MatchState matchState, bool verbose) {
    return mismatchDescription.replace(msg).add(" $item != $expectedValue");
  }
}

Matcher isTrueMsg(String msg) => new _IsTrueWithMessage(msg);
class _IsTrueWithMessage extends BaseMatcher {
  final String msg;
  const _IsTrueWithMessage(this.msg);
  bool matches(item, MatchState matchState) {
    return item == true;
  }
  Description describe(Description mismatchDescription) {
    return mismatchDescription.replace(msg);
  }
  Description describeMismatch(item, Description mismatchDescription,
                               MatchState matchState, bool verbose) {
    return mismatchDescription.replace(msg).add(" $item is not true");
  }
}

Matcher isFalseMsg(String msg) => new _IsFalseWithMessage(msg);
class _IsFalseWithMessage extends BaseMatcher {
  final String msg;
  const _IsFalseWithMessage(this.msg);
  bool matches(item, MatchState matchState) {
    return item == false;
  }
  Description describe(Description mismatchDescription) {
    return mismatchDescription.replace(msg);
  }
  Description describeMismatch(item, Description mismatchDescription,
                               MatchState matchState, bool verbose) {
    return mismatchDescription.replace(msg).add(" $item is not false");
  }
}

Matcher isNotNullMsg(String msg) => new _IsNotNullWithMessage(msg);
class _IsNotNullWithMessage extends BaseMatcher {
  final String msg;
  const _IsNotNullWithMessage(this.msg);
  bool matches(item, MatchState matchState) {
    return item != null;
  }
  Description describe(Description mismatchDescription) {
    return mismatchDescription.replace(msg);
  }
  Description describeMismatch(item, Description mismatchDescription,
                               MatchState matchState, bool verbose) {
    return mismatchDescription.replace(msg).add(" $item is null");
  }
}
