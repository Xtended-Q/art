/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "optimizing_compiler.h"

#include <fstream>
#include <memory>
#include <sstream>

#include <stdint.h>

#include "art_method-inl.h"
#include "base/arena_allocator.h"
#include "base/arena_containers.h"
#include "base/dumpable.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/mutex.h"
#include "base/scoped_arena_allocator.h"
#include "base/timing_logger.h"
#include "builder.h"
#include "class_root.h"
#include "code_generator.h"
#include "compiled_method.h"
#include "compiler.h"
#include "debug/elf_debug_writer.h"
#include "debug/method_debug_info.h"
#include "dex/dex_file_types.h"
#include "dex/verification_results.h"
#include "dex/verified_method.h"
#include "driver/compiled_method_storage.h"
#include "driver/compiler_options.h"
#include "driver/dex_compilation_unit.h"
#include "graph_checker.h"
#include "graph_visualizer.h"
#include "inliner.h"
#include "jit/debugger_interface.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "jit/jit_logger.h"
#include "jni/quick/jni_compiler.h"
#include "linker/linker_patch.h"
#include "nodes.h"
#include "oat_quick_method_header.h"
#include "prepare_for_register_allocation.h"
#include "reference_type_propagation.h"
#include "register_allocator_linear_scan.h"
#include "select_generator.h"
#include "ssa_builder.h"
#include "ssa_liveness_analysis.h"
#include "ssa_phi_elimination.h"
#include "stack_map_stream.h"
#include "utils/assembler.h"
#include "verifier/verifier_compiler_binding.h"

namespace art {

static constexpr size_t kArenaAllocatorMemoryReportThreshold = 8 * MB;

static constexpr const char* kPassNameSeparator = "$";

/**
 * Used by the code generator, to allocate the code in a vector.
 */
class CodeVectorAllocator final : public CodeAllocator {
 public:
  explicit CodeVectorAllocator(ArenaAllocator* allocator)
      : memory_(allocator->Adapter(kArenaAllocCodeBuffer)) {}

  uint8_t* Allocate(size_t size) override {
    memory_.resize(size);
    return &memory_[0];
  }

  ArrayRef<const uint8_t> GetMemory() const override { return ArrayRef<const uint8_t>(memory_); }
  uint8_t* GetData() { return memory_.data(); }

 private:
  ArenaVector<uint8_t> memory_;

  DISALLOW_COPY_AND_ASSIGN(CodeVectorAllocator);
};

/**
 * Filter to apply to the visualizer. Methods whose name contain that filter will
 * be dumped.
 */
static constexpr const char kStringFilter[] = "";

class PassScope;

class PassObserver : public ValueObject {
 public:
  PassObserver(HGraph* graph,
               CodeGenerator* codegen,
               std::ostream* visualizer_output,
               const CompilerOptions& compiler_options,
               Mutex& dump_mutex)
      : graph_(graph),
        last_seen_graph_size_(0),
        cached_method_name_(),
        timing_logger_enabled_(compiler_options.GetDumpPassTimings()),
        timing_logger_(timing_logger_enabled_ ? GetMethodName() : "", true, true),
        disasm_info_(graph->GetAllocator()),
        visualizer_oss_(),
        visualizer_output_(visualizer_output),
        visualizer_enabled_(!compiler_options.GetDumpCfgFileName().empty()),
        visualizer_(&visualizer_oss_, graph, *codegen),
        visualizer_dump_mutex_(dump_mutex),
        graph_in_bad_state_(false) {
    if (timing_logger_enabled_ || visualizer_enabled_) {
      if (!IsVerboseMethod(compiler_options, GetMethodName())) {
        timing_logger_enabled_ = visualizer_enabled_ = false;
      }
      if (visualizer_enabled_) {
        visualizer_.PrintHeader(GetMethodName());
        codegen->SetDisassemblyInformation(&disasm_info_);
      }
    }
  }

  ~PassObserver() {
    if (timing_logger_enabled_) {
      LOG(INFO) << "TIMINGS " << GetMethodName();
      LOG(INFO) << Dumpable<TimingLogger>(timing_logger_);
    }
    DCHECK(visualizer_oss_.str().empty());
  }

  void DumpDisassembly() REQUIRES(!visualizer_dump_mutex_) {
    if (visualizer_enabled_) {
      visualizer_.DumpGraphWithDisassembly();
      FlushVisualizer();
    }
  }

  void SetGraphInBadState() { graph_in_bad_state_ = true; }

  const char* GetMethodName() {
    // PrettyMethod() is expensive, so we delay calling it until we actually have to.
    if (cached_method_name_.empty()) {
      cached_method_name_ = graph_->GetDexFile().PrettyMethod(graph_->GetMethodIdx());
    }
    return cached_method_name_.c_str();
  }

 private:
  void StartPass(const char* pass_name) REQUIRES(!visualizer_dump_mutex_) {
    VLOG(compiler) << "Starting pass: " << pass_name;
    // Dump graph first, then start timer.
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass= */ false, graph_in_bad_state_);
      FlushVisualizer();
    }
    if (timing_logger_enabled_) {
      timing_logger_.StartTiming(pass_name);
    }
  }

  void FlushVisualizer() REQUIRES(!visualizer_dump_mutex_) {
    MutexLock mu(Thread::Current(), visualizer_dump_mutex_);
    *visualizer_output_ << visualizer_oss_.str();
    visualizer_output_->flush();
    visualizer_oss_.str("");
    visualizer_oss_.clear();
  }

  void EndPass(const char* pass_name, bool pass_change) REQUIRES(!visualizer_dump_mutex_) {
    // Pause timer first, then dump graph.
    if (timing_logger_enabled_) {
      timing_logger_.EndTiming();
    }
    if (visualizer_enabled_) {
      visualizer_.DumpGraph(pass_name, /* is_after_pass= */ true, graph_in_bad_state_);
      FlushVisualizer();
    }

    // Validate the HGraph if running in debug mode.
    if (kIsDebugBuild) {
      if (!graph_in_bad_state_) {
        GraphChecker checker(graph_);
        last_seen_graph_size_ = checker.Run(pass_change, last_seen_graph_size_);
        if (!checker.IsValid()) {
          LOG(FATAL) << "Error after " << pass_name << ": " << Dumpable<GraphChecker>(checker);
        }
      }
    }
  }

  static bool IsVerboseMethod(const CompilerOptions& compiler_options, const char* method_name) {
    // Test an exact match to --verbose-methods. If verbose-methods is set, this overrides an
    // empty kStringFilter matching all methods.
    if (compiler_options.HasVerboseMethods()) {
      return compiler_options.IsVerboseMethod(method_name);
    }

    // Test the kStringFilter sub-string. constexpr helper variable to silence unreachable-code
    // warning when the string is empty.
    constexpr bool kStringFilterEmpty = arraysize(kStringFilter) <= 1;
    if (kStringFilterEmpty || strstr(method_name, kStringFilter) != nullptr) {
      return true;
    }

    return false;
  }

  HGraph* const graph_;
  size_t last_seen_graph_size_;

  std::string cached_method_name_;

  bool timing_logger_enabled_;
  TimingLogger timing_logger_;

  DisassemblyInformation disasm_info_;

  std::ostringstream visualizer_oss_;
  std::ostream* visualizer_output_;
  bool visualizer_enabled_;
  HGraphVisualizer visualizer_;
  Mutex& visualizer_dump_mutex_;

  // Flag to be set by the compiler if the pass failed and the graph is not
  // expected to validate.
  bool graph_in_bad_state_;

  friend PassScope;

  DISALLOW_COPY_AND_ASSIGN(PassObserver);
};

