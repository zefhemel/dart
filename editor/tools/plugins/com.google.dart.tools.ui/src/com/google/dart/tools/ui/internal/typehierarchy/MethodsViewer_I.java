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

package com.google.dart.tools.ui.internal.typehierarchy;

import org.eclipse.jface.action.IToolBarManager;
import org.eclipse.ui.IMemento;

/**
 * Interface for methods viewer in {@link TypeHierarchyViewPart}.
 */
public interface MethodsViewer_I {
  void contributeToToolBar(IToolBarManager tbm);

  void restoreState(IMemento memento);

  void saveState(IMemento memento);

  void setInputType(Object inputType);
}
