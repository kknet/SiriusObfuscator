//===-ThinLTOCodeGenerator.h - LLVM Link Time Optimizer -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the ThinLTOCodeGenerator class, similar to the
// LTOCodeGenerator but for the ThinLTO scheme. It provides an interface for
// linker plugin.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LTO_THINLTOCODEGENERATOR_H
#define LLVM_LTO_THINLTOCODEGENERATOR_H

#include "llvm-c/lto.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Target/TargetOptions.h"

#include <string>

namespace llvm {
class StringRef;
class LLVMContext;
class TargetMachine;

/// Wrapper around MemoryBufferRef, owning the identifier
class ThinLTOBuffer {
  std::string OwnedIdentifier;
  StringRef Buffer;

public:
  ThinLTOBuffer(StringRef Buffer, StringRef Identifier)
      : OwnedIdentifier(Identifier), Buffer(Buffer) {}

  MemoryBufferRef getMemBuffer() const {
    return MemoryBufferRef(Buffer,
                           {OwnedIdentifier.c_str(), OwnedIdentifier.size()});
  }
  StringRef getBuffer() const { return Buffer; }
  StringRef getBufferIdentifier() const { return OwnedIdentifier; }
};

/// Helper to gather options relevant to the target machine creation
struct TargetMachineBuilder {
  Triple TheTriple;
  std::string MCpu;
  std::string MAttr;
  TargetOptions Options;
  Optional<Reloc::Model> RelocModel;
  CodeGenOpt::Level CGOptLevel = CodeGenOpt::Aggressive;

  std::unique_ptr<TargetMachine> create() const;
};

/// This class define an interface similar to the LTOCodeGenerator, but adapted
/// for ThinLTO processing.
/// The ThinLTOCodeGenerator is not intended to be reuse for multiple
/// compilation: the model is that the client adds modules to the generator and
/// ask to perform the ThinLTO optimizations / codegen, and finally destroys the
/// codegenerator.
class ThinLTOCodeGenerator {
public:
  /// Add given module to the code generator.
  void addModule(StringRef Identifier, StringRef Data);

  /**
   * Adds to a list of all global symbols that must exist in the final generated
   * code. If a symbol is not listed there, it will be optimized away if it is
   * inlined into every usage.
   */
  void preserveSymbol(StringRef Name);

  /**
   * Adds to a list of all global symbols that are cross-referenced between
   * ThinLTO files. If the ThinLTO CodeGenerator can ensure that every
   * references from a ThinLTO module to this symbol is optimized away, then
   * the symbol can be discarded.
   */
  void crossReferenceSymbol(StringRef Name);

  /**
   * Process all the modules that were added to the code generator in parallel.
   *
   * Client can access the resulting object files using getProducedBinaries(),
   * unless setGeneratedObjectsDirectory() has been called, in which case
   * results are available through getProducedBinaryFiles().
   */
  void run();

  /**
   * Return the "in memory" binaries produced by the code generator. This is
   * filled after run() unless setGeneratedObjectsDirectory() has been
   * called, in which case results are available through
   * getProducedBinaryFiles().
   */
  std::vector<std::unique_ptr<MemoryBuffer>> &getProducedBinaries() {
    return ProducedBinaries;
  }

  /**
   * Return the "on-disk" binaries produced by the code generator. This is
   * filled after run() when setGeneratedObjectsDirectory() has been
   * called, in which case results are available through getProducedBinaries().
   */
  std::vector<std::string> &getProducedBinaryFiles() {
    return ProducedBinaryFiles;
  }

  /**
   * \defgroup Options setters
   * @{
   */

  /**
   * \defgroup Cache controlling options
   *
   * These entry points control the ThinLTO cache. The cache is intended to
   * support incremental build, and thus needs to be persistent accross build.
   * The client enabled the cache by supplying a path to an existing directory.
   * The code generator will use this to store objects files that may be reused
   * during a subsequent build.
   * To avoid filling the disk space, a few knobs are provided:
   *  - The pruning interval limit the frequency at which the garbage collector
   *    will try to scan the cache directory to prune it from expired entries.
   *    Setting to -1 disable the pruning (default).
   *  - The pruning expiration time indicates to the garbage collector how old
   *    an entry needs to be to be removed.
   *  - Finally, the garbage collector can be instructed to prune the cache till
   *    the occupied space goes below a threshold.
   * @{
   */

  struct CachingOptions {
    std::string Path;                    // Path to the cache, empty to disable.
    int PruningInterval = 1200;          // seconds, -1 to disable pruning.
    unsigned int Expiration = 7 * 24 * 3600;     // seconds (1w default).
    unsigned MaxPercentageOfAvailableSpace = 75; // percentage.
  };

  /// Provide a path to a directory where to store the cached files for
  /// incremental build.
  void setCacheDir(std::string Path) { CacheOptions.Path = std::move(Path); }

  /// Cache policy: interval (seconds) between two prune of the cache. Set to a
  /// negative value (default) to disable pruning. A value of 0 will be ignored.
  void setCachePruningInterval(int Interval) {
    if (Interval)
      CacheOptions.PruningInterval = Interval;
  }

  /// Cache policy: expiration (in seconds) for an entry.
  /// A value of 0 will be ignored.
  void setCacheEntryExpiration(unsigned Expiration) {
    if (Expiration)
      CacheOptions.Expiration = Expiration;
  }

