//===--- NonFixedTypeInfo.h - Non-fixed-layout types ------------*- C++ -*-===//
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
//  This file defines classes that are useful for implementing types
//  that do not have a fixed representation and cannot be laid out
//  statically.
//
//  These classes are useful only for creating TypeInfo
//  implementations; unlike the similiarly-named FixedTypeInfo, they
//  do not provide a supplemental API.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IRGEN_NONFIXEDTYPEINFO_H
#define SWIFT_IRGEN_NONFIXEDTYPEINFO_H

#include "GenOpaque.h"
#include "IndirectTypeInfo.h"

namespace swift {
namespace irgen {

/// An abstract CRTP class designed for types whose storage size,
/// alignment, and stride need to be fetched from the value witness
/// table for the type.
template <class Impl>
class WitnessSizedTypeInfo : public IndirectTypeInfo<Impl, TypeInfo> {
private:
  typedef IndirectTypeInfo<Impl, TypeInfo> super;

protected:
  const Impl &asImpl() const { return static_cast<const Impl &>(*this); }

  WitnessSizedTypeInfo(llvm::Type *type, Alignment align, IsPOD_t pod,
                       IsBitwiseTakable_t bt)
    : super(type, align, pod, bt, TypeInfo::STIK_None) {}

private:
  /// Bit-cast the given pointer to the right type and assume it as an
  /// address of this type.
  Address getAsBitCastAddress(IRGenFunction &IGF, llvm::Value *addr) const {
    addr = IGF.Builder.CreateBitCast(addr,
                                     this->getStorageType()->getPointerTo());
    return this->getAddressForPointer(addr);
  }

public:
  // This is useful for metaprogramming.
  static bool isFixed() { return false; }

  OwnedAddress allocateBox(IRGenFunction &IGF,
                           CanType T,
                           const llvm::Twine &name) const override {
    // Allocate a new object using the allocBox runtime call.
    llvm::Value *metadata = IGF.emitTypeMetadataRef(T);
    llvm::Value *box, *address;
    IGF.emitAllocBoxCall(metadata, box, address);
    return OwnedAddress(getAsBitCastAddress(IGF, address), box);
  }

  ContainedAddress allocateStack(IRGenFunction &IGF,
                                 CanType T,
                                 const llvm::Twine &name) const override {
    // Make a fixed-size buffer.
    Address buffer = IGF.createAlloca(IGF.IGM.getFixedBufferTy(),
                                      getFixedBufferAlignment(IGF.IGM),
                                      name);

    // Allocate an object of the appropriate type within it.
    llvm::Value *metadata = IGF.emitTypeMetadataRef(T);
    llvm::Value *address =
      emitAllocateBufferCall(IGF, metadata, buffer);
    return { buffer, getAsBitCastAddress(IGF, address) };
  }

  void deallocateStack(IRGenFunction &IGF, Address buffer,
                       CanType T) const override {
    llvm::Value *metadata = IGF.emitTypeMetadataRef(T);
    emitDeallocateBufferCall(IGF, metadata, buffer);
  }

  llvm::Value *getValueWitnessTable(IRGenFunction &IGF, CanType T) const {
    auto metadata = IGF.emitTypeMetadataRef(T);
    return IGF.emitValueWitnessTableRefForMetadata(metadata);
  }

  std::pair<llvm::Value*,llvm::Value*>
  getSizeAndAlignmentMask(IRGenFunction &IGF, CanType T) const override {
    auto wtable = getValueWitnessTable(IGF, T);
    auto size = emitLoadOfSize(IGF, wtable);
    auto align = emitLoadOfAlignmentMask(IGF, wtable);
    return { size, align };
  }

  std::tuple<llvm::Value*,llvm::Value*,llvm::Value*>
  getSizeAndAlignmentMaskAndStride(IRGenFunction &IGF, CanType T) const override {
    auto wtable = getValueWitnessTable(IGF, T);
    auto size = emitLoadOfSize(IGF, wtable);
    auto align = emitLoadOfAlignmentMask(IGF, wtable);
    auto stride = emitLoadOfStride(IGF, wtable);
    return std::make_tuple(size, align, stride);
  }

  llvm::Value *getSize(IRGenFunction &IGF, CanType T) const override {
    auto wtable = getValueWitnessTable(IGF, T);
    return emitLoadOfSize(IGF, wtable);
  }

  llvm::Value *getAlignmentMask(IRGenFunction &IGF, CanType T) const override {
    auto wtable = getValueWitnessTable(IGF, T);
    return emitLoadOfAlignmentMask(IGF, wtable);
  }

  llvm::Value *getStride(IRGenFunction &IGF, CanType T) const override {
    auto wtable = getValueWitnessTable(IGF, T);
    return emitLoadOfStride(IGF, wtable);
  }

  llvm::Value *isDynamicallyPackedInline(IRGenFunction &IGF,
                                         CanType T) const override {
    auto wtable = getValueWitnessTable(IGF, T);
    return emitLoadOfIsInline(IGF, wtable);
  }

  /// FIXME: Dynamic extra inhabitant lookup.
  bool mayHaveExtraInhabitants(IRGenModule &) const override { return false; }
  llvm::Value *getExtraInhabitantIndex(IRGenFunction &IGF,
                                       Address src, CanType T) const override {
    llvm_unreachable("dynamic extra inhabitants not supported");
  }
  void storeExtraInhabitant(IRGenFunction &IGF,
                            llvm::Value *index,
                            Address dest, CanType T) const override {
    llvm_unreachable("dynamic extra inhabitants not supported");
  }

  llvm::Constant *getStaticSize(IRGenModule &IGM) const override {
    return nullptr;
  }
  llvm::Constant *getStaticAlignmentMask(IRGenModule &IGM) const override {
    return nullptr;
  }
  llvm::Constant *getStaticStride(IRGenModule &IGM) const override {
    return nullptr;
  }
};

}
}

#endif
