/*
 * Copyright (c) 2013, the Dart project authors.
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
package com.google.dart.engine.internal.error;

import com.google.dart.engine.ast.ASTNode;
import com.google.dart.engine.error.AnalysisError;
import com.google.dart.engine.error.AnalysisErrorListener;
import com.google.dart.engine.error.ErrorCode;
import com.google.dart.engine.scanner.Token;
import com.google.dart.engine.source.Source;

/**
 * Instances of the class {@code ErrorReporter} wrap an error listener with utility methods used to
 * create the errors being reported.
 * 
 * @coverage dart.engine.error
 */
public class ErrorReporter {
  /**
   * The error listener to which errors will be reported.
   */
  private AnalysisErrorListener errorListener;

  /**
   * The default source to be used when reporting errors.
   */
  private Source defaultSource;

  /**
   * The source to be used when reporting errors.
   */
  private Source source;

  /**
   * Initialize a newly created error reporter that will report errors to the given listener.
   * 
   * @param errorListener the error listener to which errors will be reported
   * @param defaultSource the default source to be used when reporting errors
   */
  public ErrorReporter(AnalysisErrorListener errorListener, Source defaultSource) {
    if (errorListener == null) {
      throw new IllegalArgumentException("An error listener must be provided");
    } else if (defaultSource == null) {
      throw new IllegalArgumentException("A default source must be provided");
    }
    this.errorListener = errorListener;
    this.defaultSource = defaultSource;
    this.source = defaultSource;
  }

  /**
   * Report an error with the given error code and arguments.
   * 
   * @param errorCode the error code of the error to be reported
   * @param node the node specifying the location of the error
   * @param arguments the arguments to the error, used to compose the error message
   */
  public void reportError(ErrorCode errorCode, ASTNode node, Object... arguments) {
    errorListener.onError(new AnalysisError(
        source,
        node.getOffset(),
        node.getLength(),
        errorCode,
        arguments));
  }

  /**
   * Report an error with the given error code and arguments.
   * 
   * @param errorCode the error code of the error to be reported
   * @param token the token specifying the location of the error
   * @param arguments the arguments to the error, used to compose the error message
   */
  public void reportError(ErrorCode errorCode, Token token, Object... arguments) {
    errorListener.onError(new AnalysisError(
        source,
        token.getOffset(),
        token.getLength(),
        errorCode,
        arguments));
  }

  /**
   * Set the source to be used when reporting errors. Setting the source to {@code null} will cause
   * the default source to be used.
   * 
   * @param source the source to be used when reporting errors
   */
  public void setSource(Source source) {
    this.source = source == null ? defaultSource : source;
  }
}
