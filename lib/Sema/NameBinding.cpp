//===--- NameBinding.cpp - Name Binding -----------------------------------===//
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
//  This file implements name binding for Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "swift/AST/AST.h"
#include "swift/AST/Component.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/ASTWalker.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/OwningPtr.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/system_error.h"
#include "llvm/Support/Path.h"
using namespace swift;

//===----------------------------------------------------------------------===//
// NameBinder
//===----------------------------------------------------------------------===//

typedef TranslationUnit::ImportedModule ImportedModule;
typedef llvm::PointerUnion<const ImportedModule*, OneOfType*> BoundScope;

namespace {  
  class NameBinder {
    llvm::error_code findModule(StringRef Module, 
                                SourceLoc ImportLoc,
                                llvm::OwningPtr<llvm::MemoryBuffer> &Buffer);
    
  public:
    TranslationUnit *TU;
    ASTContext &Context;
    
    NameBinder(TranslationUnit *TU) : TU(TU), Context(TU->Ctx) {
    }
    ~NameBinder() {
    }
    
    template<typename ...ArgTypes>
    InFlightDiagnostic diagnose(ArgTypes... Args) {
      return Context.Diags.diagnose(Args...);
    }
    
    void addImport(ImportDecl *ID, SmallVectorImpl<ImportedModule> &Result);

    /// resolveIdentifierType - Perform name binding for a IdentifierType,
    /// resolving it or diagnosing the error as appropriate and return true on
    /// failure.  On failure, this leaves IdentifierType alone, otherwise it
    /// fills in the Components.
    bool resolveIdentifierType(IdentifierType *DNT);
    
  private:
    /// getModule - Load a module referenced by an import statement,
    /// emitting an error at the specified location and returning null on
    /// failure.
    Module *getModule(std::pair<Identifier,SourceLoc> ModuleID);
  };
}

llvm::error_code NameBinder::findModule(StringRef Module, 
                                        SourceLoc ImportLoc,
                                llvm::OwningPtr<llvm::MemoryBuffer> &Buffer) {
  std::string ModuleFilename = Module.str() + std::string(".swift");
  
  llvm::SmallString<128> InputFilename;
  
  // First, search in the directory corresponding to the import location.
  // FIXME: This screams for a proper FileManager abstraction.
  llvm::SourceMgr &SourceMgr = Context.SourceMgr;
  int CurrentBufferID = SourceMgr.FindBufferContainingLoc(ImportLoc.Value);
  if (CurrentBufferID >= 0) {
    const llvm::MemoryBuffer *ImportingBuffer 
      = SourceMgr.getBufferInfo(CurrentBufferID).Buffer;
    StringRef CurrentDirectory 
      = llvm::sys::path::parent_path(ImportingBuffer->getBufferIdentifier());
    if (!CurrentDirectory.empty()) {
      InputFilename = CurrentDirectory;
      llvm::sys::path::append(InputFilename, ModuleFilename);
      llvm::error_code Err = llvm::MemoryBuffer::getFile(InputFilename, Buffer);
      if (!Err)
        return Err;
    }
  }
  
  // Second, search in the current directory.
  llvm::error_code Err = llvm::MemoryBuffer::getFile(ModuleFilename, Buffer);
  if (!Err)
    return Err;

  // If we fail, search each import search path.
  for (auto Path : Context.ImportSearchPaths) {
    InputFilename = Path;
    llvm::sys::path::append(InputFilename, ModuleFilename);
    Err = llvm::MemoryBuffer::getFile(InputFilename, Buffer);
    if (!Err)
      return Err;
  }

  return Err;
}