class PassScope : public ValueObject {
 public:
  PassScope(const char *pass_name, PassObserver* pass_observer)
      : pass_name_(pass_name),
        pass_change_(true),  // assume change
        pass_observer_(pass_observer) {
    pass_observer_->StartPass(pass_name_);
  }

  void SetPassNotChanged() {
    pass_change_ = false;
  }

  ~PassScope() {
    pass_observer_->EndPass(pass_name_, pass_change_);
  }

 private:
  const char* const pass_name_;
  bool pass_change_;
  PassObserver* const pass_observer_;
};

class OptimizingCompiler final : public Compiler {
 public:
  explicit OptimizingCompiler(const CompilerOptions& compiler_options,
                              CompiledMethodStorage* storage);
  ~OptimizingCompiler() override;

  bool CanCompileMethod(uint32_t method_idx, const DexFile& dex_file) const override;

  CompiledMethod* Compile(const dex::CodeItem* code_item,
                          uint32_t access_flags,
                          InvokeType invoke_type,
                          uint16_t class_def_idx,
                          uint32_t method_idx,
                          Handle<mirror::ClassLoader> class_loader,
                          const DexFile& dex_file,
                          Handle<mirror::DexCache> dex_cache) const override;

  CompiledMethod* JniCompile(uint32_t access_flags,
                             uint32_t method_idx,
                             const DexFile& dex_file,
                             Handle<mirror::DexCache> dex_cache) const override;

  uintptr_t GetEntryPointOf(ArtMethod* method) const override
      REQUIRES_SHARED(Locks::mutator_lock_) {
    return reinterpret_cast<uintptr_t>(method->GetEntryPointFromQuickCompiledCodePtrSize(
        InstructionSetPointerSize(GetCompilerOptions().GetInstructionSet())));
  }

  bool JitCompile(Thread* self,
                  jit::JitCodeCache* code_cache,
                  ArtMethod* method,
                  bool baseline,
                  bool osr,
                  jit::JitLogger* jit_logger)
      override
      REQUIRES_SHARED(Locks::mutator_lock_);

 private:
  bool RunOptimizations(HGraph* graph,
                        CodeGenerator* codegen,
                        const DexCompilationUnit& dex_compilation_unit,
                        PassObserver* pass_observer,
                        VariableSizedHandleScope* handles,
                        const OptimizationDef definitions[],
                        size_t length) const {
    // Convert definitions to optimization passes.
    ArenaVector<HOptimization*> optimizations = ConstructOptimizations(
        definitions,
        length,
        graph->GetAllocator(),
        graph,
        compilation_stats_.get(),
        codegen,
        dex_compilation_unit,
        handles);
    DCHECK_EQ(length, optimizations.size());
    // Run the optimization passes one by one. Any "depends_on" pass refers back to
    // the most recent occurrence of that pass, skipped or executed.
    std::bitset<static_cast<size_t>(OptimizationPass::kLast) + 1u> pass_changes;
    pass_changes[static_cast<size_t>(OptimizationPass::kNone)] = true;
    bool change = false;
    for (size_t i = 0; i < length; ++i) {
      if (pass_changes[static_cast<size_t>(definitions[i].depends_on)]) {
        // Execute the pass and record whether it changed anything.
        PassScope scope(optimizations[i]->GetPassName(), pass_observer);
        bool pass_change = optimizations[i]->Run();
        pass_changes[static_cast<size_t>(definitions[i].pass)] = pass_change;
        if (pass_change) {
          change = true;
        } else {
          scope.SetPassNotChanged();
        }
      } else {
        // Skip the pass and record that nothing changed.
        pass_changes[static_cast<size_t>(definitions[i].pass)] = false;
      }
    }
    return change;
  }

  template <size_t length> bool RunOptimizations(
      HGraph* graph,
      CodeGenerator* codegen,
      const DexCompilationUnit& dex_compilation_unit,
      PassObserver* pass_observer,
      VariableSizedHandleScope* handles,
      const OptimizationDef (&definitions)[length]) const {
    return RunOptimizations(
        graph, codegen, dex_compilation_unit, pass_observer, handles, definitions, length);
  }

  void RunOptimizations(HGraph* graph,
                        CodeGenerator* codegen,
                        const DexCompilationUnit& dex_compilation_unit,
                        PassObserver* pass_observer,
                        VariableSizedHandleScope* handles) const;

 private:
  // Create a 'CompiledMethod' for an optimized graph.
  CompiledMethod* Emit(ArenaAllocator* allocator,
                       CodeVectorAllocator* code_allocator,
                       CodeGenerator* codegen,
                       const dex::CodeItem* item) const;

  // Try compiling a method and return the code generator used for
  // compiling it.
  // This method:
  // 1) Builds the graph. Returns null if it failed to build it.
  // 2) Transforms the graph to SSA. Returns null if it failed.
  // 3) Runs optimizations on the graph, including register allocator.
  // 4) Generates code with the `code_allocator` provided.
  CodeGenerator* TryCompile(ArenaAllocator* allocator,
                            ArenaStack* arena_stack,
                            CodeVectorAllocator* code_allocator,
                            const DexCompilationUnit& dex_compilation_unit,
                            ArtMethod* method,
                            bool baseline,
                            bool osr,
                            VariableSizedHandleScope* handles) const;

  CodeGenerator* TryCompileIntrinsic(ArenaAllocator* allocator,
                                     ArenaStack* arena_stack,
                                     CodeVectorAllocator* code_allocator,
                                     const DexCompilationUnit& dex_compilation_unit,
                                     ArtMethod* method,
                                     VariableSizedHandleScope* handles) const;

  bool RunArchOptimizations(HGraph* graph,
                            CodeGenerator* codegen,
                            const DexCompilationUnit& dex_compilation_unit,
                            PassObserver* pass_observer,
                            VariableSizedHandleScope* handles) const;

  bool RunBaselineOptimizations(HGraph* graph,
                                CodeGenerator* codegen,
                                const DexCompilationUnit& dex_compilation_unit,
                                PassObserver* pass_observer,
                                VariableSizedHandleScope* handles) const;

  void GenerateJitDebugInfo(ArtMethod* method,
                            const debug::MethodDebugInfo& method_debug_info)
      REQUIRES_SHARED(Locks::mutator_lock_);

  std::unique_ptr<OptimizingCompilerStats> compilation_stats_;

  std::unique_ptr<std::ostream> visualizer_output_;

  mutable Mutex dump_mutex_;  // To synchronize visualizer writing.

  DISALLOW_COPY_AND_ASSIGN(OptimizingCompiler);
};

static const int kMaximumCompilationTimeBeforeWarning = 100; /* ms */

