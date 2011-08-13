//===--- SemaDecl.cpp - Swift Semantic Analysis for Declarations ----------===//
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
//  This file implements semantic analysis for Swift declarations.
//
//===----------------------------------------------------------------------===//

#include "SemaDecl.h"
#include "Sema.h"
#include "Parser.h"
#include "Scope.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/SMLoc.h"
using namespace swift;

typedef std::pair<unsigned, ValueDecl*> ValueScopeEntry;
typedef llvm::ScopedHashTable<Identifier, ValueScopeEntry> ValueScopeHTType;

typedef std::pair<unsigned, TypeAliasDecl*> TypeScopeEntry;
typedef llvm::ScopedHashTable<Identifier, TypeScopeEntry> TypeScopeHTType;

static ValueScopeHTType &getValueHT(void *P) {
  return *(ValueScopeHTType*)P;
}
static TypeScopeHTType &getTypeHT(void *P) {
  return *(TypeScopeHTType*)P;
}

typedef llvm::DenseMap<Identifier,TypeAliasDecl*> UnresolvedTypesMapTy;
static UnresolvedTypesMapTy &getUnresolvedTypesHT(void *P){
  return *(UnresolvedTypesMapTy*)P;
}

SemaDecl::SemaDecl(Sema &S)
  : SemaBase(S),
    ValueScopeHT(new ValueScopeHTType()),
    TypeScopeHT(new TypeScopeHTType()),
    CurScope(0),
    UnresolvedTypes(new UnresolvedTypesMapTy()) {
}

SemaDecl::~SemaDecl() {
  delete &getValueHT(ValueScopeHT);
  delete &getTypeHT(TypeScopeHT);
  delete &getUnresolvedTypesHT(UnresolvedTypes);
}


/// handleEndOfTranslationUnit - This is invoked at the end of the translation
/// unit.
void SemaDecl::handleEndOfTranslationUnit(TranslationUnitDecl *TUD,
                                          SMLoc FileStart,
                                          ArrayRef<ExprStmtOrDecl> Items,
                                          SMLoc FileEnd) {
  // First thing, we transform the body into a brace expression.
  ExprStmtOrDecl *NewElements = 
    S.Context.AllocateCopy<ExprStmtOrDecl>(Items.begin(), Items.end());
  TUD->Body = new (S.Context) BraceStmt(FileStart, NewElements, Items.size(),
                                        FileEnd);
  
  // Do a prepass over the declarations to make sure they have basic sanity and
  // to find the list of top-level value declarations.
  for (unsigned i = 0, e = TUD->Body->NumElements; i != e; ++i) {
    if (!TUD->Body->Elements[i].is<Decl*>()) continue;
    
    Decl *D = TUD->Body->Elements[i].get<Decl*>();
       
    // If any top-level value decl has an unresolved type, then it is erroneous.
    // It is not valid to have something like "var x = 4" at the top level, all
    // types must be explicit here.
    ValueDecl *VD = dyn_cast<ValueDecl>(D);
    if (VD == 0) continue;

    // FIXME: This can be better handled in the various ActOnDecl methods when
    // they get passed in a parent context decl.

    // Verify that values have a type specified.
    if (false && VD->Ty->is<DependentType>()) {
      error(VD->getLocStart(),
            "top level declarations require a type specifier");
      // FIXME: Should mark the decl as invalid.
      VD->Ty = TupleType::getEmpty(S.Context);
    }
  }
  
  // Verify that any forward declared types were ultimately defined.
  // TODO: Move this to name binding!
  unsigned Next = 0;
  for (TypeAliasDecl *Decl : UnresolvedTypeList) {
    
    // If a type got defined, remove it from the vector.
    if (!Decl->UnderlyingTy.isNull())
      continue;
    
    UnresolvedTypeList[Next++] = Decl;
  }
  // Strip out stuff that got replaced.
  UnresolvedTypeList.resize(Next);
    
  TUD->UnresolvedTypesForParser = S.Context.AllocateCopy(UnresolvedTypeList);
}

//===----------------------------------------------------------------------===//
// Name lookup.
//===----------------------------------------------------------------------===//

/// LookupValueName - Perform a lexical scope lookup for the specified name,
/// returning the active decl if found or null if not.
ValueDecl *SemaDecl::LookupValueName(Identifier Name) {
  std::pair<unsigned, ValueDecl*> Res = getValueHT(ValueScopeHT).lookup(Name);
  // If we found nothing, or we found a decl at the top-level, return nothing.
  // We ignore results at the top-level because we may have overloading that
  // will be resolved properly by name binding.
  if (Res.first == 0) return 0;
  return Res.second;
}

