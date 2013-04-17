/*
 * Copyright (c) 2011, the Dart project authors.
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
package com.google.dart.tools.ui;

import com.google.dart.tools.core.dom.AST;
import com.google.dart.tools.core.dom.rewrite.ASTRewrite;
import com.google.dart.tools.core.model.CompilationUnit;

/**
 * The {@link SharedASTProvider} provides access to the {@link CompilationUnit AST root} used by the
 * current active Java editor.
 * <p>
 * For performance reasons, not more than one AST should be kept in memory at a time. Therefore,
 * clients must not keep any references to the shared AST or its nodes or bindings.
 * </p>
 * <p>
 * Clients can make the following assumptions about the AST:
 * <dl>
 * <li>the AST has a {@link ITypeRoot} as source: {@link CompilationUnit#getTypeRoot()} is not null.
 * </li>
 * <li>the {@link AST#apiLevel() AST API level} is {@link AST#JLS3 API level 3} or higher</li>
 * <li>the AST has bindings resolved ({@link AST#hasResolvedBindings()})</li>
 * <li>{@link AST#hasStatementsRecovery() statement} and {@link AST#hasBindingsRecovery() bindings}
 * recovery are enabled</li>
 * </dl>
 * It is possible that in the future a higher API level is used, or that future options will be
 * enabled.
 * </p>
 * <p>
 * The returned AST is shared. It is marked as {@link ASTNode#PROTECT} and must not be modified.
 * Clients are advised to use the non-modifying {@link ASTRewrite} to get update scripts.
 * </p>
 * <p>
 * This class is not intended to be subclassed or instantiated by clients.
 * </p>
 * 
 * @since 3.4
 * @noinstantiate This class is not intended to be instantiated by clients.
 */
public final class SharedASTProvider {

  /**
   * Wait flag class.
   */
  public static final class WAIT_FLAG {

    private String fName;

    private WAIT_FLAG(String name) {
      fName = name;
    }

    /*
     * @see java.lang.Object#toString()
     */
    @Override
    public String toString() {
      return fName;
    }
  }

  /**
   * Wait flag indicating that a client requesting an AST wants to wait until an AST is ready.
   * <p>
   * An AST will be created by this AST provider if the shared AST is not for the given Java
   * element.
   * </p>
   */
  public static final WAIT_FLAG WAIT_YES = new WAIT_FLAG("wait yes"); //$NON-NLS-1$

  /**
   * Wait flag indicating that a client requesting an AST only wants to wait for the shared AST of
   * the active editor.
   * <p>
   * No AST will be created by the AST provider.
   * </p>
   */
  public static final WAIT_FLAG WAIT_ACTIVE_ONLY = new WAIT_FLAG("wait active only"); //$NON-NLS-1$

  /**
   * Wait flag indicating that a client requesting an AST only wants the already available shared
   * AST.
   * <p>
   * No AST will be created by the AST provider.
   * </p>
   */
  public static final WAIT_FLAG WAIT_NO = new WAIT_FLAG("don't wait"); //$NON-NLS-1$

  /**
   * Returns a compilation unit AST for the given Java element. If the element is the input of the
   * active Java editor, the AST is the shared AST.
   * <p>
   * Clients are not allowed to modify the AST and must not keep any references.
   * </p>
   * 
   * @param element the {@link ITypeRoot}, must not be <code>null</code>
   * @param waitFlag {@link #WAIT_YES}, {@link #WAIT_NO} or {@link #WAIT_ACTIVE_ONLY}
   * @param progressMonitor the progress monitor or <code>null</code>
   * @return the AST or <code>null</code>.
   *         <dl>
   *         <li>If {@link #WAIT_NO} has been specified <code>null</code> is returned if the element
   *         is not input of the current Java editor or no AST is available</li>
   *         <li>If {@link #WAIT_ACTIVE_ONLY} has been specified <code>null</code> is returned if
   *         the element is not input of the current Java editor</li>
   *         <li>If {@link #WAIT_YES} has been specified either the shared AST is returned or a new
   *         AST is created.</li>
   *         <li><code>null</code> will be returned if the operation gets canceled.</li>
   *         </dl>
   */
//  public static CompilationUnit getAST(ITypeRoot element, WAIT_FLAG waitFlag,
//      IProgressMonitor progressMonitor) {
//    return DartToolsPlugin.getDefault().getASTProvider().getAST(element, waitFlag, progressMonitor);
//  }

  private SharedASTProvider() {
    // Prevent instantiation.
  }

}