OptimizingCompiler::OptimizingCompiler(const CompilerOptions& compiler_options,
                                       CompiledMethodStorage* storage)
    : Compiler(compiler_options, storage, kMaximumCompilationTimeBeforeWarning),
      dump_mutex_("Visualizer dump lock") {
  // Enable C1visualizer output.
  const std::string& cfg_file_name = compiler_options.GetDumpCfgFileName();
  if (!cfg_file_name.empty()) {
    std::ios_base::openmode cfg_file_mode =
        compiler_options.GetDumpCfgAppend() ? std::ofstream::app : std::ofstream::out;
    visualizer_output_.reset(new std::ofstream(cfg_file_name, cfg_file_mode));
  }
  if (compiler_options.GetDumpStats()) {
    compilation_stats_.reset(new OptimizingCompilerStats());
  }
}

OptimizingCompiler::~OptimizingCompiler() {
  if (compilation_stats_.get() != nullptr) {
    compilation_stats_->Log();
  }
}

bool OptimizingCompiler::CanCompileMethod(uint32_t method_idx ATTRIBUTE_UNUSED,
                                          const DexFile& dex_file ATTRIBUTE_UNUSED) const {
  return true;
}

static bool IsInstructionSetSupported(InstructionSet instruction_set) {
  return instruction_set == InstructionSet::kArm
      || instruction_set == InstructionSet::kArm64
      || instruction_set == InstructionSet::kThumb2
      || instruction_set == InstructionSet::kMips
      || instruction_set == InstructionSet::kMips64
      || instruction_set == InstructionSet::kX86
      || instruction_set == InstructionSet::kX86_64;
}