/// LookupTypeName - Perform a lexical scope lookup for the specified name in
/// a type context, returning the decl if found or installing and returning a
/// new Unresolved one if not.
TypeAliasDecl *SemaDecl::LookupTypeName(Identifier Name, SMLoc Loc) {
  TypeAliasDecl *TAD = getTypeHT(TypeScopeHT).lookup(Name).second;
  if (TAD) return TAD;
  
  // If we don't have a definition for this type, introduce a new TypeAliasDecl
  // with an unresolved underlying type.
  TAD = new (S.Context) TypeAliasDecl(Loc, Name, Type());
  getUnresolvedTypesHT(UnresolvedTypes)[Name] = TAD;
  UnresolvedTypeList.push_back(TAD);
  
  // Inject this into the outermost scope so that subsequent name lookups of the
  // same type will find it.
  llvm::ScopedHashTableScope<Identifier, TypeScopeEntry> *S =
    getTypeHT(TypeScopeHT).getCurScope();
  while (S->getParentScope())
    S = S->getParentScope();
  
  getTypeHT(TypeScopeHT).insertIntoScope(S, Name, std::make_pair(0, TAD));
  return TAD;
}

static void DiagnoseRedefinition(ValueDecl *Prev, ValueDecl *New, SemaDecl &SD){
  assert(New != Prev && "Cannot conflict with self");
  if (New->Init)
    SD.error(New->getLocStart(), "definition conflicts with previous value");
  else
    SD.error(New->getLocStart(), "declaration conflicts with previous value");
  
  if (Prev->Init)
    SD.note(Prev->getLocStart(), "previous definition here");
  else
    SD.note(Prev->getLocStart(), "previous declaration here");
}

/// CheckValidOverload - Check whether it is ok for D1 and D2 to be declared at
/// the same scope.  This check is a transitive relationship, so if "D1 is a
/// valid overload of D2" and "D2 is a valid overload of D3" then we know that
/// D1/D3 are valid overloads and we don't have to check all permutations.
static bool CheckValidOverload(const ValueDecl *D1, const ValueDecl *D2,
                               SemaDecl &SD) {
  if (D1->Attrs.InfixPrecedence != D2->Attrs.InfixPrecedence) {
    SD.error(D1->getLocStart(),
             "infix precedence of functions in an overload set must match");
    SD.note(D2->getLocStart(), "previous declaration here");
    return true;
  }
  
  // Otherwise, everything is fine.
  return false;
}

/// AddToScope - Register the specified decl as being in the current lexical
/// scope.
void SemaDecl::AddToScope(ValueDecl *D) {
  // If we have a shadowed variable definition, check to see if we have a
  // redefinition: two definitions in the same scope with the same name.
  ValueScopeHTType &ValueHT = getValueHT(ValueScopeHT);
  ValueScopeHTType::iterator EntryI = ValueHT.begin(D->Name);
  
  // A redefinition is a hit in the scoped table at the same depth.
  if (EntryI != ValueHT.end() && EntryI->first == CurScope->getDepth()) {
    ValueDecl *PrevDecl = EntryI->second;
    
    // If this is at top-level scope, we allow overloading.  If not, we don't.
    // FIXME: This should be tied to whether the scope corresponds to a
    // DeclContext like a TranslationUnit or a Namespace.  Add a bit to Scope
    // to track this?
    if (CurScope->getDepth() != 0)
      return DiagnoseRedefinition(PrevDecl, D, *this);
    
    // If this is at top-level scope, validate that the members of the overload
    // set all agree.
    
    // Check to see if D and PrevDecl are valid in the same overload set.
    if (CheckValidOverload(D, PrevDecl, *this))
      return;
    
    // Note: we don't check whether all of the elements of the overload set have
    // different argument types.  This is checked later.
  }
  
  getValueHT(ValueScopeHT).insert(D->Name,
                                  std::make_pair(CurScope->getDepth(), D));
}

//===----------------------------------------------------------------------===//
// Declaration handling.
//===----------------------------------------------------------------------===//



TypeAliasDecl *SemaDecl::ActOnTypeAlias(SMLoc TypeAliasLoc,
                                        Identifier Name, Type Ty) {
  std::pair<unsigned,TypeAliasDecl*> Entry =getTypeHT(TypeScopeHT).lookup(Name);

  // If we have no existing entry, or if the existing entry is at a different
  // scope level then this is a valid insertion.
  if (Entry.second == 0 || Entry.first != CurScope->getDepth()) {
    TypeAliasDecl *New = new (S.Context) TypeAliasDecl(TypeAliasLoc, Name, Ty);
    getTypeHT(TypeScopeHT).insert(Name,
                                  std::make_pair(CurScope->getDepth(), New));
    return New;
  }
  
  TypeAliasDecl *ExistingDecl = Entry.second;
  
  // If the previous definition was just a use of an undeclared type, complete
  // the type now.
  if (ExistingDecl->UnderlyingTy.isNull()) {
    // Remove the entry for this type from the UnresolvedTypes map.
    getUnresolvedTypesHT(UnresolvedTypes).erase(Name);
    
    // This will get removed from UnresolvedTypeList at the end of the TU.
    
    // Update the decl we already have to be the correct type.
    ExistingDecl->TypeAliasLoc = TypeAliasLoc;
    ExistingDecl->UnderlyingTy = Ty;
    return ExistingDecl;
  }
  
  // Otherwise, we have a redefinition: two definitions in the same scope with
  // the same name.
  error(TypeAliasLoc,
        "redefinition of type named '" +StringRef(Name.get()) + "'");
  warning(ExistingDecl->getLocStart(), "previous declaration here");
  return ExistingDecl;
}
