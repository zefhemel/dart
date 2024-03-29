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
package com.google.dart.tools.ui.internal.handlers;

import com.google.dart.tools.core.DartCore;
import com.google.dart.tools.core.DartCoreDebug;
import com.google.dart.tools.core.internal.model.PackageLibraryManagerProvider;
import com.google.dart.tools.core.utilities.general.AdapterUtilities;

import org.eclipse.core.commands.AbstractHandler;
import org.eclipse.core.commands.ExecutionEvent;
import org.eclipse.core.commands.ExecutionException;
import org.eclipse.core.resources.IResource;
import org.eclipse.jface.viewers.ISelection;
import org.eclipse.jface.viewers.IStructuredSelection;
import org.eclipse.ui.handlers.HandlerUtil;

/**
 * Handler for ignoring resources.
 */
public class IgnoreResourceHandler extends AbstractHandler {

  @Override
  public Object execute(ExecutionEvent event) throws ExecutionException {

    ISelection selection = HandlerUtil.getCurrentSelection(event);

    if (selection instanceof IStructuredSelection) {
      try {
        for (Object elem : ((IStructuredSelection) selection).toArray()) {
          IResource resource = AdapterUtilities.getAdapter(elem, IResource.class);
          if (resource != null) {
            DartCore.addToIgnores(resource);
            if (!DartCoreDebug.ENABLE_NEW_ANALYSIS) {
              PackageLibraryManagerProvider.getDefaultAnalysisServer().discard(
                  resource.getLocation().toFile());
            }
          }
        }
      } catch (Throwable th) {
        throw new ExecutionException(th.getMessage(), th);
      }

    }

    return null;
  }
}
