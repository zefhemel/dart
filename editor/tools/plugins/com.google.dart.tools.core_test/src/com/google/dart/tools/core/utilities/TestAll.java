/*
 * Copyright (c) 2012, the Dart project authors.
 * 
 * Licensed under the Eclipse Public License v1.0 (the "License"); you may not use this file except
 * in compliance with the License. You may obtain a copy of the License at
 * 
 * http://www.eclipse.org/legal/epl-v10.html
 * 
 * Unless required by applicable law or agreed to in writing, software distributed under the License
 * is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
 * or implied. See the License for the specific language governing permissions and limitations under
 * the License.
 */
package com.google.dart.tools.core.utilities;

import com.google.dart.tools.core.DartCoreDebug;

import junit.framework.Test;
import junit.framework.TestSuite;

public class TestAll {
  public static Test suite() {
    TestSuite suite = new TestSuite("Tests in " + TestAll.class.getPackage().getName());
    if (!DartCoreDebug.ENABLE_NEW_ANALYSIS) {
      suite.addTest(com.google.dart.tools.core.utilities.ast.TestAll.suite());
      suite.addTest(com.google.dart.tools.core.utilities.bindings.TestAll.suite());
    }
    suite.addTest(com.google.dart.tools.core.utilities.collections.TestAll.suite());
    if (!DartCoreDebug.ENABLE_NEW_ANALYSIS) {
      suite.addTest(com.google.dart.tools.core.utilities.compiler.TestAll.suite());
    }
    suite.addTest(com.google.dart.tools.core.utilities.general.TestAll.suite());
    suite.addTest(com.google.dart.tools.core.utilities.io.TestAll.suite());
    if (!DartCoreDebug.ENABLE_NEW_ANALYSIS) {
      suite.addTest(com.google.dart.tools.core.utilities.net.TestAll.suite());
    }
    suite.addTest(com.google.dart.tools.core.utilities.resource.TestAll.suite());
    return suite;
  }
}