Module *NameBinder::getModule(std::pair<Identifier, SourceLoc> ModuleID) {
  // TODO: We currently just recursively parse referenced modules.  This works
  // fine for now since they are each a single file.  Ultimately we'll want a
  // compiled form of AST's like clang's that support lazy deserialization.
  
  // Open the input file.
  llvm::OwningPtr<llvm::MemoryBuffer> InputFile;
  if (llvm::error_code Err = findModule(ModuleID.first.str(), ModuleID.second,
                                        InputFile)) {
    diagnose(ModuleID.second, diag::sema_opening_import,
             ModuleID.first.str(), Err.message());
    return 0;
  }

  unsigned BufferID =
    Context.SourceMgr.AddNewSourceBuffer(InputFile.take(),
                                         ModuleID.second.Value);

  // For now, treat all separate modules as unique components.
  Component *Comp = new (Context.Allocate<Component>(1)) Component();

  // Parse the translation unit, but don't do name binding or type checking.
  // This can produce new errors etc if the input is erroneous.
  TranslationUnit *TU = parseTranslationUnit(BufferID, Comp, Context);
  if (TU == 0)
    return 0;
  
  // We have to do name binding on it to ensure that types are fully resolved.
  // This should eventually be eliminated by having actual fully resolved binary
  // dumps of the code instead of reparsing though.
  performNameBinding(TU);
  performTypeChecking(TU);
  
  return TU;
}


void NameBinder::addImport(ImportDecl *ID, 
                           SmallVectorImpl<ImportedModule> &Result) {
  ArrayRef<ImportDecl::AccessPathElement> Path = ID->getAccessPath();
  Module *M = getModule(Path[0]);
  if (M == 0) return;
  
  // FIXME: Validate the access path against the module.  Reject things like
  // import swift.aslkdfja
  if (Path.size() > 2) {
    diagnose(Path[2].second, diag::invalid_declaration_imported);
    return;
  }
  
  Result.push_back(std::make_pair(Path.slice(1), M));
}

/// resolveIdentifierType - Perform name binding for a IdentifierType,
/// resolving it or diagnosing the error as appropriate and return true on
/// failure.  On failure, this leaves IdentifierType alone, otherwise it fills
/// in the Components.
bool NameBinder::resolveIdentifierType(IdentifierType *DNT) {
  const MutableArrayRef<IdentifierType::Component> &Components =DNT->Components;
  
  // If name lookup for the base of the type didn't get resolved in the
  // parsing phase, do a global lookup for it.
  if (Components[0].Value.isNull()) {
    Identifier Name = Components[0].Id;
    SourceLoc Loc = Components[0].Loc;
    
    // Perform an unqualified lookup.
    SmallVector<ValueDecl*, 4> Decls;
    TU->lookupGlobalValue(Name, NLKind::UnqualifiedLookup, Decls);
    
    // If we find multiple results, we have an ambiguity error.
    // FIXME: This should be reevaluated and probably turned into a new NLKind.
    // Certain matches (e.g. of a function) should just be filtered out/ignored.
    if (Decls.size() > 1) {
      diagnose(Loc, diag::abiguous_type_base, Name)
        << SourceRange(Loc, Components.back().Loc);
      for (ValueDecl *D : Decls)
        diagnose(D->getLocStart(), diag::found_candidate);
      return true;
    }
    
    if (!Decls.empty()) {
      Components[0].Value = Decls[0];
    } else {
      // If that fails, this may be the name of a module, try looking that up.
      for (const ImportedModule &ImpEntry : TU->getImportedModules())
        if (ImpEntry.second->Name == Name) {
          Components[0].Value = ImpEntry.second;
          break;
        }
    
      // If we still don't have anything, we fail.
      if (Components[0].Value.isNull()) {
        diagnose(Loc, Components.size() == 1 ? 
                   diag::use_undeclared_type : diag::unknown_name_in_type, Name)
          << SourceRange(Loc, Components.back().Loc);
        return true;
      }
    }
  }
  
  assert(!Components[0].Value.isNull() && "Failed to get a base");
  
  // Now that we have a base, iteratively resolve subsequent member entries.
  auto LastOne = Components[0];
  for (auto &C : Components.slice(1, Components.size()-1)) {
    // TODO: Only support digging into modules so far.
    if (auto M = LastOne.Value.dyn_cast<Module*>()) {
#if 0
      // FIXME: Why is this lookupType instead of lookupValue?  How are they
      // different?
#endif
      C.Value = M->lookupType(Module::AccessPathTy(), C.Id, 
                              NLKind::QualifiedLookup);
    } else {
      diagnose(C.Loc, diag::unknown_dotted_type_base, LastOne.Id)
        << SourceRange(Components[0].Loc, Components.back().Loc);
      return true;
    }
    
    if (C.Value.isNull()) {
      diagnose(C.Loc, diag::invalid_member_type, C.Id, LastOne.Id)
      << SourceRange(Components[0].Loc, Components.back().Loc);
      return true;
    }
    
    LastOne = C;
  }
  
  // Finally, sanity check that the last value is a type.
  if (ValueDecl *Last = Components.back().Value.dyn_cast<ValueDecl*>())
    if (auto TAD = dyn_cast<TypeAliasDecl>(Last)) {
      Components[Components.size()-1].Value = TAD->getAliasType();
      return false;
    }

  diagnose(Components.back().Loc,
           Components.size() == 1 ? diag::named_definition_isnt_type :
             diag::dotted_reference_not_type, Components.back().Id)
    << SourceRange(Components[0].Loc, Components.back().Loc);
  return true;
}


