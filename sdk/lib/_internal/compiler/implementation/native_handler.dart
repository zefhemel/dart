// Copyright (c) 2012, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

library native;

import 'dart:collection' show Queue;
import 'dart:uri';
import 'dart2jslib.dart' hide SourceString;
import 'dart_types.dart';
import 'elements/elements.dart';
import 'js_backend/js_backend.dart';
import 'resolution/resolution.dart' show ResolverVisitor;
import 'scanner/scannerlib.dart';
import 'ssa/ssa.dart';
import 'tree/tree.dart';
import 'util/util.dart';


/// This class is a temporary work-around until we get a more powerful DartType.
class SpecialType {
  final String name;
  const SpecialType._(this.name);

  /// The type Object, but no subtypes:
  static const JsObject = const SpecialType._('=Object');

  /// The specific implementation of List that is JavaScript Array:
  static const JsArray = const SpecialType._('=List');
}


/**
 * This could be an abstract class but we use it as a stub for the dart_backend.
 */
class NativeEnqueuer {
  /// Initial entry point to native enqueuer.
  void processNativeClasses(Iterable<LibraryElement> libraries) {}

  /// Notification of a main Enqueuer worklist element.  For methods, adds
  /// information from metadata attributes, and computes types instantiated due
  /// to calling the method.
  void registerElement(Element element) {}

  /// Notification of native field.  Adds information from metadata attributes.
  void handleFieldAnnotations(Element field) {}

  /// Computes types instantiated due to getting a native field.
  void registerFieldLoad(Element field) {}

  /// Computes types instantiated due to setting a native field.
  void registerFieldStore(Element field) {}

  NativeBehavior getNativeBehaviorOf(Send node) => null;

  /**
   * Handles JS-calls, which can be an instantiation point for types.
   *
   * For example, the following code instantiates and returns native classes
   * that are `_DOMWindowImpl` or a subtype.
   *
   *     JS('_DOMWindowImpl', 'window')
   *
   */
  // TODO(sra): The entry from codegen will not have a resolver.
  void registerJsCall(Send node, ResolverVisitor resolver) {}

  /// Emits a summary information using the [log] function.
  void logSummary(log(message)) {}
}


abstract class NativeEnqueuerBase implements NativeEnqueuer {

  /**
   * The set of all native classes.  Each native class is in [nativeClasses] and
   * exactly one of [unusedClasses], [pendingClasses] and [registeredClasses].
   */
  final Set<ClassElement> nativeClasses = new Set<ClassElement>();

  final Set<ClassElement> registeredClasses = new Set<ClassElement>();
  final Set<ClassElement> pendingClasses = new Set<ClassElement>();
  final Set<ClassElement> unusedClasses = new Set<ClassElement>();

  /**
   * Records matched constraints ([SpecialType] or [DartType]).  Once a type
   * constraint has been matched, there is no need to match it again.
   */
  final Set matchedTypeConstraints = new Set();

  /// Pending actions.  Classes in [pendingClasses] have action thunks in
  /// [queue] to register the class.
  final queue = new Queue();
  bool flushing = false;

  /// Maps JS foreign calls to their computed native behavior.
  final Map<Node, NativeBehavior> nativeBehaviors =
      new Map<Node, NativeBehavior>();

  final Enqueuer world;
  final Compiler compiler;
  final bool enableLiveTypeAnalysis;

  ClassElement _annotationCreatesClass;
  ClassElement _annotationReturnsClass;
  ClassElement _annotationJsNameClass;

  /// Subclasses of [NativeEnqueuerBase] are constructed by the backend.
  NativeEnqueuerBase(this.world, this.compiler, this.enableLiveTypeAnalysis);

  void processNativeClasses(Iterable<LibraryElement> libraries) {
    libraries.forEach(processNativeClassesInLibrary);
    processNativeClassesInLibrary(compiler.isolateHelperLibrary);
    if (!enableLiveTypeAnalysis) {
      nativeClasses.forEach((c) => enqueueClass(c, 'forced'));
      flushQueue();
    }
  }