bool OptimizingCompiler::RunBaselineOptimizations(HGraph* graph,
                                                  CodeGenerator* codegen,
                                                  const DexCompilationUnit& dex_compilation_unit,
                                                  PassObserver* pass_observer,
                                                  VariableSizedHandleScope* handles) const {
  switch (codegen->GetCompilerOptions().GetInstructionSet()) {
#ifdef ART_ENABLE_CODEGEN_mips
    case InstructionSet::kMips: {
      OptimizationDef mips_optimizations[] = {
        OptDef(OptimizationPass::kPcRelativeFixupsMips)
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              mips_optimizations);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case InstructionSet::kX86: {
      OptimizationDef x86_optimizations[] = {
        OptDef(OptimizationPass::kPcRelativeFixupsX86),
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              x86_optimizations);
    }
#endif
    default:
      UNUSED(graph);
      UNUSED(codegen);
      UNUSED(dex_compilation_unit);
      UNUSED(pass_observer);
      UNUSED(handles);
      return false;
  }
}

bool OptimizingCompiler::RunArchOptimizations(HGraph* graph,
                                              CodeGenerator* codegen,
                                              const DexCompilationUnit& dex_compilation_unit,
                                              PassObserver* pass_observer,
                                              VariableSizedHandleScope* handles) const {
  switch (codegen->GetCompilerOptions().GetInstructionSet()) {
#if defined(ART_ENABLE_CODEGEN_arm)
    case InstructionSet::kThumb2:
    case InstructionSet::kArm: {
      OptimizationDef arm_optimizations[] = {
        OptDef(OptimizationPass::kInstructionSimplifierArm),
        OptDef(OptimizationPass::kSideEffectsAnalysis),
        OptDef(OptimizationPass::kGlobalValueNumbering, "GVN$after_arch"),
        OptDef(OptimizationPass::kScheduling)
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              arm_optimizations);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_arm64
    case InstructionSet::kArm64: {
      OptimizationDef arm64_optimizations[] = {
        OptDef(OptimizationPass::kInstructionSimplifierArm64),
        OptDef(OptimizationPass::kSideEffectsAnalysis),
        OptDef(OptimizationPass::kGlobalValueNumbering, "GVN$after_arch"),
        OptDef(OptimizationPass::kScheduling)
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              arm64_optimizations);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_mips
    case InstructionSet::kMips: {
      OptimizationDef mips_optimizations[] = {
        OptDef(OptimizationPass::kInstructionSimplifierMips),
        OptDef(OptimizationPass::kSideEffectsAnalysis),
        OptDef(OptimizationPass::kGlobalValueNumbering, "GVN$after_arch"),
        OptDef(OptimizationPass::kPcRelativeFixupsMips)
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              mips_optimizations);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_mips64
    case InstructionSet::kMips64: {
      OptimizationDef mips64_optimizations[] = {
        OptDef(OptimizationPass::kSideEffectsAnalysis),
        OptDef(OptimizationPass::kGlobalValueNumbering, "GVN$after_arch")
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              mips64_optimizations);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86
    case InstructionSet::kX86: {
      OptimizationDef x86_optimizations[] = {
        OptDef(OptimizationPass::kInstructionSimplifierX86),
        OptDef(OptimizationPass::kSideEffectsAnalysis),
        OptDef(OptimizationPass::kGlobalValueNumbering, "GVN$after_arch"),
        OptDef(OptimizationPass::kPcRelativeFixupsX86),
        OptDef(OptimizationPass::kX86MemoryOperandGeneration)
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              x86_optimizations);
    }
#endif
#ifdef ART_ENABLE_CODEGEN_x86_64
    case InstructionSet::kX86_64: {
      OptimizationDef x86_64_optimizations[] = {
        OptDef(OptimizationPass::kInstructionSimplifierX86_64),
        OptDef(OptimizationPass::kSideEffectsAnalysis),
        OptDef(OptimizationPass::kGlobalValueNumbering, "GVN$after_arch"),
        OptDef(OptimizationPass::kX86MemoryOperandGeneration)
      };
      return RunOptimizations(graph,
                              codegen,
                              dex_compilation_unit,
                              pass_observer,
                              handles,
                              x86_64_optimizations);
    }
#endif
    default:
      return false;
  }
}

NO_INLINE  // Avoid increasing caller's frame size by large stack-allocated objects.
static void AllocateRegisters(HGraph* graph,
                              CodeGenerator* codegen,
                              PassObserver* pass_observer,
                              RegisterAllocator::Strategy strategy,
                              OptimizingCompilerStats* stats) {
  {
    PassScope scope(PrepareForRegisterAllocation::kPrepareForRegisterAllocationPassName,
                    pass_observer);
    PrepareForRegisterAllocation(graph, codegen->GetCompilerOptions(), stats).Run();
  }
  // Use local allocator shared by SSA liveness analysis and register allocator.
  // (Register allocator creates new objects in the liveness data.)
  ScopedArenaAllocator local_allocator(graph->GetArenaStack());
  SsaLivenessAnalysis liveness(graph, codegen, &local_allocator);
  {
    PassScope scope(SsaLivenessAnalysis::kLivenessPassName, pass_observer);
    liveness.Analyze();
  }
  {
    PassScope scope(RegisterAllocator::kRegisterAllocatorPassName, pass_observer);
    std::unique_ptr<RegisterAllocator> register_allocator =
        RegisterAllocator::Create(&local_allocator, codegen, liveness, strategy);
    register_allocator->AllocateRegisters();
  }
}

// Strip pass name suffix to get optimization name.
static std::string ConvertPassNameToOptimizationName(const std::string& pass_name) {
  size_t pos = pass_name.find(kPassNameSeparator);
  return pos == std::string::npos ? pass_name : pass_name.substr(0, pos);
}

void OptimizingCompiler::RunOptimizations(HGraph* graph,
                                          CodeGenerator* codegen,
                                          const DexCompilationUnit& dex_compilation_unit,
                                          PassObserver* pass_observer,
                                          VariableSizedHandleScope* handles) const {
  const std::vector<std::string>* pass_names = GetCompilerOptions().GetPassesToRun();
  if (pass_names != nullptr) {
    // If passes were defined on command-line, build the optimization
    // passes and run these instead of the built-in optimizations.
    // TODO: a way to define depends_on via command-line?
    const size_t length = pass_names->size();
    std::vector<OptimizationDef> optimizations;
    for (const std::string& pass_name : *pass_names) {
      std::string opt_name = ConvertPassNameToOptimizationName(pass_name);
      optimizations.push_back(OptDef(OptimizationPassByName(opt_name), pass_name.c_str()));
    }
    RunOptimizations(graph,
                     codegen,
                     dex_compilation_unit,
                     pass_observer,
                     handles,
                     optimizations.data(),
                     length);
    return;
  }

  OptimizationDef optimizations[] = {
    // Initial optimizations.
    OptDef(OptimizationPass::kConstantFolding),
    OptDef(OptimizationPass::kInstructionSimplifier),
    OptDef(OptimizationPass::kDeadCodeElimination,
           "dead_code_elimination$initial"),
    // Inlining.
    OptDef(OptimizationPass::kInliner),
    // Simplification (only if inlining occurred).
    OptDef(OptimizationPass::kConstantFolding,
           "constant_folding$after_inlining",
           OptimizationPass::kInliner),
    OptDef(OptimizationPass::kInstructionSimplifier,
           "instruction_simplifier$after_inlining",
           OptimizationPass::kInliner),
    OptDef(OptimizationPass::kDeadCodeElimination,
           "dead_code_elimination$after_inlining",
           OptimizationPass::kInliner),
    // GVN.
    OptDef(OptimizationPass::kSideEffectsAnalysis,
           "side_effects$before_gvn"),
    OptDef(OptimizationPass::kGlobalValueNumbering),
    // Simplification (TODO: only if GVN occurred).
    OptDef(OptimizationPass::kSelectGenerator),
    OptDef(OptimizationPass::kConstantFolding,
           "constant_folding$after_gvn"),
    OptDef(OptimizationPass::kInstructionSimplifier,
           "instruction_simplifier$after_gvn"),
    OptDef(OptimizationPass::kDeadCodeElimination,
           "dead_code_elimination$after_gvn"),
    // High-level optimizations.
    OptDef(OptimizationPass::kSideEffectsAnalysis,
           "side_effects$before_licm"),
    OptDef(OptimizationPass::kInvariantCodeMotion),
    OptDef(OptimizationPass::kInductionVarAnalysis),
    OptDef(OptimizationPass::kBoundsCheckElimination),
    OptDef(OptimizationPass::kLoopOptimization),
    // Simplification.
    OptDef(OptimizationPass::kConstantFolding,
           "constant_folding$after_bce"),
    OptDef(OptimizationPass::kAggressiveInstructionSimplifier,
           "instruction_simplifier$after_bce"),
    // Other high-level optimizations.
    OptDef(OptimizationPass::kSideEffectsAnalysis,
           "side_effects$before_lse"),
    OptDef(OptimizationPass::kLoadStoreAnalysis),
    OptDef(OptimizationPass::kLoadStoreElimination),
    OptDef(OptimizationPass::kCHAGuardOptimization),
    OptDef(OptimizationPass::kDeadCodeElimination,
           "dead_code_elimination$final"),
    OptDef(OptimizationPass::kCodeSinking),
    // The codegen has a few assumptions that only the instruction simplifier
    // can satisfy. For example, the code generator does not expect to see a
    // HTypeConversion from a type to the same type.
    OptDef(OptimizationPass::kAggressiveInstructionSimplifier,
           "instruction_simplifier$before_codegen"),
    // Eliminate constructor fences after code sinking to avoid
    // complicated sinking logic to split a fence with many inputs.
    OptDef(OptimizationPass::kConstructorFenceRedundancyElimination)
  };
  RunOptimizations(graph,
                   codegen,
                   dex_compilation_unit,
                   pass_observer,
                   handles,
                   optimizations);

  RunArchOptimizations(graph, codegen, dex_compilation_unit, pass_observer, handles);
}

static ArenaVector<linker::LinkerPatch> EmitAndSortLinkerPatches(CodeGenerator* codegen) {
  ArenaVector<linker::LinkerPatch> linker_patches(codegen->GetGraph()->GetAllocator()->Adapter());
  codegen->EmitLinkerPatches(&linker_patches);

  // Sort patches by literal offset. Required for .oat_patches encoding.
  std::sort(linker_patches.begin(), linker_patches.end(),
            [](const linker::LinkerPatch& lhs, const linker::LinkerPatch& rhs) {
    return lhs.LiteralOffset() < rhs.LiteralOffset();
  });

  return linker_patches;
}

CompiledMethod* OptimizingCompiler::Emit(ArenaAllocator* allocator,
                                         CodeVectorAllocator* code_allocator,
                                         CodeGenerator* codegen,
                                         const dex::CodeItem* code_item_for_osr_check) const {
  ArenaVector<linker::LinkerPatch> linker_patches = EmitAndSortLinkerPatches(codegen);
  ScopedArenaVector<uint8_t> stack_map = codegen->BuildStackMaps(code_item_for_osr_check);

  CompiledMethodStorage* storage = GetCompiledMethodStorage();
  CompiledMethod* compiled_method = CompiledMethod::SwapAllocCompiledMethod(
      storage,
      codegen->GetInstructionSet(),
      code_allocator->GetMemory(),
      ArrayRef<const uint8_t>(stack_map),
      ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data()),
      ArrayRef<const linker::LinkerPatch>(linker_patches));

  for (const linker::LinkerPatch& patch : linker_patches) {
    if (codegen->NeedsThunkCode(patch) && storage->GetThunkCode(patch).empty()) {
      ArenaVector<uint8_t> code(allocator->Adapter());
      std::string debug_name;
      codegen->EmitThunkCode(patch, &code, &debug_name);
      storage->SetThunkCode(patch, ArrayRef<const uint8_t>(code), debug_name);
    }
  }

  return compiled_method;
}

CodeGenerator* OptimizingCompiler::TryCompile(ArenaAllocator* allocator,
                                              ArenaStack* arena_stack,
                                              CodeVectorAllocator* code_allocator,
                                              const DexCompilationUnit& dex_compilation_unit,
                                              ArtMethod* method,
                                              bool baseline,
                                              bool osr,
                                              VariableSizedHandleScope* handles) const {
  MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kAttemptBytecodeCompilation);
  const CompilerOptions& compiler_options = GetCompilerOptions();
  InstructionSet instruction_set = compiler_options.GetInstructionSet();
  const DexFile& dex_file = *dex_compilation_unit.GetDexFile();
  uint32_t method_idx = dex_compilation_unit.GetDexMethodIndex();
  const dex::CodeItem* code_item = dex_compilation_unit.GetCodeItem();

  // Always use the Thumb-2 assembler: some runtime functionality
  // (like implicit stack overflow checks) assume Thumb-2.
  DCHECK_NE(instruction_set, InstructionSet::kArm);

  // Do not attempt to compile on architectures we do not support.
  if (!IsInstructionSetSupported(instruction_set)) {
    MaybeRecordStat(compilation_stats_.get(),
                    MethodCompilationStat::kNotCompiledUnsupportedIsa);
    return nullptr;
  }

  if (Compiler::IsPathologicalCase(*code_item, method_idx, dex_file)) {
    MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kNotCompiledPathological);
    return nullptr;
  }

  // Implementation of the space filter: do not compile a code item whose size in
  // code units is bigger than 128.
  static constexpr size_t kSpaceFilterOptimizingThreshold = 128;
  if ((compiler_options.GetCompilerFilter() == CompilerFilter::kSpace)
      && (CodeItemInstructionAccessor(dex_file, code_item).InsnsSizeInCodeUnits() >
          kSpaceFilterOptimizingThreshold)) {
    MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kNotCompiledSpaceFilter);
    return nullptr;
  }

  CodeItemDebugInfoAccessor code_item_accessor(dex_file, code_item, method_idx);

  bool dead_reference_safe;
  ArrayRef<const uint8_t> interpreter_metadata;
  // For AOT compilation, we may not get a method, for example if its class is erroneous,
  // possibly due to an unavailable superclass.  JIT should always have a method.
  DCHECK(Runtime::Current()->IsAotCompiler() || method != nullptr);
  if (method != nullptr) {
    const dex::ClassDef* containing_class;
    {
      ScopedObjectAccess soa(Thread::Current());
      containing_class = &method->GetClassDef();
      interpreter_metadata = method->GetQuickenedInfo();
    }
    // MethodContainsRSensitiveAccess is currently slow, but HasDeadReferenceSafeAnnotation()
    // is currently rarely true.
    dead_reference_safe =
        annotations::HasDeadReferenceSafeAnnotation(dex_file, *containing_class)
        && !annotations::MethodContainsRSensitiveAccess(dex_file, *containing_class, method_idx);
  } else {
    // If we could not resolve the class, conservatively assume it's dead-reference unsafe.
    dead_reference_safe = false;
  }

  HGraph* graph = new (allocator) HGraph(
      allocator,
      arena_stack,
      dex_file,
      method_idx,
      compiler_options.GetInstructionSet(),
      kInvalidInvokeType,
      dead_reference_safe,
      compiler_options.GetDebuggable(),
      /* osr= */ osr);

  if (method != nullptr) {
    graph->SetArtMethod(method);
  }

  std::unique_ptr<CodeGenerator> codegen(
      CodeGenerator::Create(graph,
                            compiler_options,
                            compilation_stats_.get()));
  if (codegen.get() == nullptr) {
    MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kNotCompiledNoCodegen);
    return nullptr;
  }
  codegen->GetAssembler()->cfi().SetEnabled(compiler_options.GenerateAnyDebugInfo());

  PassObserver pass_observer(graph,
                             codegen.get(),
                             visualizer_output_.get(),
                             compiler_options,
                             dump_mutex_);

  {
    VLOG(compiler) << "Building " << pass_observer.GetMethodName();
    PassScope scope(HGraphBuilder::kBuilderPassName, &pass_observer);
    HGraphBuilder builder(graph,
                          code_item_accessor,
                          &dex_compilation_unit,
                          &dex_compilation_unit,
                          codegen.get(),
                          compilation_stats_.get(),
                          interpreter_metadata,
                          handles);
    GraphAnalysisResult result = builder.BuildGraph();
    if (result != kAnalysisSuccess) {
      switch (result) {
        case kAnalysisSkipped: {
          MaybeRecordStat(compilation_stats_.get(),
                          MethodCompilationStat::kNotCompiledSkipped);
          break;
        }
        case kAnalysisInvalidBytecode: {
          MaybeRecordStat(compilation_stats_.get(),
                          MethodCompilationStat::kNotCompiledInvalidBytecode);
          break;
        }
        case kAnalysisFailThrowCatchLoop: {
          MaybeRecordStat(compilation_stats_.get(),
                          MethodCompilationStat::kNotCompiledThrowCatchLoop);
          break;
        }
        case kAnalysisFailAmbiguousArrayOp: {
          MaybeRecordStat(compilation_stats_.get(),
                          MethodCompilationStat::kNotCompiledAmbiguousArrayOp);
          break;
        }
        case kAnalysisFailIrreducibleLoopAndStringInit: {
          MaybeRecordStat(compilation_stats_.get(),
                          MethodCompilationStat::kNotCompiledIrreducibleLoopAndStringInit);
          break;
        }
        case kAnalysisFailPhiEquivalentInOsr: {
          MaybeRecordStat(compilation_stats_.get(),
                          MethodCompilationStat::kNotCompiledPhiEquivalentInOsr);
          break;
        }
        case kAnalysisSuccess:
          UNREACHABLE();
      }
      pass_observer.SetGraphInBadState();
      return nullptr;
    }
  }

  if (baseline) {
    RunBaselineOptimizations(graph, codegen.get(), dex_compilation_unit, &pass_observer, handles);
  } else {
    RunOptimizations(graph, codegen.get(), dex_compilation_unit, &pass_observer, handles);
  }

  RegisterAllocator::Strategy regalloc_strategy =
    compiler_options.GetRegisterAllocationStrategy();
  AllocateRegisters(graph,
                    codegen.get(),
                    &pass_observer,
                    regalloc_strategy,
                    compilation_stats_.get());

  codegen->Compile(code_allocator);
  pass_observer.DumpDisassembly();

  MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kCompiledBytecode);
  return codegen.release();
}

