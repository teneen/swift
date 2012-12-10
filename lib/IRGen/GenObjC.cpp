//===--- GenObjC.cpp - Objective-C interaction ----------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements bridging to Objective-C.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SmallString.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"

#include "swift/IRGen/Options.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "clang/AST/Attr.h"
#include "clang/AST/DeclObjC.h"

#include "ASTVisitor.h"
#include "CallEmission.h"
#include "Cleanup.h"
#include "Explosion.h"
#include "FixedTypeInfo.h"
#include "GenMeta.h"
#include "GenType.h"
#include "HeapTypeInfo.h"
#include "IRGenFunction.h"
#include "IRGenModule.h"
#include "ScalarTypeInfo.h"

#include "GenObjC.h"

using namespace swift;
using namespace irgen;

/// Create an Objective-C runtime function.  The ObjC runtime uses the
/// standard C conventions.
static llvm::Constant *createObjCRuntimeFunction(IRGenModule &IGM, StringRef name,
                                                 llvm::FunctionType *fnType) {
  llvm::Constant *addr = IGM.Module.getOrInsertFunction(name, fnType);
  return addr;
}


llvm::Constant *IRGenModule::getObjCMsgSendFn() {
  if (ObjCMsgSendFn) return ObjCMsgSendFn;

  // T objc_msgSend(id, SEL*, U...);
  // We use a totally bogus signature to make sure we *always* cast.
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(VoidTy, ArrayRef<llvm::Type*>(), false);
  ObjCMsgSendFn = createObjCRuntimeFunction(*this, "objc_msgSend", fnType);
  return ObjCMsgSendFn;
}

llvm::Constant *IRGenModule::getObjCMsgSendStretFn() {
  if (ObjCMsgSendStretFn) return ObjCMsgSendStretFn;

  // T objc_msgSend_stret(id, SEL*, U...);
  // We use a totally bogus signature to make sure we *always* cast.
  llvm::FunctionType *fnType =
    llvm::FunctionType::get(VoidTy, ArrayRef<llvm::Type*>(), false);
  ObjCMsgSendStretFn =
    createObjCRuntimeFunction(*this, "objc_msgSend_stret", fnType);
  return ObjCMsgSendStretFn;
}

llvm::Constant *IRGenModule::getObjCSelRegisterNameFn() {
  if (ObjCSelRegisterNameFn) return ObjCSelRegisterNameFn;

  // SEL sel_registerName(const char *str);
  llvm::Type *argTypes[1] = { Int8PtrTy };
  auto fnType = llvm::FunctionType::get(ObjCSELTy, argTypes, false);
  ObjCSelRegisterNameFn = createObjCRuntimeFunction(*this, "sel_registerName",
                                                    fnType);
  return ObjCSelRegisterNameFn;
}

#define DEFINE_OBJC_RUNTIME_FUNCTION(LABEL, NAME, RETTY)            \
llvm::Constant *IRGenModule::getObjC##LABEL##Fn() {                 \
  if (ObjC##LABEL##Fn) return ObjC##LABEL##Fn;                      \
                                                                    \
  llvm::FunctionType *fnType =                                      \
    llvm::FunctionType::get(RETTY, ObjCPtrTy, false);               \
  ObjC##LABEL##Fn = createObjCRuntimeFunction(*this, NAME, fnType); \
  return ObjC##LABEL##Fn;                                           \
}

DEFINE_OBJC_RUNTIME_FUNCTION(Retain,
                             "objc_retain",
                             ObjCPtrTy)
DEFINE_OBJC_RUNTIME_FUNCTION(RetainAutoreleasedReturnValue,
                             "objc_retainAutoreleasedReturnValue",
                             ObjCPtrTy)
DEFINE_OBJC_RUNTIME_FUNCTION(Release,
                             "objc_release",
                             VoidTy)

void IRGenFunction::emitObjCRelease(llvm::Value *value) {
  // Get an appropriately-casted function pointer.
  auto fn = IGM.getObjCReleaseFn();
  if (value->getType() != IGM.ObjCPtrTy) {
    auto fnTy = llvm::FunctionType::get(IGM.VoidTy, value->getType(),
                                        false)->getPointerTo();
    fn = llvm::ConstantExpr::getBitCast(fn, fnTy);
  }

  auto call = Builder.CreateCall(fn, value);
  call->setDoesNotThrow();
}