  void processNativeClassesInLibrary(LibraryElement library) {
    // Use implementation to ensure the inclusion of injected members.
    library.implementation.forEachLocalMember((Element element) {
      if (element.isClass() && element.isNative()) {
        processNativeClass(element);
      }
    });
  }

  void processNativeClass(ClassElement classElement) {
    nativeClasses.add(classElement);
    unusedClasses.add(classElement);
    // Resolve class to ensure the class has valid inheritance info.
    classElement.ensureResolved(compiler);
  }

  ClassElement get annotationCreatesClass {
    findAnnotationClasses();
    return _annotationCreatesClass;
  }

  ClassElement get annotationReturnsClass {
    findAnnotationClasses();
    return _annotationReturnsClass;
  }

  ClassElement get annotationJsNameClass {
    findAnnotationClasses();
    return _annotationJsNameClass;
  }

  void findAnnotationClasses() {
    if (_annotationCreatesClass != null) return;
    ClassElement find(name) {
      Element e = compiler.findHelper(name);
      if (e == null || e is! ClassElement) {
        compiler.cancel("Could not find implementation class '${name}'");
      }
      return e;
    }
    _annotationCreatesClass = find(const SourceString('Creates'));
    _annotationReturnsClass = find(const SourceString('Returns'));
    _annotationJsNameClass = find(const SourceString('JSName'));
  }

  /// Returns the JSName annotation string or `null` if no JSName annotation is
  /// present.
  String findJsNameFromAnnotation(Element element) {
    String name = null;
    ClassElement annotationClass = annotationJsNameClass;
    for (Link<MetadataAnnotation> link = element.metadata;
         !link.isEmpty;
         link = link.tail) {
      MetadataAnnotation annotation = link.head.ensureResolved(compiler);
      var value = annotation.value;
      if (value is! ConstructedConstant) continue;
      if (value.type is! InterfaceType) continue;
      if (!identical(value.type.element, annotationClass)) continue;

      var fields = value.fields;
      // TODO(sra): Better validation of the constant.
      if (fields.length != 1 || fields[0] is! StringConstant) {
        PartialMetadataAnnotation partial = annotation;
        compiler.cancel(
            'Annotations needs one string: ${partial.parseNode(compiler)}');
      }
      String specString = fields[0].toDartString().slowToString();
      if (name == null) {
        name = specString;
      } else {
        PartialMetadataAnnotation partial = annotation;
        compiler.cancel(
            'Too many JSName annotations: ${partial.parseNode(compiler)}');
      }
    }
    return name;
  }

  enqueueClass(ClassElement classElement, cause) {
    assert(unusedClasses.contains(classElement));
    unusedClasses.remove(classElement);
    pendingClasses.add(classElement);
    queue.add(() { processClass(classElement, cause); });
  }

  void flushQueue() {
    if (flushing) return;
    flushing = true;
    while (!queue.isEmpty) {
      (queue.removeFirst())();
    }
    flushing = false;
  }

  processClass(ClassElement classElement, cause) {
    assert(!registeredClasses.contains(classElement));

    bool firstTime = registeredClasses.isEmpty;
    pendingClasses.remove(classElement);
    registeredClasses.add(classElement);

    // TODO(ahe): Is this really a global dependency?
    world.registerInstantiatedClass(classElement, compiler.globalDependencies);

    // Also parse the node to know all its methods because otherwise it will
    // only be parsed if there is a call to one of its constructors.
    classElement.parseNode(compiler);

    if (firstTime) {
      queue.add(onFirstNativeClass);
    }
  }

  registerElement(Element element) {
    compiler.withCurrentElement(element, () {
      if (element.isFunction() || element.isGetter() || element.isSetter()) {
        handleMethodAnnotations(element);
        if (element.isNative()) {
          registerMethodUsed(element);
        }
      } else if (element.isField()) {
        handleFieldAnnotations(element);
        if (element.isNative()) {
          registerFieldLoad(element);
          registerFieldStore(element);
        }
      }
    });
  }