CodeGenerator* OptimizingCompiler::TryCompileIntrinsic(
    ArenaAllocator* allocator,
    ArenaStack* arena_stack,
    CodeVectorAllocator* code_allocator,
    const DexCompilationUnit& dex_compilation_unit,
    ArtMethod* method,
    VariableSizedHandleScope* handles) const {
  MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kAttemptIntrinsicCompilation);
  const CompilerOptions& compiler_options = GetCompilerOptions();
  InstructionSet instruction_set = compiler_options.GetInstructionSet();
  const DexFile& dex_file = *dex_compilation_unit.GetDexFile();
  uint32_t method_idx = dex_compilation_unit.GetDexMethodIndex();

  // Always use the Thumb-2 assembler: some runtime functionality
  // (like implicit stack overflow checks) assume Thumb-2.
  DCHECK_NE(instruction_set, InstructionSet::kArm);

  // Do not attempt to compile on architectures we do not support.
  if (!IsInstructionSetSupported(instruction_set)) {
    return nullptr;
  }

  HGraph* graph = new (allocator) HGraph(
      allocator,
      arena_stack,
      dex_file,
      method_idx,
      compiler_options.GetInstructionSet(),
      kInvalidInvokeType,
      /* dead_reference_safe= */ true,  // Intrinsics don't affect dead reference safety.
      compiler_options.GetDebuggable(),
      /* osr= */ false);

  DCHECK(Runtime::Current()->IsAotCompiler());
  DCHECK(method != nullptr);
  graph->SetArtMethod(method);

  std::unique_ptr<CodeGenerator> codegen(
      CodeGenerator::Create(graph,
                            compiler_options,
                            compilation_stats_.get()));
  if (codegen.get() == nullptr) {
    return nullptr;
  }
  codegen->GetAssembler()->cfi().SetEnabled(compiler_options.GenerateAnyDebugInfo());

  PassObserver pass_observer(graph,
                             codegen.get(),
                             visualizer_output_.get(),
                             compiler_options,
                             dump_mutex_);

  {
    VLOG(compiler) << "Building intrinsic graph " << pass_observer.GetMethodName();
    PassScope scope(HGraphBuilder::kBuilderPassName, &pass_observer);
    HGraphBuilder builder(graph,
                          CodeItemDebugInfoAccessor(),  // Null code item.
                          &dex_compilation_unit,
                          &dex_compilation_unit,
                          codegen.get(),
                          compilation_stats_.get(),
                          /* interpreter_metadata= */ ArrayRef<const uint8_t>(),
                          handles);
    builder.BuildIntrinsicGraph(method);
  }

  OptimizationDef optimizations[] = {
    // The codegen has a few assumptions that only the instruction simplifier
    // can satisfy.
    OptDef(OptimizationPass::kInstructionSimplifier),
  };
  RunOptimizations(graph,
                   codegen.get(),
                   dex_compilation_unit,
                   &pass_observer,
                   handles,
                   optimizations);

  RunArchOptimizations(graph, codegen.get(), dex_compilation_unit, &pass_observer, handles);

  AllocateRegisters(graph,
                    codegen.get(),
                    &pass_observer,
                    compiler_options.GetRegisterAllocationStrategy(),
                    compilation_stats_.get());
  if (!codegen->IsLeafMethod()) {
    VLOG(compiler) << "Intrinsic method is not leaf: " << method->GetIntrinsic()
        << " " << graph->PrettyMethod();
    return nullptr;
  }

  codegen->Compile(code_allocator);
  pass_observer.DumpDisassembly();

  VLOG(compiler) << "Compiled intrinsic: " << method->GetIntrinsic()
      << " " << graph->PrettyMethod();
  MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kCompiledIntrinsic);
  return codegen.release();
}

