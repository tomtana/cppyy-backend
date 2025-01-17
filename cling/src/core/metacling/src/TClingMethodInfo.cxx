// @(#)root/core/meta:$Id$
// Author: Paul Russo   30/07/2012

/*************************************************************************
 * Copyright (C) 1995-2000, Rene Brun and Fons Rademakers.               *
 * All rights reserved.                                                  *
 *                                                                       *
 * For the licensing terms see $ROOTSYS/LICENSE.                         *
 * For the list of contributors see $ROOTSYS/README/CREDITS.             *
 *************************************************************************/

/** \class TClingMethodInfo
Emulation of the CINT MethodInfo class.

The CINT C++ interpreter provides an interface to metadata about
a function through the MethodInfo class.  This class provides the
same functionality, using an interface as close as possible to
MethodInfo but the typedef metadata comes from the Clang C++
compiler, not CINT.
*/

#include "TClingMethodInfo.h"

#include "TClingCallFunc.h"
#include "TClingClassInfo.h"
#include "TClingMethodArgInfo.h"
#include "TDictionary.h"
#include "TClingTypeInfo.h"
#include "TError.h"
#include "TClingUtils.h"
#include "TCling.h"
#include "ThreadLocalStorage.h"

#include "cling/Interpreter/Interpreter.h"
#include "cling/Interpreter/LookupHelper.h"
#include "cling/Utils/AST.h"

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/GlobalDecl.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/AST/Type.h"
#include "clang/Basic/IdentifierTable.h"
#include "clang/Sema/Lookup.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/Template.h"
#include "clang/Sema/TemplateDeduction.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/raw_ostream.h"

#include <string>


using namespace clang;

namespace CppyyLegacy {

class TClingMethodInfo::UsingIterator
{
public:
   typedef clang::UsingDecl::shadow_iterator Iterator;

   UsingIterator(cling::Interpreter* interp, Iterator begin, Iterator end) : fInterp(interp), fIter(begin), fEnd(end) {}
   explicit UsingIterator(cling::Interpreter* interp, clang::UsingDecl *decl) :
      fInterp(interp), fIter(decl->shadow_begin()), fEnd(decl->shadow_end()) {}
   UsingIterator(const UsingIterator& s) : fInterp(s.fInterp), fIter(s.fIter), fEnd(s.fEnd) {}