  handleFieldAnnotations(Element element) {
    if (element.enclosingElement.isNative()) {
      // Exclude non-instance (static) fields - they not really native and are
      // compiled as isolate globals.  Access of a property of a constructor
      // function or a non-method property in the prototype chain, must be coded
      // using a JS-call.
      if (element.isInstanceMember()) {
        setNativeName(element);
      }
    }
  }

  handleMethodAnnotations(Element method) {
    if (isNativeMethod(method)) {
      setNativeName(method);
    }
  }

  /// Sets the native name of [element], either from an annotation, or
  /// defaulting to the Dart name.
  void setNativeName(Element element) {
    String name = findJsNameFromAnnotation(element);
    if (name == null) name = element.name.slowToString();
    element.setNative(name);
  }

  bool isNativeMethod(Element element) {
    if (!element.getLibrary().canUseNative) return false;
    // Native method?
    return compiler.withCurrentElement(element, () {
      Node node = element.parseNode(compiler);
      if (node is! FunctionExpression) return false;
      node = node.body;
      Token token = node.getBeginToken();
      if (identical(token.stringValue, 'native')) return true;
      return false;
    });
  }

  void registerMethodUsed(Element method) {
    processNativeBehavior(
        NativeBehavior.ofMethod(method, compiler),
        method);
      flushQueue();
  }

  void registerFieldLoad(Element field) {
    processNativeBehavior(
        NativeBehavior.ofFieldLoad(field, compiler),
        field);
    flushQueue();
  }

  void registerFieldStore(Element field) {
    processNativeBehavior(
        NativeBehavior.ofFieldStore(field, compiler),
        field);
    flushQueue();
  }

  void registerJsCall(Send node, ResolverVisitor resolver) {
    NativeBehavior behavior = NativeBehavior.ofJsCall(node, compiler, resolver);
    processNativeBehavior(behavior, node);
    nativeBehaviors[node] = behavior;
    flushQueue();
  }

  NativeBehavior getNativeBehaviorOf(Send node) => nativeBehaviors[node];

  processNativeBehavior(NativeBehavior behavior, cause) {
    // TODO(ahe): Is this really a global dependency?
    TreeElements elements = compiler.globalDependencies;
    bool allUsedBefore = unusedClasses.isEmpty;
    for (var type in behavior.typesInstantiated) {
      if (matchedTypeConstraints.contains(type)) continue;
      matchedTypeConstraints.add(type);
      if (type is SpecialType) {
        if (type == SpecialType.JsArray) {
          world.registerInstantiatedClass(compiler.listClass, elements);
        } else if (type == SpecialType.JsObject) {
          world.registerInstantiatedClass(compiler.objectClass, elements);
        }
        continue;
      }
      if (type is InterfaceType) {
        if (type.element == compiler.intClass) {
          world.registerInstantiatedClass(compiler.intClass, elements);
        } else if (type.element == compiler.doubleClass) {
          world.registerInstantiatedClass(compiler.doubleClass, elements);
        } else if (type.element == compiler.numClass) {
          world.registerInstantiatedClass(compiler.doubleClass, elements);
          world.registerInstantiatedClass(compiler.intClass, elements);
        } else if (type.element == compiler.stringClass) {
          world.registerInstantiatedClass(compiler.stringClass, elements);
        } else if (type.element == compiler.nullClass) {
          world.registerInstantiatedClass(compiler.nullClass, elements);
        } else if (type.element == compiler.boolClass) {
          world.registerInstantiatedClass(compiler.boolClass, elements);
        }
      }
      assert(type is DartType);
      enqueueUnusedClassesMatching(
          (nativeClass) => compiler.types.isSubtype(nativeClass.thisType, type),
          cause,
          'subtypeof($type)');
    }

    // Give an info so that library developers can compile with -v to find why
    // all the native classes are included.
    if (unusedClasses.isEmpty && !allUsedBefore) {
      compiler.log('All native types marked as used due to $cause.');
    }
  }