/// Given a function of type %objc* (%objc*)*, cast it as appropriate
/// to be used with values of type T.
static llvm::Constant *getCastOfRetainFn(IRGenModule &IGM,
                                         llvm::Constant *fn,
                                         llvm::Type *valueTy) {
#ifndef NDEBUG
  auto origFnTy = cast<llvm::FunctionType>(fn->getType()->getPointerElementType());
  assert(origFnTy->getReturnType() == IGM.ObjCPtrTy);
  assert(origFnTy->getNumParams() == 1);
  assert(origFnTy->getParamType(0) == IGM.ObjCPtrTy);
  assert(isa<llvm::PointerType>(valueTy));
#endif
  if (valueTy == IGM.ObjCPtrTy)
    return fn;

  auto fnTy = llvm::FunctionType::get(valueTy, valueTy, false);
  return llvm::ConstantExpr::getBitCast(fn, fnTy->getPointerTo(0));
}

llvm::Value *IRGenFunction::emitObjCRetainCall(llvm::Value *value) {
  // Get an appropriately-casted function pointer.
  auto fn = IGM.getObjCRetainFn();
  fn = getCastOfRetainFn(IGM, fn, value->getType());

  auto call = Builder.CreateCall(fn, value);
  call->setDoesNotThrow();
  return call;
}

/// Reclaim an autoreleased return value.
llvm::Value *irgen::emitObjCRetainAutoreleasedReturnValue(IRGenFunction &IGF,
                                                          llvm::Value *value) {
  auto fn = IGF.IGM.getObjCRetainAutoreleasedReturnValueFn();
  fn = getCastOfRetainFn(IGF.IGM, fn, value->getType());

  auto call = IGF.Builder.CreateCall(fn, value);
  call->setDoesNotThrow();
  return call;
}

namespace {
  struct CallObjCRelease : Cleanup {
    llvm::Value *Value;
    CallObjCRelease(llvm::Value *value) : Value(value) {}

    void emit(IRGenFunction &IGF) const {
      IGF.emitObjCRelease(Value);
    }
  };
}

ManagedValue IRGenFunction::enterObjCReleaseCleanup(llvm::Value *value) {
  pushFullExprCleanup<CallObjCRelease>(value);
  return ManagedValue(value, getCleanupsDepth());
}

namespace {
  /// A type-info implementation suitable for an ObjC pointer type.
  class ObjCTypeInfo : public HeapTypeInfo<ObjCTypeInfo> {
  public:
    ObjCTypeInfo(llvm::PointerType *storageType, Size size, Alignment align)
      : HeapTypeInfo(storageType, size, align) {
    }

    /// Builtin.ObjCPointer requires ObjC reference-counting.
    bool hasSwiftRefcount() const { return false; }
  };
}

const TypeInfo *TypeConverter::convertBuiltinObjCPointer() {
  return new ObjCTypeInfo(IGM.ObjCPtrTy, IGM.getPointerSize(),
                          IGM.getPointerAlignment());
}

/// Get or create a global Objective-C method name.  Always returns an i8*.
llvm::Constant *IRGenModule::getAddrOfObjCMethodName(StringRef selector) {
  // Check whether this selector already exists.
  auto &entry = ObjCMethodNames[selector];
  if (entry) return entry;

  // If not, create it.  This implicitly adds a trailing null.
  auto init = llvm::ConstantDataArray::getString(LLVMContext, selector);
  auto global = new llvm::GlobalVariable(Module, init->getType(), true,
                                         llvm::GlobalValue::InternalLinkage,
                                         init, "\01L_OBJC_METH_VAR_NAME_");
  global->setSection("__TEXT,__objc_methname,cstring_literals");
  global->setAlignment(1);

  // Drill down to make an i8*.
  auto zero = llvm::ConstantInt::get(SizeTy, 0);
  llvm::Constant *indices[] = { zero, zero };
  auto address = llvm::ConstantExpr::getInBoundsGetElementPtr(global, indices);

  // Cache and return.
  entry = address;
  return address;
}

/// Get or create an Objective-C selector reference.  Always returns
/// an i8**.  The design is that the compiler will emit a load of this
/// pointer, and the linker will ensure that that pointer is unique.
llvm::Constant *IRGenModule::getAddrOfObjCSelectorRef(StringRef selector) {
  // Check whether a reference for this selector already exists.
  auto &entry = ObjCSelectorRefs[selector];
  if (entry) return entry;

  // If not, create it.  The initializer is just a pointer to the
  // method name.  Note that the label here is unimportant, so we
  // choose something descriptive to make the IR readable.
  auto init = getAddrOfObjCMethodName(selector);
  auto global = new llvm::GlobalVariable(Module, init->getType(), false,
                                         llvm::GlobalValue::InternalLinkage,
                                         init,
                                     llvm::Twine("selector(") + selector + ")");
  global->setAlignment(getPointerAlignment().getValue());

  // This section name is magical for the Darwin static and dynamic linkers.
  global->setSection("__DATA,__objc_selrefs,literal_pointers,no_dead_strip");

  // Make sure that this reference does not get optimized away.
  addUsedGlobal(global);

  // Cache and return.
  entry = global;
  return global;
}

