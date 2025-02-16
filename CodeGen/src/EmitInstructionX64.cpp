// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details
#include "EmitInstructionX64.h"

#include "Luau/AssemblyBuilderX64.h"

#include "CustomExecUtils.h"
#include "EmitBuiltinsX64.h"
#include "EmitCommonX64.h"
#include "NativeState.h"
#include "IrTranslateBuiltins.h" // Used temporarily until emitInstFastCallN is removed

#include "lobject.h"
#include "ltm.h"

namespace Luau
{
namespace CodeGen
{

void emitInstNameCall(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, const TValue* k, Label& next, Label& fallback)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    uint32_t aux = pc[1];

    Label secondfpath;

    jumpIfTagIsNot(build, rb, LUA_TTABLE, fallback);

    RegisterX64 table = r8;
    build.mov(table, luauRegValue(rb));

    // &h->node[tsvalue(kv)->hash & (sizenode(h) - 1)];
    RegisterX64 node = rdx;
    build.mov(node, qword[table + offsetof(Table, node)]);
    build.mov(eax, 1);
    build.mov(cl, byte[table + offsetof(Table, lsizenode)]);
    build.shl(eax, cl);
    build.dec(eax);
    build.and_(eax, tsvalue(&k[aux])->hash);
    build.shl(rax, kLuaNodeSizeLog2);
    build.add(node, rax);

    jumpIfNodeKeyNotInExpectedSlot(build, rax, node, luauConstantValue(aux), secondfpath);

    setLuauReg(build, xmm0, ra + 1, luauReg(rb));
    setLuauReg(build, xmm0, ra, luauNodeValue(node));
    build.jmp(next);

    build.setLabel(secondfpath);

    jumpIfNodeHasNext(build, node, fallback);
    callGetFastTmOrFallback(build, table, TM_INDEX, fallback);
    jumpIfTagIsNot(build, rax, LUA_TTABLE, fallback);

    build.mov(table, qword[rax + offsetof(TValue, value)]);

    getTableNodeAtCachedSlot(build, rax, node, table, pcpos);
    jumpIfNodeKeyNotInExpectedSlot(build, rax, node, luauConstantValue(aux), fallback);