  enqueueUnusedClassesMatching(bool predicate(classElement),
                               cause,
                               [String reason]) {
    Iterable matches = unusedClasses.where(predicate);
    matches.toList().forEach((c) => enqueueClass(c, cause));
  }

  onFirstNativeClass() {
    staticUse(name) => world.registerStaticUse(compiler.findHelper(name));

    staticUse(const SourceString('dynamicFunction'));
    staticUse(const SourceString('dynamicSetMetadata'));
    staticUse(const SourceString('defineProperty'));
    staticUse(const SourceString('toStringForNativeObject'));
    staticUse(const SourceString('hashCodeForNativeObject'));
    staticUse(const SourceString('convertDartClosureToJS'));
    staticUse(const SourceString('defineNativeMethods'));
    staticUse(const SourceString('defineNativeMethodsNonleaf'));
    // TODO(9577): Registering `defineNativeMethodsFinish` seems redundant with
    // respect to the registering in the backend. When the backend registering
    // is removed as part of fixing Issue 9577, this can be the sole place
    // registering this methods.
    staticUse(const SourceString('defineNativeMethodsFinish'));

    addNativeExceptions();
  }

  addNativeExceptions() {
    enqueueUnusedClassesMatching((classElement) {
        // TODO(sra): Annotate exception classes in dart:html.
        String name = classElement.name.slowToString();
        if (name.contains('Exception')) return true;
        if (name.contains('Error')) return true;
        return false;
      },
      'native exception');
  }
}


class NativeResolutionEnqueuer extends NativeEnqueuerBase {

  NativeResolutionEnqueuer(Enqueuer world, Compiler compiler)
    : super(world, compiler, compiler.enableNativeLiveTypeAnalysis);

  void logSummary(log(message)) {
    log('Resolved ${registeredClasses.length} native elements used, '
        '${unusedClasses.length} native elements dead.');
  }
}


class NativeCodegenEnqueuer extends NativeEnqueuerBase {

  final CodeEmitterTask emitter;

  final Set<ClassElement> doneAddSubtypes = new Set<ClassElement>();

  NativeCodegenEnqueuer(Enqueuer world, Compiler compiler, this.emitter)
    : super(world, compiler, compiler.enableNativeLiveTypeAnalysis);

  void processNativeClasses(Iterable<LibraryElement> libraries) {
    super.processNativeClasses(libraries);

    // HACK HACK - add all the resolved classes.
    NativeEnqueuerBase enqueuer = compiler.enqueuer.resolution.nativeEnqueuer;
    for (final classElement in enqueuer.registeredClasses) {
      if (unusedClasses.contains(classElement)) {
        enqueueClass(classElement, 'was resolved');
      }
    }
    flushQueue();
  }

  processClass(ClassElement classElement, cause) {
    super.processClass(classElement, cause);
    // Add the information that this class is a subtype of its supertypes.  The
    // code emitter and the ssa builder use that information.
    addSubtypes(classElement, emitter.nativeEmitter);
  }

  void addSubtypes(ClassElement cls, NativeEmitter emitter) {
    if (!cls.isNative()) return;
    if (doneAddSubtypes.contains(cls)) return;
    doneAddSubtypes.add(cls);

    // Walk the superclass chain since classes on the superclass chain might not
    // be instantiated (abstract or simply unused).
    addSubtypes(cls.superclass, emitter);

    for (DartType type in cls.allSupertypes) {
      List<Element> subtypes = emitter.subtypes.putIfAbsent(
          type.element,
          () => <ClassElement>[]);
      subtypes.add(cls);
    }

    // Skip through all the mixin applications in the super class
    // chain. That way, the direct subtypes set only contain the
    // natives classes.
    ClassElement superclass = cls.superclass;
    while (superclass != null && superclass.isMixinApplication) {
      assert(!superclass.isNative());
      superclass = superclass.superclass;
    }

    List<Element> directSubtypes = emitter.directSubtypes.putIfAbsent(
        superclass,
        () => <ClassElement>[]);
    directSubtypes.add(cls);
  }