void IRGenModule::addUsedGlobal(llvm::GlobalValue *global) {
  assert(!global->isDeclaration() &&
         "Only globals with definition can force usage.");
  LLVMUsed.push_back(global);
}

void IRGenModule::emitLLVMUsed() {
  // Don't create llvm.used if there is no need.
  if (LLVMUsed.empty())
    return;

  // Convert LLVMUsed to what ConstantArray needs.
  SmallVector<llvm::Constant*, 8> UsedArray;
  UsedArray.resize(LLVMUsed.size());
  for (unsigned i = 0, e = LLVMUsed.size(); i != e; ++i) {
    UsedArray[i] =
    llvm::ConstantExpr::getBitCast(cast<llvm::Constant>(&*LLVMUsed[i]),
                                   Int8PtrTy);
  }

  if (UsedArray.empty())
    return;
  llvm::ArrayType *ATy = llvm::ArrayType::get(Int8PtrTy, UsedArray.size());

  llvm::GlobalVariable *GV =
  new llvm::GlobalVariable(Module, ATy, false,
                           llvm::GlobalValue::AppendingLinkage,
                           llvm::ConstantArray::get(ATy, UsedArray),
                           "llvm.used");

  GV->setSection("llvm.metadata");
}

/// Determine the natural limits on how we can call the given method
/// using Objective-C method dispatch.
AbstractCallee irgen::getAbstractObjCMethodCallee(IRGenFunction &IGF,
                                                  FuncDecl *fn) {
  return AbstractCallee(AbstractCC::C, ExplosionKind::Minimal,
                        /*minUncurry*/ 1, /*maxUncurry*/ 1,
                        ExtraData::None);
}

/// Does an Objective-C method returning the given type require an
/// indirect result?
static llvm::PointerType *requiresObjCIndirectResult(IRGenModule &IGM,
                                                     CanType type) {
  // FIXME: we need to consider the target's C calling convention.
  return IGM.requiresIndirectResult(type, ExplosionKind::Minimal);
}

namespace {
  struct ObjCMethodSignature {
    bool IsIndirectReturn;
    llvm::FunctionType *FnTy;

    ObjCMethodSignature(IRGenModule &IGM, CanType formalType) {
      auto selfFnType = cast<FunctionType>(formalType);
      auto formalFnType = cast<FunctionType>(CanType(selfFnType->getResult()));

      llvm::Type *resultTy;
      SmallVector<llvm::Type*, 8> argTys;

      // Consider the result type first.
      auto resultType = CanType(formalFnType->getResult());
      if (auto ptrTy = requiresObjCIndirectResult(IGM, resultType)) {
        IsIndirectReturn = true;
        resultTy = IGM.VoidTy;
        argTys.push_back(ptrTy);
      } else {
        IsIndirectReturn = false;

        auto resultSchema = IGM.getSchema(resultType, ExplosionKind::Minimal);
        assert(!resultSchema.containsAggregate());
        resultTy = resultSchema.getScalarResultType(IGM);
      }

      // Add the 'self' argument.
      argTys.push_back(IGM.getFragileType(CanType(selfFnType->getInput())));

      // Add the _cmd argument.
      argTys.push_back(IGM.ObjCSELTy);

      // Add the formal arguments.
      auto argSchema = IGM.getSchema(CanType(formalFnType->getInput()),
                                     ExplosionKind::Minimal);
      argSchema.addToArgTypes(IGM, argTys);

      FnTy = llvm::FunctionType::get(resultTy, argTys, /*variadic*/ false);
    }
  };

  class Selector {
    llvm::SmallString<80> Text;

  public:

#define FOREACH_FAMILY(FAMILY)         \
    FAMILY(Alloc, "alloc")             \
    FAMILY(Copy, "copy")               \
    FAMILY(Init, "init")               \
    FAMILY(MutableCopy, "mutableCopy") \
    FAMILY(New, "new")

    // Note that these are in parallel with 'prefixes', below.
    enum class Family {
      None,
#define GET_LABEL(LABEL, PREFIX) LABEL,
      FOREACH_FAMILY(GET_LABEL)
#undef GET_LABEL
    };

    Selector(FuncDecl *method) {
      method->getObjCSelector(Text);
    }

