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
package com.google.dart.engine.internal.element.handle;

import com.google.dart.engine.element.ClassElement;
import com.google.dart.engine.element.ConstructorElement;
import com.google.dart.engine.element.ElementKind;
import com.google.dart.engine.element.FieldElement;
import com.google.dart.engine.element.LibraryElement;
import com.google.dart.engine.element.MethodElement;
import com.google.dart.engine.element.PropertyAccessorElement;
import com.google.dart.engine.element.TypeVariableElement;
import com.google.dart.engine.type.InterfaceType;

/**
 * Instances of the class {@code ClassElementHandle} implement a handle to a {@code ClassElement}.
 * 
 * @coverage dart.engine.element
 */
public class ClassElementHandle extends ElementHandle implements ClassElement {
  /**
   * Initialize a newly created element handle to represent the given element.
   * 
   * @param element the element being represented
   */
  public ClassElementHandle(ClassElement element) {
    super(element);
  }

  @Override
  public PropertyAccessorElement[] getAccessors() {
    return getActualElement().getAccessors();
    // TODO(brianwilkerson) Decide whether we need to return handles to all of the accessors rather
    // than just returning the accessors themselves.
    //
    // return forElements(getActualElement().getAccessors());
  }

  @Override
  public InterfaceType[] getAllSupertypes() {
    return getActualElement().getAllSupertypes();
  }

  @Override
  public ConstructorElement[] getConstructors() {
    return getActualElement().getConstructors();
  }

  @Override
  public FieldElement[] getFields() {
    return getActualElement().getFields();
  }

  @Override
  public InterfaceType[] getInterfaces() {
    return getActualElement().getInterfaces();
  }

  @Override
  public ElementKind getKind() {
    return ElementKind.CLASS;
  }

  @Override
  public MethodElement[] getMethods() {
    return getActualElement().getMethods();
  }

  @Override
  public InterfaceType[] getMixins() {
    return getActualElement().getMixins();
  }

  @Override
  public ConstructorElement getNamedConstructor(String name) {
    return getActualElement().getNamedConstructor(name);
  }

  @Override
  public InterfaceType getSupertype() {
    return getActualElement().getSupertype();
  }

  @Override
  public InterfaceType getType() {
    return getActualElement().getType();
  }

  @Override
  public TypeVariableElement[] getTypeVariables() {
    return getActualElement().getTypeVariables();
  }

  @Override
  public ConstructorElement getUnnamedConstructor() {
    return getActualElement().getUnnamedConstructor();
  }

  @Override
  public boolean isAbstract() {
    return getActualElement().isAbstract();
  }

  @Override
  public boolean isTypedef() {
    return getActualElement().isTypedef();
  }

  @Override
  public boolean isValidMixin() {
    return getActualElement().isValidMixin();
  }

  @Override
  public PropertyAccessorElement lookUpGetter(String getterName, LibraryElement library) {
    return getActualElement().lookUpGetter(getterName, library);
  }

  @Override
  public MethodElement lookUpMethod(String methodName, LibraryElement library) {
    return getActualElement().lookUpMethod(methodName, library);
  }

  @Override
  public PropertyAccessorElement lookUpSetter(String setterName, LibraryElement library) {
    return getActualElement().lookUpSetter(setterName, library);
  }

  @Override
  protected ClassElement getActualElement() {
    return (ClassElement) super.getActualElement();
  }
}