//===----------------------------------------------------------------------===//
// performNameBinding
//===----------------------------------------------------------------------===//

/// BindNameToIVar - We have an unresolved reference to an identifier in some
/// DeclContext.  Check to see if this is a reference to an instance variable,
/// and return an AST for the reference if so.  If not, return null with no
/// error emitted.
static Expr *BindNameToIVar(UnresolvedDeclRefExpr *UDRE, DeclContext *DC, 
                            NameBinder &Binder) {
  // Scan up the DeclContext chain until we find a FuncExpr.
  for (; DC; DC = DC->getParent()) {
    FuncExpr *FE = dyn_cast<FuncExpr>(DC);
    if (FE == 0) continue;
    
    // If this is a non-plus function, it's parameter pattern will have a 'this'
    // argument without location information.
    VarDecl *ThisDecl = FE->getImplicitThisDecl();
    if (ThisDecl == 0) continue;

    //ThisDecl->dump();
    
    
    // This is happening after name binding types.  
      
    //(tuple_element_expr type='[byref(implicit)] CGSize' field #1
    //  (look_through_oneof_expr type='[byref(implicit)] (origin : CGPoint, size : CGSize)'
    //    (declref_expr type='[byref(implicit)] oneof { CGRect : (origin : CGPoint, size : CGSize)}' decl=t
  }
  
  return 0;
}

/// BindName - Bind an UnresolvedDeclRefExpr by performing name lookup and
/// returning the resultant expression.  If this reference is inside of a decl
/// (e.g. in a function body) then DC is the DeclContext, otherwise it is null.
static Expr *BindName(UnresolvedDeclRefExpr *UDRE, DeclContext *DC, 
                      NameBinder &Binder) {
  
  // If we are inside of a declaration context, check to see if there are any
  // ivars in scope, and if so, whether this is a reference to one of them.
  if (DC)
    if (Expr *E = BindNameToIVar(UDRE, DC, Binder))
      return E;

  // Process UnresolvedDeclRefExpr by doing an unqualified lookup.
  Identifier Name = UDRE->getName();
  SourceLoc Loc = UDRE->getLoc();
  SmallVector<ValueDecl*, 4> Decls;
  // Perform standard value name lookup.
  Binder.TU->lookupGlobalValue(Name, NLKind::UnqualifiedLookup, Decls);

  // If that fails, this may be the name of a module, try looking that up.
  if (Decls.empty()) {
    for (const ImportedModule &ImpEntry : Binder.TU->getImportedModules())
      if (ImpEntry.second->Name == Name) {
        ModuleType *MT = ModuleType::get(ImpEntry.second);
        return new (Binder.Context) ModuleExpr(Loc, MT);
      }
  }

  if (Decls.empty()) {
    Binder.diagnose(Loc, diag::use_unresolved_identifier, Name);
    return new (Binder.Context) ErrorExpr(Loc);
  }

  return OverloadSetRefExpr::createWithCopy(Decls, Loc);
}