  /**
   * Sets the maximum cache size that can be persistent across build, in terms
   * of percentage of the available space on the the disk. Set to 100 to
   * indicate no limit, 50 to indicate that the cache size will not be left over
   * half the available space. A value over 100 will be reduced to 100, and a
   * value of 0 will be ignored.
   *
   *
   * The formula looks like:
   *  AvailableSpace = FreeSpace + ExistingCacheSize
   *  NewCacheSize = AvailableSpace * P/100
   *
   */
  void setMaxCacheSizeRelativeToAvailableSpace(unsigned Percentage) {
    if (Percentage)
      CacheOptions.MaxPercentageOfAvailableSpace = Percentage;
  }

  /**@}*/

  /// Set the path to a directory where to save temporaries at various stages of
  /// the processing.
  void setSaveTempsDir(std::string Path) { SaveTempsDir = std::move(Path); }

  /// Set the path to a directory where to save generated object files. This
  /// path can be used by a linker to request on-disk files instead of in-memory
  /// buffers. When set, results are available through getProducedBinaryFiles()
  /// instead of getProducedBinaries().
  void setGeneratedObjectsDirectory(std::string Path) {
    SavedObjectsDirectoryPath = std::move(Path);
  }

  /// CPU to use to initialize the TargetMachine
  void setCpu(std::string Cpu) { TMBuilder.MCpu = std::move(Cpu); }

  /// Subtarget attributes
  void setAttr(std::string MAttr) { TMBuilder.MAttr = std::move(MAttr); }

  /// TargetMachine options
  void setTargetOptions(TargetOptions Options) {
    TMBuilder.Options = std::move(Options);
  }

  /// Enable the Freestanding mode: indicate that the optimizer should not
  /// assume builtins are present on the target.
  void setFreestanding(bool Enabled) { Freestanding = Enabled; }

  /// CodeModel
  void setCodePICModel(Optional<Reloc::Model> Model) {
    TMBuilder.RelocModel = Model;
  }

  /// CodeGen optimization level
  void setCodeGenOptLevel(CodeGenOpt::Level CGOptLevel) {
    TMBuilder.CGOptLevel = CGOptLevel;
  }

  /// IR optimization level: from 0 to 3.
  void setOptLevel(unsigned NewOptLevel) {
    OptLevel = (NewOptLevel > 3) ? 3 : NewOptLevel;
  }

  /// Disable CodeGen, only run the stages till codegen and stop. The output
  /// will be bitcode.
  void disableCodeGen(bool Disable) { DisableCodeGen = Disable; }

  /// Perform CodeGen only: disable all other stages.
  void setCodeGenOnly(bool CGOnly) { CodeGenOnly = CGOnly; }

  /**@}*/

  /**
   * \defgroup Set of APIs to run individual stages in isolation.
   * @{
   */

  /**
   * Produce the combined summary index from all the bitcode files:
   * "thin-link".
   */
  std::unique_ptr<ModuleSummaryIndex> linkCombinedIndex();

  /**
   * Perform promotion and renaming of exported internal functions,
   * and additionally resolve weak and linkonce symbols.
   * Index is updated to reflect linkage changes from weak resolution.
   */
  void promote(Module &Module, ModuleSummaryIndex &Index);

  /**
   * Compute and emit the imported files for module at \p ModulePath.
   */
  static void emitImports(StringRef ModulePath, StringRef OutputName,
                          ModuleSummaryIndex &Index);

  /**
   * Perform cross-module importing for the module identified by
   * ModuleIdentifier.
   */
  void crossModuleImport(Module &Module, ModuleSummaryIndex &Index);

  /**
   * Compute the list of summaries needed for importing into module.
   */
  static void gatherImportedSummariesForModule(
      StringRef ModulePath, ModuleSummaryIndex &Index,
      std::map<std::string, GVSummaryMapTy> &ModuleToSummariesForIndex);

  /**
   * Perform internalization. Index is updated to reflect linkage changes.
   */
  void internalize(Module &Module, ModuleSummaryIndex &Index);

  /**
   * Perform post-importing ThinLTO optimizations.
   */
  void optimize(Module &Module);

  /**
   * Perform ThinLTO CodeGen.
   */
  std::unique_ptr<MemoryBuffer> codegen(Module &Module);

  /**@}*/

private:
  /// Helper factory to build a TargetMachine
  TargetMachineBuilder TMBuilder;

  /// Vector holding the in-memory buffer containing the produced binaries, when
  /// SavedObjectsDirectoryPath isn't set.
  std::vector<std::unique_ptr<MemoryBuffer>> ProducedBinaries;

  /// Path to generated files in the supplied SavedObjectsDirectoryPath if any.
  std::vector<std::string> ProducedBinaryFiles;

  /// Vector holding the input buffers containing the bitcode modules to
  /// process.
  std::vector<ThinLTOBuffer> Modules;

  /// Set of symbols that need to be preserved outside of the set of bitcode
  /// files.
  StringSet<> PreservedSymbols;

  /// Set of symbols that are cross-referenced between bitcode files.
  StringSet<> CrossReferencedSymbols;

  /// Control the caching behavior.
  CachingOptions CacheOptions;

  /// Path to a directory to save the temporary bitcode files.
  std::string SaveTempsDir;

  /// Path to a directory to save the generated object files.
  std::string SavedObjectsDirectoryPath;

  /// Flag to enable/disable CodeGen. When set to true, the process stops after
  /// optimizations and a bitcode is produced.
  bool DisableCodeGen = false;

  /// Flag to indicate that only the CodeGen will be performed, no cross-module
  /// importing or optimization.
  bool CodeGenOnly = false;

  /// Flag to indicate that the optimizer should not assume builtins are present
  /// on the target.
  bool Freestanding = false;

  /// IR Optimization Level [0-3].
  unsigned OptLevel = 3;
};
}
#endif