  void logSummary(log(message)) {
    log('Compiled ${registeredClasses.length} native classes, '
        '${unusedClasses.length} native classes omitted.');
  }
}

void maybeEnableNative(Compiler compiler,
                       LibraryElement library) {
  String libraryName = library.canonicalUri.toString();
  if (library.entryCompilationUnit.script.name.contains(
          'dart/tests/compiler/dart2js_native')
      || libraryName == 'dart:async'
      || libraryName == 'dart:html'
      || libraryName == 'dart:html_common'
      || libraryName == 'dart:indexed_db'
      || libraryName == 'dart:svg'
      || libraryName == 'dart:web_audio'
      || libraryName == 'dart:web_gl'
      || libraryName == 'dart:web_sql') {
    library.canUseNative = true;
  }
}

/**
 * A summary of the behavior of a native element.
 *
 * Native code can return values of one type and cause native subtypes of
 * another type to be instantiated.  By default, we compute both from the
 * declared type.
 *
 * A field might yield any native type that 'is' the field type.
 *
 * A method might create and return instances of native subclasses of its
 * declared return type, and a callback argument may be called with instances of
 * the callback parameter type (e.g. Event).
 *
 * If there is one or more @Creates annotations, the union of the named types
 * replaces the inferred instantiated type, and the return type is ignored for
 * the purpose of inferring instantiated types.
 *
 *     @Creates(IDBCursor)    // Created asynchronously.
 *     @Creates(IDBRequest)   // Created synchronously (for return value).
 *     IDBRequest request = objectStore.openCursor();
 *
 * If there is one or more @Returns annotations, the union of the named types
 * replaces the declared return type.
 *
 *     @Returns(IDBRequest)
 *     IDBRequest request = objectStore.openCursor();
 */
class NativeBehavior {

  /// [DartType]s or [SpecialType]s returned or yielded by the native element.
  final List typesReturned = [];

  /// [DartType]s or [SpecialType]s instantiated by the native element.
  final List typesInstantiated = [];

  static final NativeBehavior NONE = new NativeBehavior();

  //NativeBehavior();

  static NativeBehavior ofJsCall(Send jsCall, Compiler compiler, resolver) {
    // The first argument of a JS-call is a string encoding various attributes
    // of the code.
    //
    //  'Type1|Type2'.  A union type.
    //  '=Object'.      A JavaScript Object, no subtype.
    //  '=List'.        A JavaScript Array, no subtype.

    var argNodes = jsCall.arguments;
    if (argNodes.isEmpty) {
      compiler.cancel("JS expression has no type", node: jsCall);
    }

    var firstArg = argNodes.head;
    LiteralString specLiteral = firstArg.asLiteralString();
    if (specLiteral != null) {
      String specString = specLiteral.dartString.slowToString();
      // Various things that are not in fact types.
      if (specString == 'void') return NativeBehavior.NONE;
      if (specString == '' || specString == 'var') {
        var behavior = new NativeBehavior();
        behavior.typesReturned.add(compiler.objectClass.computeType(compiler));
        behavior.typesReturned.add(compiler.nullClass.computeType(compiler));
        return behavior;
      }
      var behavior = new NativeBehavior();
      for (final typeString in specString.split('|')) {
        var type = _parseType(typeString, compiler,
            (name) => resolver.resolveTypeFromString(name),
            jsCall);
        behavior.typesInstantiated.add(type);
        behavior.typesReturned.add(type);
      }
      return behavior;
    }

    // TODO(sra): We could accept a type identifier? e.g. JS(bool, '1<2').  It
    // is not very satisfactory because it does not work for void, dynamic.

    compiler.cancel("Unexpected JS first argument", node: firstArg);
  }