    setLuauReg(build, xmm0, ra + 1, luauReg(rb));
    setLuauReg(build, xmm0, ra, luauNodeValue(node));
}

void emitInstCall(AssemblyBuilderX64& build, ModuleHelpers& helpers, const Instruction* pc, int pcpos)
{
    int ra = LUAU_INSN_A(*pc);
    int nparams = LUAU_INSN_B(*pc) - 1;
    int nresults = LUAU_INSN_C(*pc) - 1;

    emitInterrupt(build, pcpos);

    emitSetSavedPc(build, pcpos + 1);

    build.mov(rArg1, rState);
    build.lea(rArg2, luauRegAddress(ra));

    if (nparams == LUA_MULTRET)
        build.mov(rArg3, qword[rState + offsetof(lua_State, top)]);
    else
        build.lea(rArg3, luauRegAddress(ra + 1 + nparams));

    build.mov(dwordReg(rArg4), nresults);
    build.call(qword[rNativeContext + offsetof(NativeContext, callProlog)]);
    RegisterX64 ccl = rax; // Returned from callProlog

    emitUpdateBase(build);

    Label cFuncCall;

    build.test(byte[ccl + offsetof(Closure, isC)], 1);
    build.jcc(ConditionX64::NotZero, cFuncCall);

    {
        RegisterX64 proto = rcx; // Sync with emitContinueCallInVm
        RegisterX64 ci = rdx;
        RegisterX64 argi = rsi;
        RegisterX64 argend = rdi;

        build.mov(proto, qword[ccl + offsetof(Closure, l.p)]);

        // Switch current Closure
        build.mov(sClosure, ccl); // Last use of 'ccl'

        build.mov(ci, qword[rState + offsetof(lua_State, ci)]);

        Label fillnil, exitfillnil;

        // argi = L->top
        build.mov(argi, qword[rState + offsetof(lua_State, top)]);

        // argend = L->base + p->numparams
        build.movzx(eax, byte[proto + offsetof(Proto, numparams)]);
        build.shl(eax, kTValueSizeLog2);
        build.lea(argend, addr[rBase + rax]);

        // while (argi < argend) setnilvalue(argi++);
        build.setLabel(fillnil);
        build.cmp(argi, argend);
        build.jcc(ConditionX64::NotBelow, exitfillnil);

        build.mov(dword[argi + offsetof(TValue, tt)], LUA_TNIL);
        build.add(argi, sizeof(TValue));
        build.jmp(fillnil); // This loop rarely runs so it's not worth repeating cmp/jcc

        build.setLabel(exitfillnil);

        // Set L->top to ci->top as most function expect (no vararg)
        build.mov(rax, qword[ci + offsetof(CallInfo, top)]);
        build.mov(qword[rState + offsetof(lua_State, top)], rax);

        build.mov(rax, qword[proto + offsetofProtoExecData]); // We'll need this value later

        // But if it is vararg, update it to 'argi'
        Label skipVararg;

        build.test(byte[proto + offsetof(Proto, is_vararg)], 1);
        build.jcc(ConditionX64::Zero, skipVararg);

        build.mov(qword[rState + offsetof(lua_State, top)], argi);
        build.setLabel(skipVararg);

        // Check native function data
        build.test(rax, rax);
        build.jcc(ConditionX64::Zero, helpers.continueCallInVm);

        // Switch current constants
        build.mov(rConstants, qword[proto + offsetof(Proto, k)]);

        // Switch current code
        build.mov(rdx, qword[proto + offsetof(Proto, code)]);
        build.mov(sCode, rdx);

        build.jmp(qword[rax + offsetof(NativeProto, entryTarget)]);
    }

    build.setLabel(cFuncCall);

    {
        // results = ccl->c.f(L);
        build.mov(rArg1, rState);
        build.call(qword[ccl + offsetof(Closure, c.f)]); // Last use of 'ccl'
        RegisterX64 results = eax;

        build.test(results, results);                            // test here will set SF=1 for a negative number and it always sets OF to 0
        build.jcc(ConditionX64::Less, helpers.exitNoContinueVm); // jl jumps if SF != OF

        // We have special handling for small number of expected results below
        if (nresults != 0 && nresults != 1)
        {
            build.mov(rArg1, rState);
            build.mov(dwordReg(rArg2), nresults);
            build.mov(dwordReg(rArg3), results);
            build.call(qword[rNativeContext + offsetof(NativeContext, callEpilogC)]);

            emitUpdateBase(build);
            return;
        }

        RegisterX64 ci = rdx;
        RegisterX64 cip = rcx;
        RegisterX64 vali = rsi;

        build.mov(ci, qword[rState + offsetof(lua_State, ci)]);
        build.lea(cip, addr[ci - sizeof(CallInfo)]);

        // L->base = cip->base
        build.mov(rBase, qword[cip + offsetof(CallInfo, base)]);
        build.mov(qword[rState + offsetof(lua_State, base)], rBase);

        if (nresults == 1)
        {
            // Opportunistically copy the result we expected from (L->top - results)
            build.mov(vali, qword[rState + offsetof(lua_State, top)]);
            build.shl(results, kTValueSizeLog2);
            build.sub(vali, qwordReg(results));
            build.vmovups(xmm0, xmmword[vali]);
            build.vmovups(luauReg(ra), xmm0);

            Label skipnil;

            // If there was no result, override the value with 'nil'
            build.test(results, results);
            build.jcc(ConditionX64::NotZero, skipnil);
            build.mov(luauRegTag(ra), LUA_TNIL);
            build.setLabel(skipnil);
        }

        // L->ci = cip
        build.mov(qword[rState + offsetof(lua_State, ci)], cip);

        // L->top = cip->top
        build.mov(rax, qword[cip + offsetof(CallInfo, top)]);
        build.mov(qword[rState + offsetof(lua_State, top)], rax);
    }
}

void emitInstReturn(AssemblyBuilderX64& build, ModuleHelpers& helpers, const Instruction* pc, int pcpos)
{
    emitInterrupt(build, pcpos);

    int ra = LUAU_INSN_A(*pc);
    int b = LUAU_INSN_B(*pc) - 1;

    RegisterX64 ci = r8;
    RegisterX64 cip = r9;
    RegisterX64 res = rdi;
    RegisterX64 nresults = esi;

    build.mov(ci, qword[rState + offsetof(lua_State, ci)]);
    build.lea(cip, addr[ci - sizeof(CallInfo)]);

    // res = ci->func; note: we assume CALL always puts func+args and expects results to start at func
    build.mov(res, qword[ci + offsetof(CallInfo, func)]);
    // nresults = ci->nresults
    build.mov(nresults, dword[ci + offsetof(CallInfo, nresults)]);

    {
        Label skipResultCopy;

        RegisterX64 counter = ecx;

        if (b == 0)
        {
            // Our instruction doesn't have any results, so just fill results expected in parent with 'nil'
            build.test(nresults, nresults);                     // test here will set SF=1 for a negative number, ZF=1 for zero and OF=0
            build.jcc(ConditionX64::LessEqual, skipResultCopy); // jle jumps if SF != OF or ZF == 1

            build.mov(counter, nresults);

            Label repeatNilLoop = build.setLabel();
            build.mov(dword[res + offsetof(TValue, tt)], LUA_TNIL);
            build.add(res, sizeof(TValue));
            build.dec(counter);
            build.jcc(ConditionX64::NotZero, repeatNilLoop);
        }
        else if (b == 1)
        {
            // Try setting our 1 result
            build.test(nresults, nresults);
            build.jcc(ConditionX64::Zero, skipResultCopy);

            build.lea(counter, addr[nresults - 1]);

            build.vmovups(xmm0, luauReg(ra));
            build.vmovups(xmmword[res], xmm0);
            build.add(res, sizeof(TValue));

            // Fill the rest of the expected results with 'nil'
            build.test(counter, counter);                       // test here will set SF=1 for a negative number, ZF=1 for zero and OF=0
            build.jcc(ConditionX64::LessEqual, skipResultCopy); // jle jumps if SF != OF or ZF == 1

            Label repeatNilLoop = build.setLabel();
            build.mov(dword[res + offsetof(TValue, tt)], LUA_TNIL);
            build.add(res, sizeof(TValue));
            build.dec(counter);
            build.jcc(ConditionX64::NotZero, repeatNilLoop);
        }
        else
        {
            RegisterX64 vali = rax;
            RegisterX64 valend = rdx;

            // Copy return values into parent stack (but only up to nresults!)
            build.test(nresults, nresults);
            build.jcc(ConditionX64::Zero, skipResultCopy);

            // vali = ra
            build.lea(vali, luauRegAddress(ra));

            // Copy as much as possible for MULTRET calls, and only as much as needed otherwise
            if (b == LUA_MULTRET)
                build.mov(valend, qword[rState + offsetof(lua_State, top)]); // valend = L->top
            else
                build.lea(valend, luauRegAddress(ra + b)); // valend = ra + b

            build.mov(counter, nresults);

            Label repeatValueLoop, exitValueLoop;

            build.setLabel(repeatValueLoop);
            build.cmp(vali, valend);
            build.jcc(ConditionX64::NotBelow, exitValueLoop);

            build.vmovups(xmm0, xmmword[vali]);
            build.vmovups(xmmword[res], xmm0);
            build.add(vali, sizeof(TValue));
            build.add(res, sizeof(TValue));
            build.dec(counter);
            build.jcc(ConditionX64::NotZero, repeatValueLoop);

            build.setLabel(exitValueLoop);

            // Fill the rest of the expected results with 'nil'
            build.test(counter, counter);                       // test here will set SF=1 for a negative number, ZF=1 for zero and OF=0
            build.jcc(ConditionX64::LessEqual, skipResultCopy); // jle jumps if SF != OF or ZF == 1

            Label repeatNilLoop = build.setLabel();
            build.mov(dword[res + offsetof(TValue, tt)], LUA_TNIL);
            build.add(res, sizeof(TValue));
            build.dec(counter);
            build.jcc(ConditionX64::NotZero, repeatNilLoop);
        }

        build.setLabel(skipResultCopy);
    }

    build.mov(qword[rState + offsetof(lua_State, ci)], cip);     // L->ci = cip
    build.mov(rBase, qword[cip + offsetof(CallInfo, base)]);     // sync base = L->base while we have a chance
    build.mov(qword[rState + offsetof(lua_State, base)], rBase); // L->base = cip->base

    // Start with result for LUA_MULTRET/exit value
    build.mov(qword[rState + offsetof(lua_State, top)], res); // L->top = res

    // Unlikely, but this might be the last return from VM
    build.test(byte[ci + offsetof(CallInfo, flags)], LUA_CALLINFO_RETURN);
    build.jcc(ConditionX64::NotZero, helpers.exitNoContinueVm);

    Label skipFixedRetTop;
    build.test(nresults, nresults);                 // test here will set SF=1 for a negative number and it always sets OF to 0
    build.jcc(ConditionX64::Less, skipFixedRetTop); // jl jumps if SF != OF
    build.mov(rax, qword[cip + offsetof(CallInfo, top)]);
    build.mov(qword[rState + offsetof(lua_State, top)], rax); // L->top = cip->top
    build.setLabel(skipFixedRetTop);

    // Returning back to the previous function is a bit tricky
    // Registers alive: r9 (cip)
    RegisterX64 proto = rcx;
    RegisterX64 execdata = rbx;

    // Change closure
    build.mov(rax, qword[cip + offsetof(CallInfo, func)]);
    build.mov(rax, qword[rax + offsetof(TValue, value.gc)]);
    build.mov(sClosure, rax);

    build.mov(proto, qword[rax + offsetof(Closure, l.p)]);

    build.mov(execdata, qword[proto + offsetofProtoExecData]);
    build.test(execdata, execdata);
    build.jcc(ConditionX64::Zero, helpers.exitContinueVm); // Continue in interpreter if function has no native data

    // Change constants
    build.mov(rConstants, qword[proto + offsetof(Proto, k)]);

    // Change code
    build.mov(rdx, qword[proto + offsetof(Proto, code)]);
    build.mov(sCode, rdx);

    build.mov(rax, qword[cip + offsetof(CallInfo, savedpc)]);

    // To get instruction index from instruction pointer, we need to divide byte offset by 4
    // But we will actually need to scale instruction index by 8 back to byte offset later so it cancels out
    build.sub(rax, rdx);

    // Get new instruction location and jump to it
    build.mov(rdx, qword[execdata + offsetof(NativeProto, instTargets)]);
    build.jmp(qword[rdx + rax * 2]);
}

void emitInstSetList(AssemblyBuilderX64& build, const Instruction* pc, Label& next)
{
    int ra = LUAU_INSN_A(*pc);
    int rb = LUAU_INSN_B(*pc);
    int c = LUAU_INSN_C(*pc) - 1;
    uint32_t index = pc[1];

    OperandX64 last = index + c - 1;

    // Using non-volatile 'rbx' for dynamic 'c' value (for LUA_MULTRET) to skip later recomputation
    // We also keep 'c' scaled by sizeof(TValue) here as it helps in the loop below
    RegisterX64 cscaled = rbx;

    if (c == LUA_MULTRET)
    {
        RegisterX64 tmp = rax;

        // c = L->top - rb
        build.mov(cscaled, qword[rState + offsetof(lua_State, top)]);
        build.lea(tmp, luauRegAddress(rb));
        build.sub(cscaled, tmp); // Using byte difference

        // L->top = L->ci->top
        build.mov(tmp, qword[rState + offsetof(lua_State, ci)]);
        build.mov(tmp, qword[tmp + offsetof(CallInfo, top)]);
        build.mov(qword[rState + offsetof(lua_State, top)], tmp);

        // last = index + c - 1;
        last = edx;
        build.mov(last, dwordReg(cscaled));
        build.shr(last, kTValueSizeLog2);
        build.add(last, index - 1);
    }

    Label skipResize;

    RegisterX64 table = rax;

    build.mov(table, luauRegValue(ra));

    // Resize if h->sizearray < last
    build.cmp(dword[table + offsetof(Table, sizearray)], last);
    build.jcc(ConditionX64::NotBelow, skipResize);

    // Argument setup reordered to avoid conflicts
    LUAU_ASSERT(rArg3 != table);
    build.mov(dwordReg(rArg3), last);
    build.mov(rArg2, table);
    build.mov(rArg1, rState);
    build.call(qword[rNativeContext + offsetof(NativeContext, luaH_resizearray)]);
    build.mov(table, luauRegValue(ra)); // Reload cloberred register value

    build.setLabel(skipResize);

    RegisterX64 arrayDst = rdx;
    RegisterX64 offset = rcx;

    build.mov(arrayDst, qword[table + offsetof(Table, array)]);

    const int kUnrollSetListLimit = 4;

    if (c != LUA_MULTRET && c <= kUnrollSetListLimit)
    {
        for (int i = 0; i < c; ++i)
        {
            // setobj2t(L, &array[index + i - 1], rb + i);
            build.vmovups(xmm0, luauRegValue(rb + i));
            build.vmovups(xmmword[arrayDst + (index + i - 1) * sizeof(TValue)], xmm0);
        }
    }
    else
    {
        LUAU_ASSERT(c != 0);

        build.xor_(offset, offset);
        if (index != 1)
            build.add(arrayDst, (index - 1) * sizeof(TValue));

        Label repeatLoop, endLoop;
        OperandX64 limit = c == LUA_MULTRET ? cscaled : OperandX64(c * sizeof(TValue));

        // If c is static, we will always do at least one iteration
        if (c == LUA_MULTRET)
        {
            build.cmp(offset, limit);
            build.jcc(ConditionX64::NotBelow, endLoop);
        }

        build.setLabel(repeatLoop);

        // setobj2t(L, &array[index + i - 1], rb + i);
        build.vmovups(xmm0, xmmword[offset + rBase + rb * sizeof(TValue)]); // luauReg(rb) unwrapped to add offset
        build.vmovups(xmmword[offset + arrayDst], xmm0);

        build.add(offset, sizeof(TValue));
        build.cmp(offset, limit);
        build.jcc(ConditionX64::Below, repeatLoop);

        build.setLabel(endLoop);
    }

    callBarrierTableFast(build, table, next);
}

static void emitInstFastCallN(
    AssemblyBuilderX64& build, const Instruction* pc, bool customParams, int customParamCount, OperandX64 customArgs, int pcpos, Label& fallback)
{
    int bfid = LUAU_INSN_A(*pc);
    int skip = LUAU_INSN_C(*pc);

    Instruction call = pc[skip + 1];
    LUAU_ASSERT(LUAU_INSN_OP(call) == LOP_CALL);
    int ra = LUAU_INSN_A(call);

    int nparams = customParams ? customParamCount : LUAU_INSN_B(call) - 1;
    int nresults = LUAU_INSN_C(call) - 1;
    int arg = customParams ? LUAU_INSN_B(*pc) : ra + 1;
    OperandX64 args = customParams ? customArgs : luauRegAddress(ra + 2);

    BuiltinImplResult br = emitBuiltin(build, LuauBuiltinFunction(bfid), nparams, ra, arg, args, nresults, fallback);

    if (br.type == BuiltinImplType::UsesFallback)
    {
        if (nresults == LUA_MULTRET)
        {
            // L->top = ra + n;
            build.lea(rax, addr[rBase + (ra + br.actualResultCount) * sizeof(TValue)]);
            build.mov(qword[rState + offsetof(lua_State, top)], rax);
        }
        else if (nparams == LUA_MULTRET)
        {
            // L->top = L->ci->top;
            build.mov(rax, qword[rState + offsetof(lua_State, ci)]);
            build.mov(rax, qword[rax + offsetof(CallInfo, top)]);
            build.mov(qword[rState + offsetof(lua_State, top)], rax);
        }

        return;
    }

    // TODO: we can skip saving pc for some well-behaved builtins which we didn't inline
    emitSetSavedPc(build, pcpos + 1); // uses rax/rdx

    build.mov(rax, qword[rNativeContext + offsetof(NativeContext, luauF_table) + bfid * sizeof(luau_FastFunction)]);

    // 5th parameter (args) is left unset for LOP_FASTCALL1
    if (args.cat == CategoryX64::mem)
    {
        if (build.abi == ABIX64::Windows)
        {
            build.lea(rcx, args);
            build.mov(sArg5, rcx);
        }
        else
        {
            build.lea(rArg5, args);
        }
    }

    if (nparams == LUA_MULTRET)
    {
        // L->top - (ra + 1)
        RegisterX64 reg = (build.abi == ABIX64::Windows) ? rcx : rArg6;
        build.mov(reg, qword[rState + offsetof(lua_State, top)]);
        build.lea(rdx, addr[rBase + (ra + 1) * sizeof(TValue)]);
        build.sub(reg, rdx);
        build.shr(reg, kTValueSizeLog2);

        if (build.abi == ABIX64::Windows)
            build.mov(sArg6, reg);
    }
    else
    {
        if (build.abi == ABIX64::Windows)
            build.mov(sArg6, nparams);
        else
            build.mov(rArg6, nparams);
    }

    build.mov(rArg1, rState);
    build.lea(rArg2, luauRegAddress(ra));
    build.lea(rArg3, luauRegAddress(arg));
    build.mov(dwordReg(rArg4), nresults);

    build.call(rax);

    build.test(eax, eax);                    // test here will set SF=1 for a negative number and it always sets OF to 0
    build.jcc(ConditionX64::Less, fallback); // jl jumps if SF != OF

    if (nresults == LUA_MULTRET)
    {
        // L->top = ra + n;
        build.shl(rax, kTValueSizeLog2);
        build.lea(rax, addr[rBase + rax + ra * sizeof(TValue)]);
        build.mov(qword[rState + offsetof(lua_State, top)], rax);
    }
    else if (nparams == LUA_MULTRET)
    {
        // L->top = L->ci->top;
        build.mov(rax, qword[rState + offsetof(lua_State, ci)]);
        build.mov(rax, qword[rax + offsetof(CallInfo, top)]);
        build.mov(qword[rState + offsetof(lua_State, top)], rax);
    }
}

void emitInstFastCall1(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, Label& fallback)
{
    return emitInstFastCallN(build, pc, /* customParams */ true, /* customParamCount */ 1, /* customArgs */ 0, pcpos, fallback);
}

void emitInstFastCall2(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, Label& fallback)
{
    return emitInstFastCallN(build, pc, /* customParams */ true, /* customParamCount */ 2, /* customArgs */ luauRegAddress(pc[1]), pcpos, fallback);
}

void emitInstFastCall2K(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, Label& fallback)
{
    return emitInstFastCallN(
        build, pc, /* customParams */ true, /* customParamCount */ 2, /* customArgs */ luauConstantAddress(pc[1]), pcpos, fallback);
}

void emitInstFastCall(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, Label& fallback)
{
    return emitInstFastCallN(build, pc, /* customParams */ false, /* customParamCount */ 0, /* customArgs */ 0, pcpos, fallback);
}

void emitinstForGLoop(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, Label& loopRepeat, Label& loopExit, Label& fallback)
{
    int ra = LUAU_INSN_A(*pc);
    int aux = pc[1];

    emitInterrupt(build, pcpos);

    // fast-path: builtin table iteration
    jumpIfTagIsNot(build, ra, LUA_TNIL, fallback);

    // Registers are chosen in this way to simplify fallback code for the node part
    RegisterX64 table = rArg2;
    RegisterX64 index = rArg3;
    RegisterX64 elemPtr = rax;

    build.mov(table, luauRegValue(ra + 1));
    build.mov(index, luauRegValue(ra + 2));

    // &array[index]
    build.mov(dwordReg(elemPtr), dwordReg(index));
    build.shl(dwordReg(elemPtr), kTValueSizeLog2);
    build.add(elemPtr, qword[table + offsetof(Table, array)]);

    // Clear extra variables since we might have more than two
    for (int i = 2; i < aux; ++i)
        build.mov(luauRegTag(ra + 3 + i), LUA_TNIL);

    // ipairs-style traversal is terminated early when array part ends of nil array element is encountered
    bool isIpairsIter = aux < 0;

    Label skipArray, skipArrayNil;

    // First we advance index through the array portion
    // while (unsigned(index) < unsigned(sizearray))
    Label arrayLoop = build.setLabel();
    build.cmp(dwordReg(index), dword[table + offsetof(Table, sizearray)]);
    build.jcc(ConditionX64::NotBelow, isIpairsIter ? loopExit : skipArray);

    // If element is nil, we increment the index; if it's not, we still need 'index + 1' inside
    build.inc(index);

    build.cmp(dword[elemPtr + offsetof(TValue, tt)], LUA_TNIL);
    build.jcc(ConditionX64::Equal, isIpairsIter ? loopExit : skipArrayNil);

    // setpvalue(ra + 2, reinterpret_cast<void*>(uintptr_t(index + 1)));
    build.mov(luauRegValue(ra + 2), index);
    // Tag should already be set to lightuserdata

    // setnvalue(ra + 3, double(index + 1));
    build.vcvtsi2sd(xmm0, xmm0, dwordReg(index));
    build.vmovsd(luauRegValue(ra + 3), xmm0);
    build.mov(luauRegTag(ra + 3), LUA_TNUMBER);

    // setobj2s(L, ra + 4, e);
    setLuauReg(build, xmm2, ra + 4, xmmword[elemPtr]);

    build.jmp(loopRepeat);

    if (!isIpairsIter)
    {
        build.setLabel(skipArrayNil);

        // Index already incremented, advance to next array element
        build.add(elemPtr, sizeof(TValue));
        build.jmp(arrayLoop);

        build.setLabel(skipArray);

        // Call helper to assign next node value or to signal loop exit
        build.mov(rArg1, rState);
        // rArg2 and rArg3 are already set
        build.lea(rArg4, luauRegAddress(ra));
        build.call(qword[rNativeContext + offsetof(NativeContext, forgLoopNodeIter)]);
        build.test(al, al);
        build.jcc(ConditionX64::NotZero, loopRepeat);
    }
}

void emitinstForGLoopFallback(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, Label& loopRepeat)
{
    int ra = LUAU_INSN_A(*pc);
    int aux = pc[1];

    emitSetSavedPc(build, pcpos + 1);

    build.mov(rArg1, rState);
    build.mov(dwordReg(rArg2), ra);
    build.mov(dwordReg(rArg3), aux);
    build.call(qword[rNativeContext + offsetof(NativeContext, forgLoopNonTableFallback)]);
    emitUpdateBase(build);
    build.test(al, al);
    build.jcc(ConditionX64::NotZero, loopRepeat);
}

void emitInstForGPrepXnextFallback(AssemblyBuilderX64& build, const Instruction* pc, int pcpos, Label& target)
{
    int ra = LUAU_INSN_A(*pc);

    build.mov(rArg1, rState);
    build.lea(rArg2, luauRegAddress(ra));
    build.mov(dwordReg(rArg3), pcpos + 1);
    build.call(qword[rNativeContext + offsetof(NativeContext, forgPrepXnextFallback)]);
    build.jmp(target);
}

static void emitInstAndX(AssemblyBuilderX64& build, int ra, int rb, OperandX64 c)
{
    Label target, fallthrough;
    jumpIfFalsy(build, rb, target, fallthrough);

    build.setLabel(fallthrough);

    build.vmovups(xmm0, c);
    build.vmovups(luauReg(ra), xmm0);

    if (ra == rb)
    {
        build.setLabel(target);
    }
    else
    {
        Label exit;
        build.jmp(exit);

        build.setLabel(target);

        build.vmovups(xmm0, luauReg(rb));
        build.vmovups(luauReg(ra), xmm0);

        build.setLabel(exit);
    }
}

void emitInstAnd(AssemblyBuilderX64& build, const Instruction* pc)
{
    emitInstAndX(build, LUAU_INSN_A(*pc), LUAU_INSN_B(*pc), luauReg(LUAU_INSN_C(*pc)));
}

void emitInstAndK(AssemblyBuilderX64& build, const Instruction* pc)
{
    emitInstAndX(build, LUAU_INSN_A(*pc), LUAU_INSN_B(*pc), luauConstant(LUAU_INSN_C(*pc)));
}

static void emitInstOrX(AssemblyBuilderX64& build, int ra, int rb, OperandX64 c)
{
    Label target, fallthrough;
    jumpIfTruthy(build, rb, target, fallthrough);

    build.setLabel(fallthrough);

    build.vmovups(xmm0, c);
    build.vmovups(luauReg(ra), xmm0);

    if (ra == rb)
    {
        build.setLabel(target);
    }
    else
    {
        Label exit;
        build.jmp(exit);

        build.setLabel(target);

        build.vmovups(xmm0, luauReg(rb));
        build.vmovups(luauReg(ra), xmm0);

        build.setLabel(exit);
    }
}

void emitInstOr(AssemblyBuilderX64& build, const Instruction* pc)
{
    emitInstOrX(build, LUAU_INSN_A(*pc), LUAU_INSN_B(*pc), luauReg(LUAU_INSN_C(*pc)));
}

void emitInstOrK(AssemblyBuilderX64& build, const Instruction* pc)
{
    emitInstOrX(build, LUAU_INSN_A(*pc), LUAU_INSN_B(*pc), luauConstant(LUAU_INSN_C(*pc)));
}

void emitInstGetImportFallback(AssemblyBuilderX64& build, int ra, uint32_t aux)
{
    build.mov(rax, sClosure);

    // luaV_getimport(L, cl->env, k, aux, /* propagatenil= */ false)
    build.mov(rArg1, rState);
    build.mov(rArg2, qword[rax + offsetof(Closure, env)]);
    build.mov(rArg3, rConstants);
    build.mov(dwordReg(rArg4), aux);

    if (build.abi == ABIX64::Windows)
        build.mov(sArg5, 0);
    else
        build.xor_(rArg5, rArg5);

    build.call(qword[rNativeContext + offsetof(NativeContext, luaV_getimport)]);

    emitUpdateBase(build);

    // setobj2s(L, ra, L->top - 1)
    build.mov(rax, qword[rState + offsetof(lua_State, top)]);
    build.sub(rax, sizeof(TValue));
    build.vmovups(xmm0, xmmword[rax]);
    build.vmovups(luauReg(ra), xmm0);

    // L->top--
    build.mov(qword[rState + offsetof(lua_State, top)], rax);
}

void emitInstCoverage(AssemblyBuilderX64& build, int pcpos)
{
    build.mov(rcx, sCode);
    build.add(rcx, pcpos * sizeof(Instruction));

    // hits = LUAU_INSN_E(*pc)
    build.mov(edx, dword[rcx]);
    build.sar(edx, 8);

    // hits = (hits < (1 << 23) - 1) ? hits + 1 : hits;
    build.xor_(eax, eax);
    build.cmp(edx, (1 << 23) - 1);
    build.setcc(ConditionX64::NotEqual, al);
    build.add(edx, eax);


    // VM_PATCH_E(pc, hits);
    build.sal(edx, 8);
    build.movzx(eax, byte[rcx]);
    build.or_(eax, edx);
    build.mov(dword[rcx], eax);
}

} // namespace CodeGen
} // namespace Luau
