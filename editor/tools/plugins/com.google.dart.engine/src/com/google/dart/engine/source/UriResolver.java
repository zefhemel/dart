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
package com.google.dart.engine.source;

import java.net.URI;

/**
 * The abstract class {@code UriResolver} defines the behavior of objects that are used to resolve
 * URI's for a source factory. Subclasses of this class are expected to resolve a single scheme of
 * absolute URI.
 * 
 * @coverage dart.engine.source
 */
public abstract class UriResolver {
  /**
   * Initialize a newly created resolver.
   */
  public UriResolver() {
    super();
  }

  /**
   * Resolve the given absolute URI. Return a {@link Source source} representing the file to which
   * it was resolved, or {@code null} if it could not be resolved.
   * 
   * @param contentCache the content cache used to access the contents of the returned source
   * @param uri the URI to be resolved
   * @return a {@link Source source} representing the URI to which given URI was resolved
   */
  public abstract Source resolveAbsolute(ContentCache contentCache, URI uri);
}