  static NativeBehavior ofMethod(FunctionElement method, Compiler compiler) {
    FunctionType type = method.computeType(compiler);
    var behavior = new NativeBehavior();
    behavior.typesReturned.add(type.returnType);
    behavior._capture(type, compiler);

    // TODO(sra): Optional arguments are currently missing from the
    // DartType. This should be fixed so the following work-around can be
    // removed.
    method.computeSignature(compiler).forEachOptionalParameter(
        (Element parameter) {
          behavior._escape(parameter.computeType(compiler), compiler);
        });

    behavior._overrideWithAnnotations(method, compiler);
    return behavior;
  }

  static NativeBehavior ofFieldLoad(Element field, Compiler compiler) {
    DartType type = field.computeType(compiler);
    var behavior = new NativeBehavior();
    behavior.typesReturned.add(type);
    behavior._capture(type, compiler);
    behavior._overrideWithAnnotations(field, compiler);
    return behavior;
  }

  static NativeBehavior ofFieldStore(Element field, Compiler compiler) {
    DartType type = field.computeType(compiler);
    var behavior = new NativeBehavior();
    behavior._escape(type, compiler);
    // We don't override the default behaviour - the annotations apply to
    // loading the field.
    return behavior;
  }

  void _overrideWithAnnotations(Element element, Compiler compiler) {
    if (element.metadata.isEmpty) return;

    DartType lookup(String name) {
      Element e = element.buildScope().lookup(new SourceString(name));
      if (e == null) return null;
      if (e is! ClassElement) return null;
      e.ensureResolved(compiler);
      return e.computeType(compiler);
    }

    NativeEnqueuerBase enqueuer = compiler.enqueuer.resolution.nativeEnqueuer;
    var creates = _collect(element, compiler, enqueuer.annotationCreatesClass,
                           lookup);
    var returns = _collect(element, compiler, enqueuer.annotationReturnsClass,
                           lookup);

    if (creates != null) {
      typesInstantiated..clear()..addAll(creates);
    }
    if (returns != null) {
      typesReturned..clear()..addAll(returns);
    }
  }

  /**
   * Returns a list of type constraints from the annotations of
   * [annotationClass].
   * Returns `null` if no constraints.
   */
  static _collect(Element element, Compiler compiler, Element annotationClass,
                  lookup(str)) {
    var types = null;
    for (Link<MetadataAnnotation> link = element.metadata;
         !link.isEmpty;
         link = link.tail) {
      MetadataAnnotation annotation = link.head.ensureResolved(compiler);
      var value = annotation.value;
      if (value is! ConstructedConstant) continue;
      if (value.type is! InterfaceType) continue;
      if (!identical(value.type.element, annotationClass)) continue;

      var fields = value.fields;
      // TODO(sra): Better validation of the constant.
      if (fields.length != 1 || fields[0] is! StringConstant) {
        PartialMetadataAnnotation partial = annotation;
        compiler.cancel(
            'Annotations needs one string: ${partial.parseNode(compiler)}');
      }
      String specString = fields[0].toDartString().slowToString();
      for (final typeString in specString.split('|')) {
        var type = _parseType(typeString, compiler, lookup, annotation);
        if (types == null) types = [];
        types.add(type);
      }
    }
    return types;
  }

  /// Models the behavior of having intances of [type] escape from Dart code
  /// into native code.
  void _escape(DartType type, Compiler compiler) {
    type = type.unalias(compiler);
    if (type is FunctionType) {
      // A function might be called from native code, passing us novel
      // parameters.
      _escape(type.returnType, compiler);
      for (Link<DartType> parameters = type.parameterTypes;
           !parameters.isEmpty;
           parameters = parameters.tail) {
        _capture(parameters.head, compiler);
      }
    }
  }