CompiledMethod* OptimizingCompiler::Compile(const dex::CodeItem* code_item,
                                            uint32_t access_flags,
                                            InvokeType invoke_type,
                                            uint16_t class_def_idx,
                                            uint32_t method_idx,
                                            Handle<mirror::ClassLoader> jclass_loader,
                                            const DexFile& dex_file,
                                            Handle<mirror::DexCache> dex_cache) const {
  const CompilerOptions& compiler_options = GetCompilerOptions();
  CompiledMethod* compiled_method = nullptr;
  Runtime* runtime = Runtime::Current();
  DCHECK(runtime->IsAotCompiler());
  const VerifiedMethod* verified_method = compiler_options.GetVerifiedMethod(&dex_file, method_idx);
  DCHECK(!verified_method->HasRuntimeThrow());
  if (compiler_options.IsMethodVerifiedWithoutFailures(method_idx, class_def_idx, dex_file) ||
      verifier::CanCompilerHandleVerificationFailure(
          verified_method->GetEncounteredVerificationFailures())) {
    ArenaAllocator allocator(runtime->GetArenaPool());
    ArenaStack arena_stack(runtime->GetArenaPool());
    CodeVectorAllocator code_allocator(&allocator);
    std::unique_ptr<CodeGenerator> codegen;
    bool compiled_intrinsic = false;
    {
      ScopedObjectAccess soa(Thread::Current());
      ArtMethod* method =
          runtime->GetClassLinker()->ResolveMethod<ClassLinker::ResolveMode::kCheckICCEAndIAE>(
              method_idx, dex_cache, jclass_loader, /*referrer=*/ nullptr, invoke_type);
      DCHECK_EQ(method == nullptr, soa.Self()->IsExceptionPending());
      soa.Self()->ClearException();  // Suppress exception if any.
      VariableSizedHandleScope handles(soa.Self());
      Handle<mirror::Class> compiling_class =
          handles.NewHandle(method != nullptr ? method->GetDeclaringClass() : nullptr);
      DexCompilationUnit dex_compilation_unit(
          jclass_loader,
          runtime->GetClassLinker(),
          dex_file,
          code_item,
          class_def_idx,
          method_idx,
          access_flags,
          /*verified_method=*/ nullptr,  // Not needed by the Optimizing compiler.
          dex_cache,
          compiling_class);
      // Go to native so that we don't block GC during compilation.
      ScopedThreadSuspension sts(soa.Self(), kNative);
      if (method != nullptr && UNLIKELY(method->IsIntrinsic())) {
        DCHECK(compiler_options.IsBootImage());
        codegen.reset(
            TryCompileIntrinsic(&allocator,
                                &arena_stack,
                                &code_allocator,
                                dex_compilation_unit,
                                method,
                                &handles));
        if (codegen != nullptr) {
          compiled_intrinsic = true;
        }
      }
      if (codegen == nullptr) {
        codegen.reset(
            TryCompile(&allocator,
                       &arena_stack,
                       &code_allocator,
                       dex_compilation_unit,
                       method,
                       compiler_options.IsBaseline(),
                       /* osr= */ false,
                       &handles));
      }
    }
    if (codegen.get() != nullptr) {
      compiled_method = Emit(&allocator,
                             &code_allocator,
                             codegen.get(),
                             compiled_intrinsic ? nullptr : code_item);
      if (compiled_intrinsic) {
        compiled_method->MarkAsIntrinsic();
      }

      if (kArenaAllocatorCountAllocations) {
        codegen.reset();  // Release codegen's ScopedArenaAllocator for memory accounting.
        size_t total_allocated = allocator.BytesAllocated() + arena_stack.PeakBytesAllocated();
        if (total_allocated > kArenaAllocatorMemoryReportThreshold) {
          MemStats mem_stats(allocator.GetMemStats());
          MemStats peak_stats(arena_stack.GetPeakStats());
          LOG(INFO) << "Used " << total_allocated << " bytes of arena memory for compiling "
                    << dex_file.PrettyMethod(method_idx)
                    << "\n" << Dumpable<MemStats>(mem_stats)
                    << "\n" << Dumpable<MemStats>(peak_stats);
        }
      }
    }
  } else {
    MethodCompilationStat method_stat;
    if (compiler_options.VerifyAtRuntime()) {
      method_stat = MethodCompilationStat::kNotCompiledVerifyAtRuntime;
    } else {
      method_stat = MethodCompilationStat::kNotCompiledVerificationError;
    }
    MaybeRecordStat(compilation_stats_.get(), method_stat);
  }

  if (kIsDebugBuild &&
      compiler_options.CompilingWithCoreImage() &&
      IsInstructionSetSupported(compiler_options.GetInstructionSet())) {
    // For testing purposes, we put a special marker on method names
    // that should be compiled with this compiler (when the
    // instruction set is supported). This makes sure we're not
    // regressing.
    std::string method_name = dex_file.PrettyMethod(method_idx);
    bool shouldCompile = method_name.find("$opt$") != std::string::npos;
    DCHECK((compiled_method != nullptr) || !shouldCompile) << "Didn't compile " << method_name;
  }

  return compiled_method;
}