/// performNameBinding - Once parsing is complete, this walks the AST to
/// resolve names and do other top-level validation.
///
/// At this parsing has been performed, but we still have UnresolvedDeclRefExpr
/// nodes for unresolved value names, and we may have unresolved type names as
/// well.  This handles import directives and forward references.
void swift::performNameBinding(TranslationUnit *TU) {
  NameBinder Binder(TU);

  SmallVector<ImportedModule, 8> ImportedModules;
  
  // Import the builtin library as an implicit import.
  // FIXME: This should only happen for translation units in the standard
  // library.
  ImportedModules.push_back(std::make_pair(Module::AccessPathTy(),
                                           TU->Ctx.TheBuiltinModule));
  
  // FIXME: For translation units not in the standard library, we should import
  // swift.swift implicitly.  We need a way for swift.swift itself to not
  // recursively import itself though.

  // Do a prepass over the declarations to find and load the imported modules.
  for (auto Elt : TU->Body->getElements())
    if (Decl *D = Elt.dyn_cast<Decl*>()) {
      if (ImportDecl *ID = dyn_cast<ImportDecl>(D))
        Binder.addImport(ID, ImportedModules);
    }
  
  TU->setImportedModules(TU->Ctx.AllocateCopy(ImportedModules));
  
  // Type binding.  Loop over all of the unresolved types in the translation
  // unit, resolving them with imports.
  for (TypeAliasDecl *TA : TU->getUnresolvedTypes()) {
    if (TypeAliasDecl *Result =
          Binder.TU->lookupGlobalType(TA->getName(),
                                      NLKind::UnqualifiedLookup)) {
      assert(!TA->hasUnderlyingType() && "Not an unresolved type");
      // Update the decl we already have to be the correct type.
      TA->setTypeAliasLoc(Result->getTypeAliasLoc());
      TA->setUnderlyingType(Result->getUnderlyingType());
      continue;
    }
    
    Binder.diagnose(TA->getLocStart(), diag::use_undeclared_type,
                    TA->getName());
    
    TA->setUnderlyingType(TU->Ctx.TheErrorType);
  }

  // Loop over all the unresolved dotted types in the translation unit,
  // resolving them if possible.
  for (IdentifierType *DNT : TU->getUnresolvedIdentifierTypes()) {
    if (Binder.resolveIdentifierType(DNT)) {
      TypeBase *Error = TU->Ctx.TheErrorType.getPointer();

      // This IdentifierType resolved to the error type.
      for (auto &C : DNT->Components)
        // FIXME: Want MutableArrayRef
        const_cast<IdentifierType::Component&>(C).Value = Error;
    }
  }

  struct NameBindingWalker : public ASTWalker {
    NameBinder &Binder;
    NameBindingWalker(NameBinder &binder) : Binder(binder) {}
    
    /// CurFuncs - This is the stack of FuncExprs that we're nested in.
    SmallVector<FuncExpr*, 4> CurFuncs;
    
    virtual bool walkToExprPre(Expr *E) {
      if (FuncExpr *FE = dyn_cast<FuncExpr>(E))
        CurFuncs.push_back(FE);
        
      return true;
    }
    
    Expr *walkToExprPost(Expr *E) {
      if (FuncExpr *FE = dyn_cast<FuncExpr>(E)) {
        assert(CurFuncs.back() == FE && "Decl misbalance!");
        CurFuncs.pop_back();
        return E;
      }
      
      if (UnresolvedDeclRefExpr *UDRE = dyn_cast<UnresolvedDeclRefExpr>(E))
        return BindName(UDRE, CurFuncs.empty() ? 0 : CurFuncs.back(), Binder);
      return E;
    }
  };
  NameBindingWalker walker(Binder);
  
  // Now that we know the top-level value names, go through and resolve any
  // UnresolvedDeclRefExprs that exist.
  TU->Body = cast<BraceStmt>(TU->Body->walk(walker));

  TU->ASTStage = TranslationUnit::NameBound;
  verify(TU);
}

