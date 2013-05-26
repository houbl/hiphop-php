/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010- Facebook, Inc. (http://www.facebook.com)         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/
#ifndef incl_HPHP_RUNTIME_VM_TRANSLATOR_X64_H_
#define incl_HPHP_RUNTIME_VM_TRANSLATOR_X64_H_

#include <signal.h>
#include <memory>
#include <boost/noncopyable.hpp>

#include "hphp/runtime/vm/bytecode.h"
#include "hphp/runtime/vm/jit/translator.h"
#include "hphp/util/asm-x64.h"
#include "hphp/runtime/vm/jit/srcdb.h"
#include "hphp/runtime/vm/jit/unwind-x64.h"
#include "tbb/concurrent_hash_map.h"
#include "hphp/util/ringbuffer.h"
#include "hphp/runtime/vm/debug/debug.h"
#include "hphp/runtime/vm/jit/abi-x64.h"

namespace HPHP { class ExecutionContext; }

namespace HPHP {  namespace JIT {
class HhbcTranslator;
class IRFactory;
class CSEHash;
class TraceBuilder;
class CodeGenerator;
}}

namespace HPHP { namespace Transl {

class IRTranslator;
class MVecTransState;

struct TraceletCounters {
  uint64_t m_numEntered, m_numExecuted;
};

struct TraceletCountersVec {
  int64_t m_size;
  TraceletCounters *m_elms;
  Mutex m_lock;

  TraceletCountersVec() : m_size(0), m_elms(nullptr), m_lock() { }
};

struct FreeStubList {
  struct StubNode {
    StubNode* m_next;
    uint64_t  m_freed;
  };
  static const uint64_t kStubFree = 0;
  StubNode* m_list;
  FreeStubList() : m_list(nullptr) {}
  TCA maybePop();
  void push(TCA stub);
};

struct Call {
  explicit Call(void *p) : m_kind(Direct), m_fptr(p) {}
  explicit Call(int off) : m_kind(Virtual), m_offset(off) {}
  Call(Call const&) = default;

  bool isDirect()  const { return m_kind == Direct;  }
  bool isVirtual() const { return m_kind == Virtual; }

  const void* getAddress() const { return m_fptr; }
  int         getOffset()  const { return m_offset; }

 private:
  enum { Direct, Virtual } m_kind;
  union {
    void* m_fptr;
    int   m_offset;
  };
};

class TranslatorX64;
extern __thread TranslatorX64* tx64;

extern void* interpOneEntryPoints[];

extern "C" TCA funcBodyHelper(ActRec* fp);

struct TReqInfo;
struct Label;

static const int kNumFreeLocalsHelpers = 9;

typedef X64Assembler Asm;

constexpr size_t kJmpTargetAlign = 16;
constexpr size_t kNonFallthroughAlign = 64;
constexpr int kJmpLen = 5;
constexpr int kCallLen = 5;
constexpr int kJmpccLen = 6;
constexpr int kJmpImmBytes = 4;
constexpr int kJcc8Len = 3;
constexpr int kLeaRipLen = 7;
constexpr int kTestRegRegLen = 3;
constexpr int kTestImmRegLen = 5;  // only for rax -- special encoding
// Cache alignment is required for mutable instructions to make sure
// mutations don't "tear" on remote cpus.
constexpr size_t kX64CacheLineSize = 64;
constexpr size_t kX64CacheLineMask = kX64CacheLineSize - 1;

enum TestAndSmashFlags {
  kAlignJccImmediate,
  kAlignJcc,
  kAlignJccAndJmp
};
void prepareForTestAndSmash(Asm&, int testBytes, TestAndSmashFlags flags);
void prepareForSmash(Asm&, int nBytes, int offset = 0);
bool isSmashable(Address frontier, int nBytes, int offset = 0);

class TranslatorX64 : public Translator
                    , boost::noncopyable {
  friend class SrcRec; // so it can smash code.
  friend class SrcDB;  // For write lock and code invalidation.
  friend class WithCounters;
  friend class DiamondGuard;
  friend class DiamondReturn;
  friend class RedirectSpillFill;
  friend class Tx64Reaper;
  friend class IRTranslator;
  friend class HPHP::JIT::CodeGenerator;
  friend class HPHP::JIT::HhbcTranslator; // packBitVec()
  friend TCA funcBodyHelper(ActRec* fp);
  template<unsigned, unsigned, ConditionCode, class> friend class CondBlock;
  template<ConditionCode, typename smasher> friend class JccBlock;
  template<ConditionCode> friend class IfElseBlock;
  friend class UnlikelyIfBlock;

  typedef tbb::concurrent_hash_map<TCA, TCA> SignalStubMap;
  typedef void (*sigaction_t)(int, siginfo_t*, void*);

  typedef X64Assembler Asm;
  static const int kMaxInlineContLocals = 10;

  class AHotSelector {
   public:
    AHotSelector(TranslatorX64* tx, bool hot) :
        m_tx(tx), m_hot(hot &&
                        tx->ahot.code.base + tx->ahot.code.size -
                        tx->ahot.code.frontier > 8192 &&
                        tx->a.code.base != tx->ahot.code.base) {
      if (m_hot) {
        m_save = tx->a;
        tx->a = tx->ahot;
      }
    }
    ~AHotSelector() {
      if (m_hot) {
        m_tx->ahot = m_tx->a;
        m_tx->a = m_save;
      }
    }
   private:
    TranslatorX64* m_tx;
    Asm            m_save;
    bool           m_hot;
  };

  Asm                    ahot;
  Asm                    a;
  Asm                    astubs;
  Asm                    atrampolines;
  PointerMap             trampolineMap;
  int                    m_numNativeTrampolines;
  size_t                 m_trampolineSize; // size of each trampoline

  SrcDB                  m_srcDB;
  SignalStubMap          m_segvStubs;
  sigaction_t            m_segvChain;
  TCA                    m_callToExit;
  TCA                    m_retHelper;
  TCA                    m_retInlHelper;
  TCA                    m_genRetHelper;
  TCA                    m_stackOverflowHelper;
  TCA                    m_irPopRHelper;
  TCA                    m_dtorGenericStub;
  TCA                    m_dtorGenericStubRegs;
  TCA                    m_dtorStubs[kDestrTableSize];
  TCA                    m_defClsHelper;
  TCA                    m_funcPrologueRedispatch;

  TCA                    m_freeManyLocalsHelper;
  TCA                    m_freeLocalsHelpers[kNumFreeLocalsHelpers];

  DataBlock              m_globalData;
  size_t                 m_irAUsage;
  size_t                 m_irAstubsUsage;

  // Data structures for HHIR-based translation
  uint64_t               m_numHHIRTrans;
  std::unique_ptr<JIT::IRFactory>
                         m_irFactory;
  std::unique_ptr<JIT::HhbcTranslator>
                         m_hhbcTrans;
  std::string            m_lastHHIRPunt;
  std::string            m_lastHHIRDump;

  void hhirTraceStart(Offset bcStartOffset, Offset nextTraceOffset);
  void hhirTraceCodeGen(vector<TransBCMapping>* bcMap);
  void hhirTraceEnd();
  void hhirTraceFree();


  FixupMap                   m_fixupMap;
  UnwindInfoHandle           m_unwindRegistrar;
  CatchTraceMap              m_catchTraceMap;

public:
  // Currently translating trace or instruction---only valid during
  // translate phase.
  const Tracelet*              m_curTrace;
  const NormalizedInstruction* m_curNI;
  litstr m_curFile;
  int m_curLine;
  litstr m_curFunc;
private:
  int64_t m_createdTime;

  struct PendingFixup {
    TCA m_tca;
    Fixup m_fixup;
    PendingFixup() { }
    PendingFixup(TCA tca, Fixup fixup) :
      m_tca(tca), m_fixup(fixup) { }
  };
  vector<PendingFixup> m_pendingFixups;

  void drawCFG(std::ofstream& out) const;
  static vector<PhysReg> x64TranslRegs();

  Asm& getAsmFor(TCA addr) { return asmChoose(addr, a, ahot, astubs); }
  void emitIncRef(X64Assembler &a, PhysReg base, DataType dtype);
  void emitIncRef(PhysReg base, DataType);
  void emitIncRefGenericRegSafe(PhysReg base, int disp, PhysReg tmp);
  static Call getDtorCall(DataType type);
  void emitCopy(PhysReg srcCell, int disp, PhysReg destCell);
  void emitCopyToStackRegSafe(Asm& a,
                              const NormalizedInstruction& ni,
                              PhysReg src,
                              int off,
                              PhysReg tmpReg);

  void emitThisCheck(const NormalizedInstruction& i, PhysReg reg);

public:
  void emitCall(Asm& a, TCA dest);
  void emitCall(Asm& a, Call call);
private:

  void translateClassExistsImpl(const Tracelet& t,
                                const NormalizedInstruction& i,
                                Attr typeAttr);
  void recordSyncPoint(Asm& a, Offset pcOff, Offset spOff);
  void emitEagerSyncPoint(Asm& a, const Opcode* pc, const Offset spDiff);
  void recordIndirectFixup(CTCA addr, int dwordsPushed);
  void emitStringToClass(const NormalizedInstruction& i);
  void emitStringToKnownClass(const NormalizedInstruction& i,
                              const StringData* clssName);
  void emitObjToClass(const NormalizedInstruction& i);
  void emitClsAndPals(const NormalizedInstruction& i);

  template<int Arity> TCA emitNAryStub(Asm& a, Call c);
  TCA emitUnaryStub(Asm& a, Call c);
  TCA genericRefCountStub(Asm& a);
  TCA genericRefCountStubRegs(Asm& a);
  void emitFreeLocalsHelpers();
  void emitGenericDecRefHelpers();
  TCA getCallArrayProlog(Func* func);
  TCA emitPrologueRedispatch(Asm &a);
  TCA emitFuncGuard(Asm& a, const Func *f);
  template <bool reentrant>
  void emitDerefStoreToLoc(PhysReg srcReg, const Location& destLoc);

  void getInputsIntoXMMRegs(const NormalizedInstruction& ni,
                            PhysReg lr, PhysReg rr,
                            RegXMM lxmm, RegXMM rxmm);
  void binaryMixedArith(const NormalizedInstruction &i,
                         Opcode op, PhysReg srcReg, PhysReg srcDestReg);
  void fpEq(const NormalizedInstruction& i, PhysReg lr, PhysReg rr);
  void emitRB(Asm& a, Trace::RingBufferType t, SrcKey sk,
              RegSet toSave = RegSet());
  void emitRB(Asm& a, Trace::RingBufferType t, const char* msgm,
              RegSet toSave = RegSet());
  void newTuple(const NormalizedInstruction& i, unsigned n);

  enum {
    ArgDontAllocate = -1,
    ArgAnyReg = -2
  };

 private:
#define INSTRS \
  CASE(PopC) \
  CASE(PopV) \
  CASE(PopR) \
  CASE(UnboxR) \
  CASE(Null) \
  CASE(NullUninit) \
  CASE(True) \
  CASE(False) \
  CASE(Int) \
  CASE(Double) \
  CASE(String) \
  CASE(Array) \
  CASE(NewArray) \
  CASE(NewTuple) \
  CASE(NewCol) \
  CASE(Nop) \
  CASE(AddElemC) \
  CASE(AddNewElemC) \
  CASE(ColAddElemC) \
  CASE(ColAddNewElemC) \
  CASE(Cns) \
  CASE(DefCns) \
  CASE(ClsCnsD) \
  CASE(Concat) \
  CASE(Add) \
  CASE(Xor) \
  CASE(Not) \
  CASE(Mod) \
  CASE(BitNot) \
  CASE(CastInt) \
  CASE(CastString) \
  CASE(CastDouble) \
  CASE(CastArray) \
  CASE(CastObject) \
  CASE(Print) \
  CASE(Jmp) \
  CASE(Switch) \
  CASE(SSwitch) \
  CASE(RetC) \
  CASE(RetV) \
  CASE(NativeImpl) \
  CASE(AGetC) \
  CASE(AGetL) \
  CASE(CGetL) \
  CASE(CGetL2) \
  CASE(CGetS) \
  CASE(CGetM) \
  CASE(CGetG) \
  CASE(VGetL) \
  CASE(VGetG) \
  CASE(VGetM) \
  CASE(IssetM) \
  CASE(EmptyM) \
  CASE(AKExists) \
  CASE(SetS) \
  CASE(SetG) \
  CASE(SetM) \
  CASE(SetWithRefLM) \
  CASE(SetWithRefRM) \
  CASE(SetOpL) \
  CASE(SetOpM) \
  CASE(IncDecL) \
  CASE(IncDecM) \
  CASE(UnsetL) \
  CASE(UnsetM) \
  CASE(BindM) \
  CASE(FPushFuncD) \
  CASE(FPushFunc) \
  CASE(FPushClsMethodD) \
  CASE(FPushClsMethodF) \
  CASE(FPushObjMethodD) \
  CASE(FPushCtor) \
  CASE(FPushCtorD) \
  CASE(FPassR) \
  CASE(FPassL) \
  CASE(FPassM) \
  CASE(FPassS) \
  CASE(FPassG) \
  CASE(This) \
  CASE(BareThis) \
  CASE(CheckThis) \
  CASE(InitThisLoc) \
  CASE(FCall) \
  CASE(FCallArray) \
  CASE(FCallBuiltin) \
  CASE(VerifyParamType) \
  CASE(InstanceOfD) \
  CASE(StaticLocInit) \
  CASE(IterInit) \
  CASE(IterInitK) \
  CASE(IterNext) \
  CASE(IterNextK) \
  CASE(WIterInit) \
  CASE(WIterInitK) \
  CASE(WIterNext) \
  CASE(WIterNextK) \
  CASE(ReqDoc) \
  CASE(DefCls) \
  CASE(DefFunc) \
  CASE(Self) \
  CASE(Parent) \
  CASE(ClassExists) \
  CASE(InterfaceExists) \
  CASE(TraitExists) \
  CASE(Dup) \
  CASE(CreateCl) \
  CASE(CreateCont) \
  CASE(ContEnter) \
  CASE(ContExit) \
  CASE(UnpackCont) \
  CASE(PackCont) \
  CASE(ContReceive) \
  CASE(ContRetC) \
  CASE(ContNext) \
  CASE(ContSend) \
  CASE(ContRaise) \
  CASE(ContValid) \
  CASE(ContCurrent) \
  CASE(ContStopped) \
  CASE(ContHandle) \
  CASE(Strlen) \
  CASE(IncStat) \
  CASE(ArrayIdx) \
  CASE(FPushCufIter) \
  CASE(CIterFree) \
  CASE(LateBoundCls) \
  CASE(IssetS) \
  CASE(IssetG) \
  CASE(UnsetG) \
  CASE(EmptyS) \
  CASE(EmptyG) \
  CASE(VGetS) \
  CASE(BindS) \
  CASE(BindG) \
  CASE(IterFree) \
  CASE(FPassV) \
  CASE(UnsetN) \
  CASE(DecodeCufIter) \

  // These are instruction-like functions which cover more than one
  // opcode.
#define PSEUDOINSTRS \
  CASE(BinaryArithOp) \
  CASE(SameOp) \
  CASE(EqOp) \
  CASE(LtGtOp) \
  CASE(UnaryBooleanOp) \
  CASE(BranchOp) \
  CASE(AssignToLocalOp) \
  CASE(FPushCufOp) \
  CASE(FPassCOp) \
  CASE(CheckTypeOp)

  template<typename L>
  void translatorAssert(X64Assembler& a, ConditionCode cc,
                        const char* msg, L setup);

  const Func* findCuf(const NormalizedInstruction& ni,
                      Class* &cls, StringData*& invName, bool& forward);
  static uint64_t toStringHelper(ObjectData *obj);
  void invalidateSrcKey(SrcKey sk);
  bool dontGuardAnyInputs(Opcode op);
 public:
  template<typename T>
  void invalidateSrcKeys(const T& keys) {
    BlockingLeaseHolder writer(s_writeLease);
    assert(writer);
    for (typename T::const_iterator i = keys.begin(); i != keys.end(); ++i) {
      invalidateSrcKey(*i);
    }
  }

  void registerCatchTrace(CTCA ip, TCA trace);
  TCA getCatchTrace(CTCA ip) const;

  static void SEGVHandler(int signum, siginfo_t *info, void *ctx);

  // public for syncing gdb state
  Debug::DebugInfo m_debugInfo;

  void fixupWork(VMExecutionContext* ec, ActRec* startRbp) const;
  void fixup(VMExecutionContext* ec) const;
  TCA getTranslatedCaller() const;

  // helpers for srcDB.
  SrcRec* getSrcRec(SrcKey sk) {
    // TODO: add a insert-or-find primitive to THM
    if (SrcRec* r = m_srcDB.find(sk)) return r;
    assert(s_writeLease.amOwner());
    return m_srcDB.insert(sk);
  }

  TCA getTopTranslation(SrcKey sk) {
    return getSrcRec(sk)->getTopTranslation();
  }

  TCA getCallToExit() {
    return m_callToExit;
  }

  TCA getRetFromInterpretedFrame() {
    return m_retHelper;
  }

  TCA getRetFromInlinedFrame() {
    return m_retInlHelper;
  }

  TCA getRetFromInterpretedGeneratorFrame() {
    return m_genRetHelper;
  }

  inline bool isValidCodeAddress(TCA tca) const {
    return tca >= ahot.code.base && tca < astubs.code.base + astubs.code.size;
  }

  // If we were to shove every little helper function into this class
  // header, we'd spend the rest of our lives compiling. So, these public
  // functions are for static helpers private to translator-x64.cpp. Be
  // professional.

  Asm& getAsm()   { return a; }
  void emitChainTo(SrcKey dest, bool isCall = false);

  static bool isPseudoEvent(const char* event);
  void getPerfCounters(Array& ret);

private:
  virtual void syncWork();

public:
  bool acquireWriteLease(bool blocking) {
    return s_writeLease.acquire(blocking);
  }
  void dropWriteLease() {
    s_writeLease.drop();
  }

  void emitGuardChecks(Asm& a, SrcKey, const ChangeMap&,
    const RefDeps&, SrcRec&);
  void irEmitResolvedDeps(const ChangeMap& resolvedDeps);

  void emitVariantGuards(const Tracelet& t, const NormalizedInstruction& i);

  Debug::DebugInfo* getDebugInfo() { return &m_debugInfo; }

  FreeStubList m_freeStubs;
  bool freeRequestStub(TCA stub);
  TCA getFreeStub();
private:
  void irInterpretInstr(const NormalizedInstruction& i);
  void irTranslateInstr(const Tracelet& t, const NormalizedInstruction& i);
  void irTranslateInstrWork(const Tracelet& t, const NormalizedInstruction& i);
  void irTranslateInstrDefault(const Tracelet& t,
                               const NormalizedInstruction& i);
  bool checkTranslationLimit(SrcKey, const SrcRec&) const;
  enum TranslateTraceletResult {
    Failure,
    Retry,
    Success
  };
  TranslateTraceletResult irTranslateTracelet(Tracelet& t,
                                              const TCA start,
                                              const TCA stubStart,
                                              vector<TransBCMapping>* bcMap);
  bool irTranslateTracelet(const Tracelet&         t,
                           const TCA               start,
                           const TCA               stubStart,
                           vector<TransBCMapping>* bcMap);
  void irPassPredictedAndInferredTypes(const NormalizedInstruction& i);

  void irAssertType(const Location& l, const RuntimeType& rtt);
  void checkType(Asm&, const Location& l, const RuntimeType& rtt,
    SrcRec& fail);
  void irCheckType(Asm&, const Location& l, const RuntimeType& rtt,
                   SrcRec& fail);

  void checkRefs(Asm&, SrcKey, const RefDeps&, SrcRec&);

  void emitInlineReturn(Location retvalSrcLoc, int retvalSrcDisp);
  void emitGenericReturn(bool noThis, int retvalSrcDisp);
  void dumpStack(const char* msg, int offset) const;

 private:
  void moveToAlign(Asm &aa, const size_t alignment = kJmpTargetAlign,
                   const bool unreachable = true);
  static void smash(Asm &a, TCA src, TCA dest, bool isCall);
  static void smashJmp(Asm &a, TCA src, TCA dest) {
    smash(a, src, dest, false);
  }
  static void smashCall(Asm &a, TCA src, TCA dest) {
    smash(a, src, dest, true);
  }

  TCA getTranslation(const TranslArgs& args);
  TCA createTranslation(const TranslArgs& args);
  TCA retranslate(const TranslArgs& args);
  TCA translate(const TranslArgs& args);
  void translateTracelet(const TranslArgs& args);

  TCA lookupTranslation(SrcKey sk) const;
  TCA retranslateOpt(TransID transId, bool align);
  TCA retranslateAndPatchNoIR(SrcKey sk,
                              bool   align,
                              TCA    toSmash);
  TCA bindJmp(TCA toSmash, SrcKey dest, ServiceRequest req, bool& smashed);
  TCA bindJmpccFirst(TCA toSmash,
                     Offset offTrue, Offset offFalse,
                     bool toTake,
                     ConditionCode cc,
                     bool& smashed);
  TCA bindJmpccSecond(TCA toSmash, const Offset off,
                      ConditionCode cc,
                      bool& smashed);
  void emitFallbackJmp(SrcRec& dest, ConditionCode cc = CC_NZ);
  void emitFallbackJmp(Asm& as, SrcRec& dest, ConditionCode cc = CC_NZ);
  void emitFallbackUncondJmp(Asm& as, SrcRec& dest);
  void emitFallbackCondJmp(Asm& as, SrcRec& dest, ConditionCode cc);
  void emitDebugPrint(Asm&, const char*,
                      PhysReg = reg::r13,
                      PhysReg = reg::r14,
                      PhysReg = reg::rax);

  TCA emitServiceReq(ServiceRequest, int numArgs, ...);
  TCA emitServiceReq(SRFlags flags, ServiceRequest, int numArgs, ...);
  TCA emitServiceReqVA(SRFlags flags, ServiceRequest, int numArgs,
                       va_list args);

  TCA emitRetFromInterpretedFrame();
  TCA emitRetFromInterpretedGeneratorFrame();
  TCA emitGearTrigger(Asm& a, SrcKey sk, TransID transId);
  void emitPopRetIntoActRec(Asm& a);
  int32_t emitBindCall(SrcKey srcKey, const Func* funcd, int numArgs);
  void emitCondJmp(SrcKey skTrue, SrcKey skFalse, ConditionCode cc);
  bool handleServiceRequest(TReqInfo&, TCA& start, SrcKey& sk);

  void recordGdbTranslation(SrcKey sk, const Func* f,
                            const Asm& a,
                            const TCA start,
                            bool exit, bool inPrologue);
  void recordGdbStub(const Asm& a, TCA start, const char* name);
  void recordBCInstr(uint32_t op, const Asm& a, const TCA addr);

  void emitStackCheck(int funcDepth, Offset pc);
  void emitStackCheckDynamic(int numArgs, Offset pc);
  void emitTestSurpriseFlags(Asm& a);
  void emitCheckSurpriseFlagsEnter(bool inTracelet, Fixup fixup);
  TCA  emitTransCounterInc(Asm& a);

  static void trimExtraArgs(ActRec* ar);
  static int  shuffleArgsForMagicCall(ActRec* ar);
  static void setArgInActRec(ActRec* ar, int argNum, uint64_t datum,
                             DataType t);
  TCA funcPrologue(Func* func, int nArgs, ActRec* ar = nullptr);
  bool checkCachedPrologue(const Func* func, int param, TCA& plgOut) const;
  SrcKey emitPrologue(Func* func, int nArgs);
  static bool eagerRecord(const Func* func);
  int32_t emitNativeImpl(const Func*, bool emitSavedRIPReturn);
  void emitBindJ(Asm& a, ConditionCode cc, SrcKey dest,
                 ServiceRequest req);
  void emitBindJmp(Asm& a, SrcKey dest,
                   ServiceRequest req = REQ_BIND_JMP);
  void emitBindJcc(Asm& a, ConditionCode cc, SrcKey dest,
                   ServiceRequest req = REQ_BIND_JCC);
  void emitBindJmp(SrcKey dest);
  void emitBindCallHelper(SrcKey srcKey,
                          const Func* funcd,
                          int numArgs);
  void emitIncCounter(TCA start, int cntOfs);

  struct ReqLitStaticArgs {
    HPHP::Eval::PhpFile* m_efile;
    TCA m_pseudoMain;
    Offset m_pcOff;
    bool m_local;
  };
  static void reqLitHelper(const ReqLitStaticArgs* args);
  static void fCallArrayHelper(const Offset pcOff, const Offset pcNext);

  TCA getNativeTrampoline(TCA helperAddress);
  TCA emitNativeTrampoline(TCA helperAddress);

  // Utility function shared with IR code
  static uint64_t packBitVec(const vector<bool>& bits, unsigned i);

public:
  /*
   * enterTC is the main entry point for the translator from the
   * bytecode interpreter (see enterVMWork).  It operates on behalf of
   * a given nested invocation of the intepreter (calling back into it
   * as necessary for blocks that need to be interpreted).
   *
   * If start is not null, data will be used to initialize rStashedAr,
   * to enable us to run a jitted prolog;
   * otherwise, data should be a pointer to the SrcKey to start
   * translating from.
   *
   * But don't call this directly, use one of the helpers below
   */
  void enterTC(TCA start, void* data);
  void enterTCAtSrcKey(SrcKey& sk) {
    enterTC(nullptr, &sk);
  }
  void enterTCAtProlog(ActRec *ar, TCA start) {
    enterTC(start, ar);
  }
  void enterTCAfterProlog(TCA start) {
    enterTC(start, nullptr);
  }

  TranslatorX64();
  virtual ~TranslatorX64();

  void initGdb();
  static TranslatorX64* Get();

  // Called before entering a new PHP "world."
  void requestInit();

  // Called at the end of eval()
  void requestExit();

  // Returns a string with cache usage information
  virtual std::string getUsage();
  virtual size_t getCodeSize();
  virtual size_t getStubSize();
  virtual size_t getTargetCacheSize();

  // true iff calling thread is sole writer.
  static bool canWrite() {
    // We can get called early in boot, so allow null tx64.
    return !tx64 || s_writeLease.amOwner();
  }

  // Returns true on success
  bool dumpTC(bool ignoreLease = false);

  // Returns true on success
  bool dumpTCCode(const char* filename);

  // Returns true on success
  bool dumpTCData();

  // Async hook for file modifications.
  bool invalidateFile(Eval::PhpFile* f);
  void invalidateFileWork(Eval::PhpFile* f);

  // Start a new translation space. Returns true IFF this thread created
  // a new space.
  bool replace();

  // Debugging interfaces to prevent tampering with code.
  void protectCode();
  void unprotectCode();

  int numTranslations(SrcKey sk) const;
private:
  virtual bool addDbgGuards(const Unit* unit);
  virtual bool addDbgGuard(const Func* func, Offset offset);
  void addDbgGuardImpl(SrcKey sk, SrcRec& sr);

public: // Only for HackIR
  void emitReqRetransNoIR(Asm& as, const SrcKey& sk);
#define DECLARE_FUNC(nm) \
  void irTranslate ## nm(const Tracelet& t,               \
                         const NormalizedInstruction& i);
#define CASE DECLARE_FUNC

INSTRS
PSEUDOINSTRS

#undef CASE
#undef DECLARE_FUNC

  void irTranslateReqLit(const Tracelet& t,
                         const NormalizedInstruction& i,
                         InclOpFlags flags);

private:
  // asize + astubssize + gdatasize + trampolinesblocksize
  size_t m_totalSize;
};


/*
 * RAII bookmark for temporarily rewinding a.code.frontier.
 */
class CodeCursor {
  typedef X64Assembler Asm;
  Asm& m_a;
  TCA m_oldFrontier;
  public:
  CodeCursor(Asm& a, TCA newFrontier) :
    m_a(a), m_oldFrontier(a.code.frontier) {
      assert(TranslatorX64::canWrite());
      m_a.code.frontier = newFrontier;
      TRACE_MOD(Trace::trans, 1, "RewindTo: %p (from %p)\n",
                m_a.code.frontier, m_oldFrontier);
    }
  ~CodeCursor() {
    assert(TranslatorX64::canWrite());
    m_a.code.frontier = m_oldFrontier;
    TRACE_MOD(Trace::trans, 1, "Restore: %p\n",
              m_a.code.frontier);
  }
};

const size_t kTrampolinesBlockSize = 8 << 12;

// minimum length in bytes of each trampoline code sequence
// Note that if stats is on, then this size is ~24 bytes due to the
// instrumentation code that counts the number of calls through each
// trampoline
const size_t kMinPerTrampolineSize = 11;

const size_t kMaxNumTrampolines = kTrampolinesBlockSize /
  kMinPerTrampolineSize;

void fcallHelperThunk() asm ("__fcallHelperThunk");
void funcBodyHelperThunk() asm ("__funcBodyHelperThunk");
void functionEnterHelper(const ActRec* ar);
int64_t decodeCufIterHelper(Iter* it, TypedValue func);

// These could be static but are used in hopt/codegen.cpp
void raiseUndefVariable(StringData* nm);
void defFuncHelper(Func *f);
Instance* newInstanceHelper(Class* cls, int numArgs, ActRec* ar,
                            ActRec* prevAr);
Instance* newInstanceHelperCached(Class** classCache,
                                  const StringData* clsName, int numArgs,
                                  ActRec* ar, ActRec* prevAr);
Instance* newInstanceHelperNoCtorCached(Class** classCache,
                                        const StringData* clsName);

SrcKey nextSrcKey(const Tracelet& t, const NormalizedInstruction& i);
bool isNormalPropertyAccess(const NormalizedInstruction& i,
                       int propInput,
                       int objInput);

bool mInstrHasUnknownOffsets(const NormalizedInstruction& i,
                             Class* contextClass);

struct PropInfo {
  PropInfo()
    : offset(-1)
    , hphpcType(KindOfInvalid)
  {}
  explicit PropInfo(int offset, DataType hphpcType)
    : offset(offset)
    , hphpcType(hphpcType)
  {}

  int offset;
  DataType hphpcType;
};

PropInfo getPropertyOffset(const NormalizedInstruction& ni,
                           Class* contextClass,
                           const Class*& baseClass,
                           const MInstrInfo& mii,
                           unsigned mInd, unsigned iInd);
PropInfo getFinalPropertyOffset(const NormalizedInstruction&,
                                Class* contextClass,
                                const MInstrInfo&);

bool isSupportedCGetM(const NormalizedInstruction& i);
TXFlags planInstrAdd_Int(const NormalizedInstruction& i);
TXFlags planInstrAdd_Array(const NormalizedInstruction& i);
void dumpTranslationInfo(const Tracelet& t, TCA postGuards);

bool classIsUnique(const Class* cls);
bool classIsUniqueOrCtxParent(const Class* cls);
bool classIsUniqueNormalClass(const Class* cls);

// SpaceRecorder is used in translator-x64.cpp and in hopt/irtranslator.cpp
// RAII logger for TC space consumption.
struct SpaceRecorder {
  const char *m_name;
  const X64Assembler m_a;
  // const X64Assembler& m_a;
  const uint8_t *m_start;
  SpaceRecorder(const char* name, const X64Assembler& a) :
      m_name(name), m_a(a), m_start(a.code.frontier)
    { }
  ~SpaceRecorder() {
    if (Trace::moduleEnabledRelease(Trace::tcspace, 1)) {
      ptrdiff_t diff = m_a.code.frontier - m_start;
      if (diff) Trace::traceRelease("TCSpace %10s %3d\n", m_name, diff);
    }
  }
};

typedef const int COff; // Const offsets

}}

#endif