    StringRef str() const {
      return Text;
    }

    /// Return the family string of this selector.
    Family getFamily() const {
      StringRef text = str();
      while (!text.empty() && text[0] == '_') text = text.substr(1);

#define CHECK_PREFIX(LABEL, PREFIX) \
      if (hasPrefix(text, PREFIX)) return Family::LABEL;
      FOREACH_FAMILY(CHECK_PREFIX)
#undef CHECK_PREFIX

      return Family::None;
    }

  private:
    /// Does the given selector start with the given string as a
    /// prefix, in the sense of the selector naming conventions?
    static bool hasPrefix(StringRef text, StringRef prefix) {
      if (!text.startswith(prefix)) return false;
      if (text.size() == prefix.size()) return true;
      assert(text.size() > prefix.size());
      return !islower(text[prefix.size()]);
    }

#undef FOREACH_FAMILY
  };

  /// A CRTP class for emitting an expression as an ObjC class
  /// reference.
  class ObjCClassEmitter
      : public irgen::ExprVisitor<ObjCClassEmitter, llvm::Value*> {
    IRGenFunction &IGF;
    typedef irgen::ExprVisitor<ObjCClassEmitter,llvm::Value*> super;
  public:
    ObjCClassEmitter(IRGenFunction &IGF) : IGF(IGF) {}

    llvm::Value *visit(Expr *E) {
      assert(E->getType()->is<MetaTypeType>());
      assert(E->getType()->castTo<MetaTypeType>()->getInstanceType()
                         ->getClassOrBoundGenericClass() != nullptr);
      return super::visit(E);
    }

    /// Look through metatype conversions.
    llvm::Value *visitMetatypeConversionExpr(MetatypeConversionExpr *E) {
      return visit(E->getSubExpr());
    }

    llvm::Value *emitGetMetatype(Expr *base) {
      Explosion temp(ExplosionKind::Maximal);
      IGF.emitRValue(base, temp);
      auto value = temp.claimNext().getValue(); // let the cleanup happen
      auto baseType = base->getType()->getCanonicalType();
      return emitHeapMetadataRefForHeapObject(IGF, value, baseType);
    }

    /// Special-case the .metatype implicit conversion.
    llvm::Value *visitGetMetatypeExpr(GetMetatypeExpr *E) {
      return emitGetMetatype(E->getSubExpr());
    }

    /// Special-case explicit .metatype expressions.
    llvm::Value *visitMetatypeExpr(MetatypeExpr *E) {
      // If there's a base, we need to evaluate it and then grab the
      // ObjC class for that.
      if (Expr *base = E->getBase()) {
        return emitGetMetatype(base);
      }

      // Otherwise, we need to emit a class reference.
      return emitClassHeapMetadataRef(IGF, getInstanceType(E));
    }

    /// Special-case direct type references.
    llvm::Value *visitDeclRefExpr(DeclRefExpr *E) {
      assert(isa<TypeDecl>(E->getDecl()));
      auto typeDecl = cast<TypeDecl>(E->getDecl());
      auto type = typeDecl->getDeclaredType()->getCanonicalType();
      return emitClassHeapMetadataRef(IGF, type);
    }

    /// In the fallback case, emit as a swift metatype and remap it to
    /// an ObjC class type.
    llvm::Value *visitExpr(Expr *E) {
      Explosion temp(ExplosionKind::Maximal);
      IGF.emitRValue(E, temp);
      assert(temp.size() == 1);
      llvm::Value *metatype = temp.claimUnmanagedNext();
      return emitClassHeapMetadataRefForMetatype(IGF, metatype,
                                                 getInstanceType(E));
    }

    /// Given an expression of metatype type, return the instance
    /// type of the metatype.
    static CanType getInstanceType(Expr *E) {
      auto type = E->getType()->getCanonicalType();
      return CanType(cast<MetaTypeType>(type)->getInstanceType());
    }
  };

  /// Ownership conventions derived from a Clang method declaration.
  class ObjCMethodConventions : public OwnershipConventions {
    clang::ObjCMethodDecl *Method;
  public:
    ObjCMethodConventions(clang::ObjCMethodDecl *method) : Method(method) {}

    bool isResultAutoreleased() const override {
      return (Method->getResultType()->isObjCRetainableType() &&
              !Method->hasAttr<clang::NSReturnsRetainedAttr>());
    }