static ScopedArenaVector<uint8_t> CreateJniStackMap(ScopedArenaAllocator* allocator,
                                                    const JniCompiledMethod& jni_compiled_method) {
  // StackMapStream is quite large, so allocate it using the ScopedArenaAllocator
  // to stay clear of the frame size limit.
  std::unique_ptr<StackMapStream> stack_map_stream(
      new (allocator) StackMapStream(allocator, jni_compiled_method.GetInstructionSet()));
  stack_map_stream->BeginMethod(
      jni_compiled_method.GetFrameSize(),
      jni_compiled_method.GetCoreSpillMask(),
      jni_compiled_method.GetFpSpillMask(),
      /* num_dex_registers= */ 0);
  stack_map_stream->EndMethod();
  return stack_map_stream->Encode();
}

CompiledMethod* OptimizingCompiler::JniCompile(uint32_t access_flags,
                                               uint32_t method_idx,
                                               const DexFile& dex_file,
                                               Handle<mirror::DexCache> dex_cache) const {
  Runtime* runtime = Runtime::Current();
  ArenaAllocator allocator(runtime->GetArenaPool());
  ArenaStack arena_stack(runtime->GetArenaPool());

  const CompilerOptions& compiler_options = GetCompilerOptions();
  if (compiler_options.IsBootImage()) {
    ScopedObjectAccess soa(Thread::Current());
    ArtMethod* method = runtime->GetClassLinker()->LookupResolvedMethod(
        method_idx, dex_cache.Get(), /*class_loader=*/ nullptr);
    if (method != nullptr && UNLIKELY(method->IsIntrinsic())) {
      VariableSizedHandleScope handles(soa.Self());
      ScopedNullHandle<mirror::ClassLoader> class_loader;  // null means boot class path loader.
      Handle<mirror::Class> compiling_class = handles.NewHandle(method->GetDeclaringClass());
      DexCompilationUnit dex_compilation_unit(
          class_loader,
          runtime->GetClassLinker(),
          dex_file,
          /*code_item=*/ nullptr,
          /*class_def_idx=*/ DexFile::kDexNoIndex16,
          method_idx,
          access_flags,
          /*verified_method=*/ nullptr,
          dex_cache,
          compiling_class);
      CodeVectorAllocator code_allocator(&allocator);
      // Go to native so that we don't block GC during compilation.
      ScopedThreadSuspension sts(soa.Self(), kNative);
      std::unique_ptr<CodeGenerator> codegen(
          TryCompileIntrinsic(&allocator,
                              &arena_stack,
                              &code_allocator,
                              dex_compilation_unit,
                              method,
                              &handles));
      if (codegen != nullptr) {
        CompiledMethod* compiled_method = Emit(&allocator,
                                               &code_allocator,
                                               codegen.get(),
                                               /* item= */ nullptr);
        compiled_method->MarkAsIntrinsic();
        return compiled_method;
      }
    }
  }

  JniCompiledMethod jni_compiled_method = ArtQuickJniCompileMethod(
      compiler_options, access_flags, method_idx, dex_file);
  MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kCompiledNativeStub);

  ScopedArenaAllocator stack_map_allocator(&arena_stack);  // Will hold the stack map.
  ScopedArenaVector<uint8_t> stack_map = CreateJniStackMap(&stack_map_allocator,
                                                           jni_compiled_method);
  return CompiledMethod::SwapAllocCompiledMethod(
      GetCompiledMethodStorage(),
      jni_compiled_method.GetInstructionSet(),
      jni_compiled_method.GetCode(),
      ArrayRef<const uint8_t>(stack_map),
      jni_compiled_method.GetCfi(),
      /* patches= */ ArrayRef<const linker::LinkerPatch>());
}

Compiler* CreateOptimizingCompiler(const CompilerOptions& compiler_options,
                                   CompiledMethodStorage* storage) {
  return new OptimizingCompiler(compiler_options, storage);
}

bool EncodeArtMethodInInlineInfo(ArtMethod* method ATTRIBUTE_UNUSED) {
  // Note: the runtime is null only for unit testing.
  return Runtime::Current() == nullptr || !Runtime::Current()->IsAotCompiler();
}