  /// Models the behavior of Dart code receiving instances and methods of [type]
  /// from native code.  We usually start the analysis by capturing a native
  /// method that has been used.
  void _capture(DartType type, Compiler compiler) {
    type = type.unalias(compiler);
    if (type is FunctionType) {
      _capture(type.returnType, compiler);
      for (Link<DartType> parameters = type.parameterTypes;
           !parameters.isEmpty;
           parameters = parameters.tail) {
        _escape(parameters.head, compiler);
      }
    } else {
      typesInstantiated.add(type);
    }
  }

  static _parseType(String typeString, Compiler compiler,
      lookup(name), locationNodeOrElement) {
    if (typeString == '=Object') return SpecialType.JsObject;
    if (typeString == '=List') return SpecialType.JsArray;
    if (typeString == 'dynamic') {
      return  compiler.dynamicClass.computeType(compiler);
    }
    DartType type = lookup(typeString);
    if (type != null) return type;

    int index = typeString.indexOf('<');
    if (index < 1) {
      compiler.cancel("Type '$typeString' not found",
          node: _errorNode(locationNodeOrElement, compiler));
    }
    type = lookup(typeString.substring(0, index));
    if (type != null)  {
      // TODO(sra): Parse type parameters.
      return type;
    }
    compiler.cancel("Type '$typeString' not found",
        node: _errorNode(locationNodeOrElement, compiler));
  }

  static _errorNode(locationNodeOrElement, compiler) {
    if (locationNodeOrElement is Node) return locationNodeOrElement;
    return locationNodeOrElement.parseNode(compiler);
  }
}

void checkAllowedLibrary(ElementListener listener, Token token) {
  LibraryElement currentLibrary = listener.compilationUnitElement.getLibrary();
  if (!currentLibrary.canUseNative) {
    listener.recoverableError("Unexpected token", token: token);
  }
}

Token handleNativeBlockToSkip(Listener listener, Token token) {
  checkAllowedLibrary(listener, token);
  token = token.next;
  if (identical(token.kind, STRING_TOKEN)) {
    token = token.next;
  }
  if (identical(token.stringValue, '{')) {
    BeginGroupToken beginGroupToken = token;
    token = beginGroupToken.endGroup;
  }
  return token;
}

Token handleNativeClassBodyToSkip(Listener listener, Token token) {
  checkAllowedLibrary(listener, token);
  listener.handleIdentifier(token);
  token = token.next;
  if (!identical(token.kind, STRING_TOKEN)) {
    return listener.unexpected(token);
  }
  token = token.next;
  if (!identical(token.stringValue, '{')) {
    return listener.unexpected(token);
  }
  BeginGroupToken beginGroupToken = token;
  token = beginGroupToken.endGroup;
  return token;
}

Token handleNativeClassBody(Listener listener, Token token) {
  checkAllowedLibrary(listener, token);
  token = token.next;
  if (!identical(token.kind, STRING_TOKEN)) {
    listener.unexpected(token);
  } else {
    token = token.next;
  }
  return token;
}

Token handleNativeFunctionBody(ElementListener listener, Token token) {
  checkAllowedLibrary(listener, token);
  Token begin = token;
  listener.beginReturnStatement(token);
  token = token.next;
  bool hasExpression = false;
  if (identical(token.kind, STRING_TOKEN)) {
    hasExpression = true;
    listener.beginLiteralString(token);
    listener.endLiteralString(0);
    token = token.next;
  }
  listener.endReturnStatement(hasExpression, begin, token);
  // TODO(ngeoffray): expect a ';'.
  // Currently there are method with both native marker and Dart body.
  return token.next;
}

SourceString checkForNativeClass(ElementListener listener) {
  SourceString nativeTagInfo;
  Node node = listener.nodes.head;
  if (node != null
      && node.asIdentifier() != null
      && node.asIdentifier().source.stringValue == 'native') {
    nativeTagInfo = node.asIdentifier().token.next.value;
    listener.popNode();
  }
  return nativeTagInfo;
}