    bool isArgConsumed(unsigned index) const override {
      // 'self'
      if (index == 0) {
        return Method->hasAttr<clang::NSConsumesSelfAttr>();
      // 'cmd'
      } else if (index == 1) {
        return false;
      // Formal parameters.
      } else {
        assert(index - 2 < Method->param_size());
        auto param = *(Method->param_begin() + (index - 2));
        assert(param);
        return param->hasAttr<clang::NSConsumedAttr>();
      }
    }
  };

  /// Ownership conventions derived from a selector family.
  class ObjCSelectorConventions : public OwnershipConventions {
    CanType SubstResultType;
    Selector::Family Family;
  public:
    ObjCSelectorConventions(CanType substResultType, Selector::Family family)
      : SubstResultType(substResultType), Family(family) {}

    bool isResultAutoreleased() const override {
      // If the result type isn't a retainable object pointer, this
      // isn't applicable.
      if (!SubstResultType->getClassOrBoundGenericClass())
        return false;

      switch (Family) {
      case Selector::Family::Alloc:
      case Selector::Family::Init:
      case Selector::Family::Copy:
      case Selector::Family::MutableCopy:
      case Selector::Family::New:
        return false;

      case Selector::Family::None:
        return true;
      }
      llvm_unreachable("bad selector family!");
    }

    bool isArgConsumed(unsigned index) const override {
      // The only conventionally-consumed argument is an init method's
      // 'self'.
      return (index == 0 && Family == Selector::Family::Init);
    }
  };
}

/// Emit the given expression (of class-metatype type) as an
/// Objective-C class reference.
static void emitObjCClassRValue(IRGenFunction &IGF, Expr *E, Explosion &out) {
  out.addUnmanaged(ObjCClassEmitter(IGF).visit(E));
}

/// Prepare a call using ObjC method dispatch.
CallEmission irgen::prepareObjCMethodCall(IRGenFunction &IGF, FuncDecl *method,
                                          Expr *self,
                                          CanType substResultType,
                                          ArrayRef<Substitution> subs,
                                          ExplosionKind maxExplosion,
                                          unsigned maxUncurry) {
  CanType origFormalType = method->getType()->getCanonicalType();
  ObjCMethodSignature sig(IGF.IGM, origFormalType);

  // Create the appropriate messenger function.
  // FIXME: this needs to be target-specific.
  llvm::Constant *messenger;
  if (sig.IsIndirectReturn) {
    messenger = IGF.IGM.getObjCMsgSendStretFn();
  } else {
    messenger = IGF.IGM.getObjCMsgSendFn();
  }

  // Cast the messenger to the right type.
  messenger = llvm::ConstantExpr::getBitCast(messenger,
                                             sig.FnTy->getPointerTo());

  CallEmission emission(IGF, Callee::forKnownFunction(AbstractCC::C,
                                                      origFormalType,
                                                      substResultType,
                                                      subs,
                                                      messenger,
                                                      ManagedValue(nullptr),
                                                      ExplosionKind::Minimal,
                                                      /*uncurry*/ 1));

  // Compute the selector.
  Selector selector(method);

  // Respect conventions.
  Callee &callee = emission.getMutableCallee();
  if (auto clangDecl = method->getClangDecl()) {
    callee.setOwnershipConventions<ObjCMethodConventions>(
                                     cast<clang::ObjCMethodDecl>(clangDecl));
  } else {
    auto selectorFamily = selector.getFamily();
    callee.setOwnershipConventions<ObjCSelectorConventions>(substResultType,
                                                            selectorFamily);
  }

  // Emit the self argument.
  Explosion selfValues(ExplosionKind::Minimal);
  if (method->isInstanceMember()) {
    IGF.emitRValue(self, selfValues);
  } else {
    emitObjCClassRValue(IGF, self, selfValues);
  }
  assert(selfValues.size() == 1);

  // Add the selector value.
  auto selectorRef = IGF.IGM.getAddrOfObjCSelectorRef(selector.str());
  llvm::Value *selectorV;
  if (IGF.IGM.Opts.UseJIT) {
    // When generating JIT'd code, we need to call sel_registerName() to force
    // the runtime to unique the selector.
    selectorV = IGF.Builder.CreateLoad(Address(selectorRef,
                                               IGF.IGM.getPointerAlignment()));
    selectorV = IGF.Builder.CreateCall(IGF.IGM.getObjCSelRegisterNameFn(),
                                       selectorV);
  } else {
    // When generating statically-compiled code, just build a reference to
    // the selector.
    selectorV = IGF.Builder.CreateLoad(Address(selectorRef,
                                               IGF.IGM.getPointerAlignment()));
  }
  selfValues.addUnmanaged(selectorV);

  // Add that to the emission.
  emission.addArg(selfValues);

  return emission;
}