bool OptimizingCompiler::JitCompile(Thread* self,
                                    jit::JitCodeCache* code_cache,
                                    ArtMethod* method,
                                    bool baseline,
                                    bool osr,
                                    jit::JitLogger* jit_logger) {
  StackHandleScope<3> hs(self);
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(
      method->GetDeclaringClass()->GetClassLoader()));
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(method->GetDexCache()));
  DCHECK(method->IsCompilable());

  const DexFile* dex_file = method->GetDexFile();
  const uint16_t class_def_idx = method->GetClassDefIndex();
  const dex::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());
  const uint32_t method_idx = method->GetDexMethodIndex();
  const uint32_t access_flags = method->GetAccessFlags();

  Runtime* runtime = Runtime::Current();
  ArenaAllocator allocator(runtime->GetJitArenaPool());

  if (UNLIKELY(method->IsNative())) {
    const CompilerOptions& compiler_options = GetCompilerOptions();
    JniCompiledMethod jni_compiled_method = ArtQuickJniCompileMethod(
        compiler_options, access_flags, method_idx, *dex_file);
    std::vector<Handle<mirror::Object>> roots;
    ArenaSet<ArtMethod*, std::less<ArtMethod*>> cha_single_implementation_list(
        allocator.Adapter(kArenaAllocCHA));
    ArenaStack arena_stack(runtime->GetJitArenaPool());
    // StackMapStream is large and it does not fit into this frame, so we need helper method.
    ScopedArenaAllocator stack_map_allocator(&arena_stack);  // Will hold the stack map.
    ScopedArenaVector<uint8_t> stack_map = CreateJniStackMap(&stack_map_allocator,
                                                             jni_compiled_method);
    uint8_t* stack_map_data = nullptr;
    uint8_t* roots_data = nullptr;
    uint32_t data_size = code_cache->ReserveData(self,
                                                 stack_map.size(),
                                                 /* number_of_roots= */ 0,
                                                 method,
                                                 &stack_map_data,
                                                 &roots_data);
    if (stack_map_data == nullptr || roots_data == nullptr) {
      MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kJitOutOfMemoryForCommit);
      return false;
    }
    memcpy(stack_map_data, stack_map.data(), stack_map.size());

    const void* code = code_cache->CommitCode(
        self,
        method,
        stack_map_data,
        roots_data,
        jni_compiled_method.GetCode().data(),
        jni_compiled_method.GetCode().size(),
        data_size,
        osr,
        roots,
        /* has_should_deoptimize_flag= */ false,
        cha_single_implementation_list);
    if (code == nullptr) {
      return false;
    }

    if (compiler_options.GenerateAnyDebugInfo()) {
      const auto* method_header = reinterpret_cast<const OatQuickMethodHeader*>(code);
      const uintptr_t code_address = reinterpret_cast<uintptr_t>(method_header->GetCode());
      debug::MethodDebugInfo info = {};
      info.custom_name = "art_jni_trampoline";
      info.dex_file = dex_file;
      info.class_def_index = class_def_idx;
      info.dex_method_index = method_idx;
      info.access_flags = access_flags;
      info.code_item = code_item;
      info.isa = jni_compiled_method.GetInstructionSet();
      info.deduped = false;
      info.is_native_debuggable = compiler_options.GetNativeDebuggable();
      info.is_optimized = true;
      info.is_code_address_text_relative = false;
      info.code_address = code_address;
      info.code_size = jni_compiled_method.GetCode().size();
      info.frame_size_in_bytes = method_header->GetFrameSizeInBytes();
      info.code_info = nullptr;
      info.cfi = jni_compiled_method.GetCfi();
      GenerateJitDebugInfo(method, info);
    }

    Runtime::Current()->GetJit()->AddMemoryUsage(method, allocator.BytesUsed());
    if (jit_logger != nullptr) {
      jit_logger->WriteLog(code, jni_compiled_method.GetCode().size(), method);
    }
    return true;
  }

  ArenaStack arena_stack(runtime->GetJitArenaPool());
  CodeVectorAllocator code_allocator(&allocator);
  VariableSizedHandleScope handles(self);

  std::unique_ptr<CodeGenerator> codegen;
  {
    Handle<mirror::Class> compiling_class = handles.NewHandle(method->GetDeclaringClass());
    DexCompilationUnit dex_compilation_unit(
        class_loader,
        runtime->GetClassLinker(),
        *dex_file,
        code_item,
        class_def_idx,
        method_idx,
        access_flags,
        /*verified_method=*/ nullptr,
        dex_cache,
        compiling_class);

    // Go to native so that we don't block GC during compilation.
    ScopedThreadSuspension sts(self, kNative);
    codegen.reset(
        TryCompile(&allocator,
                   &arena_stack,
                   &code_allocator,
                   dex_compilation_unit,
                   method,
                   baseline,
                   osr,
                   &handles));
    if (codegen.get() == nullptr) {
      return false;
    }
  }

  ScopedArenaVector<uint8_t> stack_map = codegen->BuildStackMaps(code_item);
  size_t number_of_roots = codegen->GetNumberOfJitRoots();
  uint8_t* stack_map_data = nullptr;
  uint8_t* roots_data = nullptr;
  uint32_t data_size = code_cache->ReserveData(self,
                                               stack_map.size(),
                                               number_of_roots,
                                               method,
                                               &stack_map_data,
                                               &roots_data);
  if (stack_map_data == nullptr || roots_data == nullptr) {
    MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kJitOutOfMemoryForCommit);
    return false;
  }
  memcpy(stack_map_data, stack_map.data(), stack_map.size());
  std::vector<Handle<mirror::Object>> roots;
  codegen->EmitJitRoots(code_allocator.GetData(), roots_data, &roots);
  // The root Handle<>s filled by the codegen reference entries in the VariableSizedHandleScope.
  DCHECK(std::all_of(roots.begin(),
                     roots.end(),
                     [&handles](Handle<mirror::Object> root){
                       return handles.Contains(root.GetReference());
                     }));

  const void* code = code_cache->CommitCode(
      self,
      method,
      stack_map_data,
      roots_data,
      code_allocator.GetMemory().data(),
      code_allocator.GetMemory().size(),
      data_size,
      osr,
      roots,
      codegen->GetGraph()->HasShouldDeoptimizeFlag(),
      codegen->GetGraph()->GetCHASingleImplementationList());

  if (code == nullptr) {
    MaybeRecordStat(compilation_stats_.get(), MethodCompilationStat::kJitOutOfMemoryForCommit);
    code_cache->ClearData(self, stack_map_data, roots_data);
    return false;
  }

  const CompilerOptions& compiler_options = GetCompilerOptions();
  if (compiler_options.GenerateAnyDebugInfo()) {
    const auto* method_header = reinterpret_cast<const OatQuickMethodHeader*>(code);
    const uintptr_t code_address = reinterpret_cast<uintptr_t>(method_header->GetCode());
    debug::MethodDebugInfo info = {};
    DCHECK(info.custom_name.empty());
    info.dex_file = dex_file;
    info.class_def_index = class_def_idx;
    info.dex_method_index = method_idx;
    info.access_flags = access_flags;
    info.code_item = code_item;
    info.isa = codegen->GetInstructionSet();
    info.deduped = false;
    info.is_native_debuggable = compiler_options.GetNativeDebuggable();
    info.is_optimized = true;
    info.is_code_address_text_relative = false;
    info.code_address = code_address;
    info.code_size = code_allocator.GetMemory().size();
    info.frame_size_in_bytes = method_header->GetFrameSizeInBytes();
    info.code_info = stack_map.size() == 0 ? nullptr : stack_map_data;
    info.cfi = ArrayRef<const uint8_t>(*codegen->GetAssembler()->cfi().data());
    GenerateJitDebugInfo(method, info);
  }

  Runtime::Current()->GetJit()->AddMemoryUsage(method, allocator.BytesUsed());
  if (jit_logger != nullptr) {
    jit_logger->WriteLog(code, code_allocator.GetMemory().size(), method);
  }

  if (kArenaAllocatorCountAllocations) {
    codegen.reset();  // Release codegen's ScopedArenaAllocator for memory accounting.
    size_t total_allocated = allocator.BytesAllocated() + arena_stack.PeakBytesAllocated();
    if (total_allocated > kArenaAllocatorMemoryReportThreshold) {
      MemStats mem_stats(allocator.GetMemStats());
      MemStats peak_stats(arena_stack.GetPeakStats());
      LOG(INFO) << "Used " << total_allocated << " bytes of arena memory for compiling "
                << dex_file->PrettyMethod(method_idx)
                << "\n" << Dumpable<MemStats>(mem_stats)
                << "\n" << Dumpable<MemStats>(peak_stats);
    }
  }

  return true;
}

void OptimizingCompiler::GenerateJitDebugInfo(ArtMethod* method ATTRIBUTE_UNUSED,
                                              const debug::MethodDebugInfo& info) {
  const CompilerOptions& compiler_options = GetCompilerOptions();
  DCHECK(compiler_options.GenerateAnyDebugInfo());
  TimingLogger logger("Generate JIT debug info logger", true, VLOG_IS_ON(jit));
  {
    TimingLogger::ScopedTiming st("Generate JIT debug info", &logger);

    // If both flags are passed, generate full debug info.
    const bool mini_debug_info = !compiler_options.GetGenerateDebugInfo();

    // Create entry for the single method that we just compiled.
    std::vector<uint8_t> elf_file = debug::MakeElfFileForJIT(
        compiler_options.GetInstructionSet(),
        compiler_options.GetInstructionSetFeatures(),
        mini_debug_info,
        info);
    AddNativeDebugInfoForJit(Thread::Current(),
                             reinterpret_cast<const void*>(info.code_address),
                             elf_file,
                             debug::PackElfFileForJIT,
                             compiler_options.GetInstructionSet(),
                             compiler_options.GetInstructionSetFeatures());
  }
  Runtime::Current()->GetJit()->AddTimingLogger(logger);
}

}  // namespace art