   FunctionDecl *operator* () const {
      if (!(*fIter)) return nullptr;   // first iter is not checked in InternalNext()
      clang::ConstructorUsingShadowDecl* shadow_ctor = llvm::dyn_cast<clang::ConstructorUsingShadowDecl>(*fIter);
      if (shadow_ctor) {
         clang::CXXConstructorDecl* base_ctor = llvm::dyn_cast<clang::CXXConstructorDecl>(shadow_ctor->getTargetDecl());
         if (base_ctor) {
            if (base_ctor->isImplicit()) return nullptr; // skip as Cling will generate these anyway
            return fInterp->getSema().findInheritingConstructor(base_ctor->getSourceRange().getBegin(), base_ctor, shadow_ctor);
         }
      } else {
         clang::UsingShadowDecl* shadow_decl = llvm::dyn_cast<clang::UsingShadowDecl>(*fIter);
         if (shadow_decl) {
            clang::CXXMethodDecl* method = llvm::dyn_cast<clang::CXXMethodDecl>(shadow_decl->getTargetDecl());
            if (method) return method;
         }
      }
      return llvm::dyn_cast<clang::FunctionDecl>((*fIter)->getTargetDecl());
   }
   FunctionDecl *operator-> () const { return this->operator*(); }
   UsingIterator & operator++ () { ++fIter; return *this; }
   UsingIterator   operator++ (int) {
      UsingIterator tmp(fInterp, fIter,fEnd);
      ++(*this);
      return tmp;
   }
   bool operator!() { return fIter == fEnd; }
   operator bool() { return fIter != fEnd; }

private:
   cling::Interpreter* fInterp;
   Iterator fIter;
   Iterator fEnd;
};


TClingMethodInfo::TClingMethodInfo(const TClingMethodInfo &rhs) :
   TClingDeclInfo(rhs),
   fInterp(rhs.fInterp),
   fContexts(rhs.fContexts),
   fFirstTime(rhs.fFirstTime),
   fContextIdx(rhs.fContextIdx),
   fIter(rhs.fIter),
   fTitle(rhs.fTitle),
   fTemplateSpec(rhs.fTemplateSpec),
   fUsingIter(nullptr),
   fAccessDecl(rhs.fAccessDecl)
{

   if (rhs.fUsingIter) {
      // Internal loop over using shadow decls active
      R__LOCKGUARD(gInterpreterMutex);
      fUsingIter = new UsingIterator(*rhs.fUsingIter);
   }
}


TClingMethodInfo& TClingMethodInfo::operator=(const TClingMethodInfo &rhs) {
   if (this == &rhs)
      return *this;

   this->TClingDeclInfo::operator=(rhs);
   fInterp = rhs.fInterp;
   fContexts = rhs.fContexts;
   fFirstTime = rhs.fFirstTime;
   fContextIdx = rhs.fContextIdx;
   fIter = rhs.fIter;
   fTitle = rhs.fTitle;
   fTemplateSpec = rhs.fTemplateSpec;
   fUsingIter = nullptr;
   fAccessDecl = rhs.fAccessDecl;

   if (rhs.fUsingIter) {
      // The UsingIterator query the decl.
      R__LOCKGUARD(gInterpreterMutex);
      fUsingIter = new UsingIterator(*rhs.fUsingIter);
   }

   return *this;
}


TClingMethodInfo::TClingMethodInfo(cling::Interpreter *interp,
                                   TClingClassInfo *ci)
   : TClingDeclInfo(nullptr), fInterp(interp), fFirstTime(true), fContextIdx(0U), fTitle(""),
     fTemplateSpec(nullptr), fUsingIter(nullptr), fAccessDecl(nullptr)
{
   R__LOCKGUARD(gInterpreterMutex);

   if (!ci || !ci->IsValid()) {
      return;
   }
   clang::CXXRecordDecl *cxxdecl = llvm::dyn_cast<clang::CXXRecordDecl>(const_cast<clang::Decl*>(ci->GetDecl()));
   if (cxxdecl) {
      // Make sure we have an entry for all the implicit function.

      // Could trigger deserialization of decls.
      cling::Interpreter::PushTransactionRAII RAII(interp);

      fInterp->getSema().ForceDeclarationOfImplicitMembers(cxxdecl);
   }
   clang::DeclContext *dc =
      llvm::cast<clang::DeclContext>(const_cast<clang::Decl*>(ci->GetDecl()));
   dc->collectAllContexts(fContexts);
   // Could trigger deserialization of decls.
   cling::Interpreter::PushTransactionRAII RAII(interp);
   fIter = dc->decls_begin();
   InternalNext();
   fFirstTime = true;
}

TClingMethodInfo::TClingMethodInfo(cling::Interpreter *interp,
                                   const clang::FunctionDecl *FD)
   : TClingDeclInfo(FD), fInterp(interp), fFirstTime(true), fContextIdx(0U), fTitle(""),
     fTemplateSpec(nullptr), fUsingIter(nullptr), fAccessDecl(nullptr)
{

}

TClingMethodInfo::~TClingMethodInfo()
{
   delete fTemplateSpec;
   delete fUsingIter;
}

TDictionary::DeclId_t TClingMethodInfo::GetDeclId() const
{
   if (!IsValid()) {
      return TDictionary::DeclId_t();
   }
   return (const clang::Decl*)(GetMethodDecl()->getCanonicalDecl());
}

const clang::FunctionDecl *TClingMethodInfo::GetMethodDecl() const
{
   return cast_or_null<FunctionDecl>(GetDecl());
}

void TClingMethodInfo::Init(const clang::FunctionDecl *decl)
{
   fContexts.clear();
   fFirstTime = true;
   fContextIdx = 0U;
   fIter = clang::DeclContext::decl_iterator();
   fTemplateSpec = 0;
   delete fUsingIter;
   fUsingIter = nullptr;
   fAccessDecl = nullptr;
   fDecl = decl;
}

void *TClingMethodInfo::InterfaceMethod(const CppyyLegacy::TMetaUtils::TNormalizedCtxt &normCtxt, bool as_iface) const
{
   if (!IsValid()) {
      return 0;
   }
   R__LOCKGUARD(gInterpreterMutex);
   TClingCallFunc cf(fInterp,normCtxt);
   cf.SetFunc(this);
   return cf.InterfaceMethod(as_iface);
}

const clang::Decl* TClingMethodInfo::GetDeclSlow() const
{
   if (fTemplateSpec) {
      return fTemplateSpec;
   } else if (fUsingIter) {
      if (!(*fUsingIter)) return nullptr;   // first iter is not checked in InternalNext()
      R__LOCKGUARD(gInterpreterMutex);
      cling::Interpreter::PushTransactionRAII RAII(fInterp);
      return *(*fUsingIter);
   }
   return *fIter;
}

int TClingMethodInfo::NArg() const
{
   if (!IsValid()) {
      return -1;
   }
   const clang::FunctionDecl *fd = GetMethodDecl();
   unsigned num_params = fd->getNumParams();
   // Truncate cast to fit cint interface.
   return static_cast<int>(num_params);
}

int TClingMethodInfo::NDefaultArg() const
{
   if (!IsValid()) {
      return -1;
   }
   const clang::FunctionDecl *fd = GetMethodDecl();
   unsigned num_params = fd->getNumParams();
   unsigned min_args = fd->getMinRequiredArguments();
   unsigned defaulted_params = num_params - min_args;
   // Truncate cast to fit cint interface.
   return static_cast<int>(defaulted_params);
}

/*
static bool HasUnexpandedParameterPack(clang::QualType QT, clang::Sema& S) {
   if (llvm::isa<PackExpansionType>(*QT)) {
      // We are not going to expand the pack here...
      return true;
   }
   SmallVector<UnexpandedParameterPack, 4> Unexpanded;
   S.collectUnexpandedParameterPacks (QT, Unexpanded);

   return !Unexpanded.empty();
}
 */

static const clang::FunctionDecl *
GetOrInstantiateFuncTemplateWithDefaults(clang::FunctionTemplateDecl* FTDecl,
                                         clang::Sema& S,
                                         const cling::LookupHelper& LH) {
   // NO!!
   return nullptr;

   // Force instantiation if it doesn't exist yet, by looking it up.
   using namespace clang;

   auto templateParms = FTDecl->getTemplateParameters();
   if (templateParms->containsUnexpandedParameterPack())
      return nullptr;

   if (templateParms->getMinRequiredArguments() > 0)
      return nullptr;

   if (templateParms->size() > 0) {
      NamedDecl *arg0 = *templateParms->begin();
      if (arg0->isTemplateParameterPack())
         return nullptr;
      if (auto TTP = dyn_cast<TemplateTypeParmDecl>(*templateParms->begin())) {
         if (!TTP->hasDefaultArgument())
            return nullptr;
      } else if (auto NTTP = dyn_cast<NonTypeTemplateParmDecl>(
         *templateParms->begin())) {
         if (!NTTP->hasDefaultArgument())
            return nullptr;
      } else {
         // TemplateTemplateParmDecl, pack
         return nullptr;
      }
   }

   FunctionDecl *templatedDecl = FTDecl->getTemplatedDecl();
   Decl *declCtxDecl = dyn_cast<Decl>(FTDecl->getDeclContext());

   // We have a function template
   //     template <class X = int, int i = 7> void func(int a0, X a1[i], X::type a2[i])
   // which has defaults for all its template parameters `X` and `i`. To
   // instantiate it we have to do a lookup, which in turn needs the function
   // argument types, e.g. `int[12]`.
   // If the function argument type is dependent (a1 and a2) we need to
   // substitute the types first, using the template arguments derived from the
   // template parameters' defaults.
   llvm::SmallVector<TemplateArgument, 8> defaultTemplateArgs(templateParms->size());
   for (int iParam = 0, nParams = templateParms->size(); iParam < nParams; ++iParam) {
      const NamedDecl* templateParm = templateParms->getParam(iParam);
      if (templateParm->isTemplateParameterPack()) {
         // shouldn't end up here
         assert(0 && "unexpected template parameter pack");
         return nullptr;
      } if (auto TTP = dyn_cast<TemplateTypeParmDecl>(templateParm)) {
         if (!TTP->hasDefaultArgument())
            return nullptr;
         defaultTemplateArgs[iParam] = TemplateArgument(TTP->getDefaultArgument());
      } else if (auto NTTP = dyn_cast<NonTypeTemplateParmDecl>(templateParm)) {
         if (!NTTP->hasDefaultArgument())
            return nullptr;
         defaultTemplateArgs[iParam] = TemplateArgument(NTTP->getDefaultArgument());
      } else if (auto TTP = dyn_cast<TemplateTemplateParmDecl>(templateParm)) {
         if (!TTP->hasDefaultArgument())
            return nullptr;
         defaultTemplateArgs[iParam] = TemplateArgument(TTP->getDefaultArgument().getArgument());
      } else {
         // shouldn't end up here
         assert(0 && "unexpected template parameter kind");
         return nullptr;
      }
   }

   // Now substitute the dependent function parameter types given defaultTemplateArgs.
   llvm::SmallVector<QualType, 8> paramTypes;
   // Provide an instantiation context that suppresses errors:
   // DeducedTemplateArgumentSubstitution! (ROOT-8422)
   SmallVector<DeducedTemplateArgument, 4> DeducedArgs;
   sema::TemplateDeductionInfo Info{SourceLocation()};

   Sema::InstantiatingTemplate Inst(S, Info.getLocation(), FTDecl,
                                    defaultTemplateArgs,
                                    Sema::CodeSynthesisContext::DeducedTemplateArgumentSubstitution,
                                    Info);

   // Collect the function arguments of the templated function, substituting
   // dependent types as possible.
   TemplateArgumentList templArgList(TemplateArgumentList::OnStack, defaultTemplateArgs);
   MultiLevelTemplateArgumentList MLTAL{templArgList};
   for (const clang::ParmVarDecl *param: templatedDecl->parameters()) {
      QualType paramType = param->getOriginalType();

      // If the function type is dependent, try to resolve it through the class's
      // template arguments. If that fails, skip this function.
      if (paramType->isDependentType()) {
         /*if (HasUnexpandedParameterPack(paramType, S)) {
            // We are not going to expand the pack here...
            skip = true;
            break;
         }*/

         paramType = S.SubstType(paramType, MLTAL, SourceLocation(),
                                 templatedDecl->getDeclName());

         if (paramType.isNull() || paramType->isDependentType()) {
            // Even after resolving the types through the surrounding template
            // this argument type is still dependent: do not look it up.
            return nullptr;
         }
      }
      paramTypes.push_back(paramType);
   }

   return LH.findFunctionProto(declCtxDecl, FTDecl->getNameAsString(),
                               paramTypes, LH.NoDiagnostics,
                               templatedDecl->getType().isConstQualified());
}

int TClingMethodInfo::InternalNext()
{

   assert(!fDecl && "This is not an iterator!");

   fNameCache.clear(); // invalidate the cache.

   if (!fFirstTime && !*fIter) {
      // Iterator is already invalid.
      return 0;
   }
   while (true) {
      // If we had fTemplateSpec we don't need it anymore, but advance
      // to the next decl.
      fTemplateSpec = nullptr;

      // Advance to the next decl.
      if (fFirstTime) {
         // The cint semantics are weird.
         fFirstTime = false;
      }
      else {
         if (fUsingIter && *fUsingIter) {
            while (++(*fUsingIter)) {
               if (*(*fUsingIter))
                  return 1;
            }
            delete fUsingIter; fUsingIter = nullptr; fAccessDecl = nullptr;
            ++fIter;
         } else
            ++fIter;
      }
      // Fix it if we have gone past the end of the current decl context.
      while (!*fIter) {
         ++fContextIdx;
         if (fContextIdx >= fContexts.size()) {
            // Iterator is now invalid.
            return 0;
         }
         clang::DeclContext *dc = fContexts[fContextIdx];
         // Could trigger deserialization of decls.

         cling::Interpreter::PushTransactionRAII RAII(fInterp);
         fIter = dc->decls_begin();
         if (*fIter) {
            // Good, a non-empty context.
            break;
         }
      }

      auto cur_decl = *fIter;
      if (const auto templateDecl = llvm::dyn_cast<clang::FunctionTemplateDecl>(cur_decl)) {
         // Instantiation below can trigger deserialization.
         cling::Interpreter::PushTransactionRAII RAII(fInterp);

         // If this function template can be instantiated without template
         // arguments then it's worth having it. This commonly happens for
         // enable_if'ed functions.
         fTemplateSpec = GetOrInstantiateFuncTemplateWithDefaults(templateDecl, fInterp->getSema(),
                                                                  fInterp->getLookupHelper());

         if (fTemplateSpec && fTemplateSpec->isDeleted())
            fTemplateSpec = nullptr;

         if (fTemplateSpec)
            return 1;
      }


      clang::UsingDecl* udecl = llvm::dyn_cast<clang::UsingDecl>(cur_decl);
      if (udecl) {
          // A UsingDecl potentially brings in a bunch of functions, so
          // start an inner loop to catch them all
          delete fUsingIter; fAccessDecl = udecl;
          fUsingIter = new UsingIterator(fInterp, udecl);
          return 1;
      }

      // Return if this decl is a function or method.
      if (auto *FD = llvm::dyn_cast<clang::FunctionDecl>(cur_decl)) {
         if (!FD->isDeleted())
            // Iterator is now valid.
            return 1;
      }

      // Collect internal `__cling_N5xxx' inline namespaces; they will be traversed later
      if (auto NS = dyn_cast<NamespaceDecl>(cur_decl)) {
         if (NS->getDeclContext()->isTranslationUnit() && NS->isInlineNamespace())
            fContexts.push_back(NS);
      }
//      if (clang::FunctionDecl *fdecl = llvm::dyn_cast<clang::FunctionDecl>(cur_decl)) {
//         if (fdecl->getAccess() == clang::AS_public || fdecl->getAccess() == clang::AS_none) {
//            // Iterator is now valid.
//            return 1;
//         }
//      }
   }

   return 0;
}

int TClingMethodInfo::Next()
{
   return InternalNext();
}

long TClingMethodInfo::Property() const
{
   if (!IsValid()) {
      return 0L;
   }
   long property = 0L;
   property |= kIsCompiled;
   const clang::FunctionDecl *fd = GetMethodDecl();
   if (fd->isDeleted())
      return 0L;

   if (fd->isConstexpr())
      property |= kIsConstexpr;

   const clang::Decl* accessdecl = fAccessDecl ? fAccessDecl : fd;
   switch (accessdecl->getAccess()) {
      case clang::AS_public:
         property |= kIsPublic;
         break;
      case clang::AS_protected:
         property |= kIsProtected;
         break;
      case clang::AS_private:
         property |= kIsPrivate;
         break;
      case clang::AS_none:
         if (fd->getDeclContext()->isNamespace())
            property |= kIsPublic;
         break;
      default:
         // IMPOSSIBLE
         break;
   }
   if (fd->getStorageClass() == clang::SC_Static) {
      property |= kIsStatic;
   }
   clang::QualType qt = fd->getReturnType().getCanonicalType();
   if (qt.isConstQualified()) {
      property |= kIsConstant;
   }
   while (1) {
      if (qt->isArrayType()) {
         qt = llvm::cast<clang::ArrayType>(qt)->getElementType();
         continue;
      }
      else if (qt->isReferenceType()) {
         property |= kIsReference;
         qt = llvm::cast<clang::ReferenceType>(qt)->getPointeeType();
         continue;
      }
      else if (qt->isPointerType()) {
         property |= kIsPointer;
         if (qt.isConstQualified()) {
            property |= kIsConstPointer;
         }
         qt = llvm::cast<clang::PointerType>(qt)->getPointeeType();
         continue;
      }
      else if (qt->isMemberPointerType()) {
         qt = llvm::cast<clang::MemberPointerType>(qt)->getPointeeType();
         continue;
      }
      break;
   }
   if (qt.isConstQualified()) {
      property |= kIsConstant;
   }
   if (const clang::CXXMethodDecl *md =
            llvm::dyn_cast<clang::CXXMethodDecl>(fd)) {
      if (md->getMethodQualifiers().hasConst()) {
         property |= kIsConstant | kIsConstMethod;
      }
      if (md->isVirtual()) {
         property |= kIsVirtual;
      }
      if (md->isPure()) {
         property |= kIsPureVirtual;
      }
      if (const clang::CXXConstructorDecl *cd =
               llvm::dyn_cast<clang::CXXConstructorDecl>(md)) {
         if (cd->isExplicit()) {
            property |= kIsExplicit;
         }
      }
      else if (const clang::CXXConversionDecl *cd =
                  llvm::dyn_cast<clang::CXXConversionDecl>(md)) {
         if (cd->isExplicit()) {
            property |= kIsExplicit;
         }
      }
   }
   return property;
}

long TClingMethodInfo::ExtraProperty() const
{
   // Return the property not already defined in Property
   // See TDictionary's EFunctionProperty
   if (!IsValid()) {
      return 0L;
   }
   long property = 0;
   const clang::FunctionDecl *fd = GetMethodDecl();
   if (fd->isDeleted())
      return 0L;

   if (fd->isOverloadedOperator())
      property |= kIsOperator;
   if (llvm::isa<clang::CXXConversionDecl>(fd))
      property |= kIsConversion;
   if (llvm::isa<clang::CXXConstructorDecl>(fd))
      property |= kIsConstructor;
   if (llvm::isa<clang::CXXDestructorDecl>(fd))
      property |= kIsDestructor;
   if (fd->isInlined())
      property |= kIsInlined;
   if (fd->getTemplatedKind() != clang::FunctionDecl::TK_NonTemplate)
      property |= kIsTemplateSpec;
   return property;
}

TClingTypeInfo *TClingMethodInfo::Type() const
{
   TTHREAD_TLS_DECL_ARG( TClingTypeInfo, ti, fInterp );
   if (!IsValid()) {
      ti.Init(clang::QualType());
      return &ti;
   }
   if (llvm::isa<clang::CXXConstructorDecl>(GetMethodDecl())) {
      // CINT claims that constructors return the class object.
      const clang::TypeDecl* ctorClass = llvm::dyn_cast_or_null<clang::TypeDecl>
         (GetMethodDecl()->getDeclContext());
      if (!ctorClass) {
         Error("TClingMethodInfo::Type", "Cannot find DeclContext for constructor!");
      } else {
         clang::QualType qt(ctorClass->getTypeForDecl(), 0);
         ti.Init(qt);
      }
   } else {
      clang::QualType qt = GetMethodDecl()->getReturnType();
      // if the type is not deduced (e.g. 'auto'), it will cause problems downstream that
      // need the FunctionDecl to resolve, so pre-empt those issues by deducing it here.
      if (qt.getTypePtr()->isUndeducedType()) {
          clang::FunctionDecl* decl = (clang::FunctionDecl*)GetMethodDecl();
          cling::Interpreter::PushTransactionRAII RAII(fInterp);
          if (!fInterp->getSema().DeduceReturnType(decl, decl->getPointOfInstantiation(), false))
              qt = GetMethodDecl()->getReturnType();
      }
      ti.Init(qt);
   }
   return &ti;
}

std::string TClingMethodInfo::GetMangledName() const
{
   if (!IsValid()) {
      return "";
   }
   std::string mangled_name;
   mangled_name.clear();
   const FunctionDecl* D = GetMethodDecl();

   R__LOCKGUARD(gInterpreterMutex);
   cling::Interpreter::PushTransactionRAII RAII(fInterp);
   GlobalDecl GD;
   if (const CXXConstructorDecl* Ctor = dyn_cast<CXXConstructorDecl>(D))
     GD = GlobalDecl(Ctor, Ctor_Complete);
   else if (const CXXDestructorDecl* Dtor = dyn_cast<CXXDestructorDecl>(D))
     GD = GlobalDecl(Dtor, Dtor_Deleting);
   else
     GD = GlobalDecl(D);

   cling::utils::Analyze::maybeMangleDeclName(GD, mangled_name);
   return mangled_name;
}

const char *TClingMethodInfo::Name()
{
   if (!IsValid()) {
      return 0;
   }
   if (!fNameCache.empty())
     return fNameCache.c_str();

   ((TCling*)gCling)->GetFunctionName(GetMethodDecl(), fNameCache);
   return fNameCache.c_str();
}

const char *TClingMethodInfo::TypeName() const
{
   if (!IsValid()) {
      // FIXME: Cint does not check!
      return 0;
   }
   return Type()->Name();
}

const char *TClingMethodInfo::Title()
{
   if (!IsValid()) {
      return 0;
   }

   //NOTE: We can't use it as a cache due to the "thoughtful" self iterator
   //if (fTitle.size())
   //   return fTitle.c_str();

   // Try to get the comment either from the annotation or the header file if present

   // Iterate over the redeclarations, we can have multiple definitions in the
   // redecl chain (came from merging of pcms).
   const FunctionDecl *FD = GetMethodDecl();

   R__LOCKGUARD(gInterpreterMutex);

   // Could trigger deserialization of decls.
   cling::Interpreter::PushTransactionRAII RAII(fInterp);
   if (const FunctionDecl *AnnotFD
       = CppyyLegacy::TMetaUtils::GetAnnotatedRedeclarable(FD)) {
      if (AnnotateAttr *A = AnnotFD->getAttr<AnnotateAttr>()) {
         fTitle = A->getAnnotation().str();
         return fTitle.c_str();
      }
   }
   if (!FD->isFromASTFile()) {
      // Try to get the comment from the header file if present
      // but not for decls from AST file, where rootcling would have
      // created an annotation
      fTitle = CppyyLegacy::TMetaUtils::GetComment(*FD).str();
   }

   return fTitle.c_str();
}

} // namespace CppyyLegacy