bool isOverriddenMethod(FunctionElement element,
                        ClassElement cls,
                        NativeEmitter nativeEmitter) {
  List<ClassElement> subtypes = nativeEmitter.subtypes[cls];
  if (subtypes == null) return false;
  for (ClassElement subtype in subtypes) {
    if (subtype.lookupLocalMember(element.name) != null) return true;
  }
  return false;
}

final RegExp nativeRedirectionRegExp = new RegExp(r'^[a-zA-Z][a-zA-Z_$0-9]*$');

void handleSsaNative(SsaBuilder builder, Expression nativeBody) {
  Compiler compiler = builder.compiler;
  FunctionElement element = builder.work.element;
  NativeEmitter nativeEmitter = builder.emitter.nativeEmitter;

  HInstruction convertDartClosure(Element parameter, FunctionType type) {
    HInstruction local = builder.localsHandler.readLocal(parameter);
    Constant arityConstant =
        builder.constantSystem.createInt(type.computeArity());
    HInstruction arity = builder.graph.addConstant(arityConstant);
    // TODO(ngeoffray): For static methods, we could pass a method with a
    // defined arity.
    Element helper = builder.backend.getClosureConverter();
    builder.pushInvokeHelper2(helper, local, arity, HType.UNKNOWN);
    HInstruction closure = builder.pop();
    return closure;
  }

  // Check which pattern this native method follows:
  // 1) foo() native;
  //      hasBody = false
  // 2) foo() native "bar";
  //      No longer supported, this is now done with @JSName('foo') and case 1.
  // 3) foo() native "return 42";
  //      hasBody = true
  bool hasBody = false;
  assert(element.isNative());
  String nativeMethodName = element.fixedBackendName();
  if (nativeBody != null) {
    LiteralString jsCode = nativeBody.asLiteralString();
    String str = jsCode.dartString.slowToString();
    if (nativeRedirectionRegExp.hasMatch(str)) {
      compiler.cancel("Deprecated syntax, use @JSName('name') instead.",
                      node: nativeBody);
    }
    hasBody = true;
  }

  if (!hasBody) {
    nativeEmitter.nativeMethods.add(element);
  }

  FunctionSignature parameters = element.computeSignature(builder.compiler);
  if (!hasBody) {
    List<String> arguments = <String>[];
    List<HInstruction> inputs = <HInstruction>[];
    String receiver = '';
    if (element.isInstanceMember()) {
      receiver = '#.';
      inputs.add(builder.localsHandler.readThis());
    }
    parameters.forEachParameter((Element parameter) {
      DartType type = parameter.computeType(compiler).unalias(compiler);
      HInstruction input = builder.localsHandler.readLocal(parameter);
      if (type is FunctionType) {
        // The parameter type is a function type either directly or through
        // typedef(s).
        input = convertDartClosure(parameter, type);
      }
      inputs.add(input);
      arguments.add('#');
    });

    String foreignParameters = arguments.join(',');
    String nativeMethodCall;
    if (element.kind == ElementKind.FUNCTION) {
      nativeMethodCall = '$receiver$nativeMethodName($foreignParameters)';
    } else if (element.kind == ElementKind.GETTER) {
      nativeMethodCall = '$receiver$nativeMethodName';
    } else if (element.kind == ElementKind.SETTER) {
      nativeMethodCall = '$receiver$nativeMethodName = $foreignParameters';
    } else {
      builder.compiler.internalError('unexpected kind: "${element.kind}"',
                                     element: element);
    }

    DartString jsCode = new DartString.literal(nativeMethodCall);
    builder.push(new HForeign(jsCode, HType.UNKNOWN, inputs));
    builder.close(new HReturn(builder.pop())).addSuccessor(builder.graph.exit);
  } else {
    if (parameters.parameterCount != 0) {
      compiler.cancel(
          'native "..." syntax is restricted to functions with zero parameters',
          node: nativeBody);
    }
    LiteralString jsCode = nativeBody.asLiteralString();
    builder.push(new HForeign.statement(jsCode.dartString, <HInstruction>[]));
  }
}
