# BaseTimingSimpleCPU 初始化与指令执行流程详解

本文档详细说明 `BaseTimingSimpleCPU`（C++ 类名 `TimingSimpleCPU`）在一个通过 Python config 脚本建立的 gem5 SoC 系统中，是如何被初始化的，以及如何触发取指（Fetch）、译码（Decode）、执行（Execute）、提交（Commit）和再次取指的完整指令执行循环。以一条正确执行的 RISC-V `add` 指令为例，逐步跟踪代码中对象实例的调用过程。

---

## 1. 类继承层次

```
SimObject
  └── ClockedObject
        └── BaseCPU                      (src/cpu/base.hh)
              └── BaseSimpleCPU          (src/cpu/simple/base.hh)
                    └── TimingSimpleCPU  (src/cpu/simple/timing.hh)
```

**Python 端继承层次：**

```
SimObject
  └── BaseCPU                       (src/cpu/BaseCPU.py)
        └── BaseSimpleCPU           (src/cpu/simple/BaseSimpleCPU.py)
              └── BaseTimingSimpleCPU (src/cpu/simple/BaseTimingSimpleCPU.py)
                    └── RiscvTimingSimpleCPU (arch-specific, e.g. src/arch/riscv/RiscvCPU.py)
```

---

## 2. Python Config 脚本建立 SoC 系统

以 `configs/learning_gem5/part1/simple-riscv.py` 为例：

```python
import m5
from m5.objects import *

# ① 创建顶层 System 对象
system = System()
system.clk_domain = SrcClockDomain()
system.clk_domain.clock = "1GHz"
system.clk_domain.voltage_domain = VoltageDomain()
system.mem_mode = "timing"
system.mem_ranges = [AddrRange("512MiB")]

# ② 创建 TimingSimpleCPU 的 Python 实例
system.cpu = RiscvTimingSimpleCPU()

# ③ 创建总线并连接 CPU 端口
system.membus = SystemXBar()
system.cpu.icache_port = system.membus.cpu_side_ports    # IcachePort
system.cpu.dcache_port = system.membus.cpu_side_ports    # DcachePort

# ④ 创建中断控制器
system.cpu.createInterruptController()

# ⑤ 创建内存控制器
system.mem_ctrl = MemCtrl()
system.mem_ctrl.dram = DDR3_1600_8x8()
system.mem_ctrl.dram.range = system.mem_ranges[0]
system.mem_ctrl.port = system.membus.mem_side_ports

# ⑥ 设置 workload（SE 模式）
system.workload = SEWorkload.init_compatible(binary)
process = Process()
process.cmd = [binary]
system.cpu.workload = process
system.cpu.createThreads()

# ⑦ 创建 Root 并实例化
root = Root(full_system=False, system=system)
m5.instantiate()     # ← 触发 C++ 对象创建和 init()

# ⑧ 启动仿真
exit_event = m5.simulate()   # ← 触发 startup()，然后进入事件循环
```

---

## 3. 初始化流程（从 Python 到 C++）

### 3.1 `m5.instantiate()` —— 创建 C++ 对象

> 源码：`src/python/m5/simulate.py:220`

```python
def instantiate(ckpt_dir=None):
    root = Root.getInstance()
    _fix_all_objects(root)
    _dump_configs(root)
    _create_cpp_objects(root, ckpt_dir)   # ← 递归创建所有 C++ SimObject
    _dump_configs_post_cpp(root)
```

`_create_cpp_objects()` 会递归遍历 Python SimObject 树，依次为每个 Python 对象调用对应 C++ 类的**构造函数**和 `init()` 方法。

### 3.2 C++ 构造函数调用链

**步骤 A：`BaseSimpleCPU::BaseSimpleCPU()`**

> 源码：`src/cpu/simple/base.cc:83-120`

```cpp
BaseSimpleCPU::BaseSimpleCPU(const BaseSimpleCPUParams &p)
    : BaseCPU(p),
      curThread(0),
      branchPred(p.branchPred),
      traceData(NULL),
      _status(Idle)                  // 初始状态设为 Idle
{
    // 为每个硬件线程创建 SimpleThread 和 SimpleExecContext
    for (unsigned i = 0; i < numThreads; i++) {
        SimpleThread *thread;
        if (FullSystem) {
            thread = new SimpleThread(this, i, p.system, p.mmu, p.isa[i], p.decoder[i]);
        } else {
            thread = new SimpleThread(this, i, p.system, p.workload[i],
                                      p.mmu, p.isa[i], p.decoder[i]);
        }
        threadInfo.push_back(new SimpleExecContext(this, thread));
        ThreadContext *tc = thread->getTC();
        threadContexts.push_back(tc);
    }
}
```

关键初始化内容：
- 调用父类 `BaseCPU` 构造函数（设置时钟域、requestor ID 等）
- 创建 `SimpleThread`（包含寄存器文件、PC 状态、ISA 实例、Decoder 实例）
- 创建 `SimpleExecContext`（指令执行上下文，封装了对线程状态的访问）
- 初始化 `_status = Idle`

**步骤 B：`TimingSimpleCPU::TimingSimpleCPU()`**

> 源码：`src/cpu/simple/timing.cc:76-82`

```cpp
TimingSimpleCPU::TimingSimpleCPU(const BaseTimingSimpleCPUParams &p)
    : BaseSimpleCPU(p),
      fetchTranslation(this),       // FetchTranslation 对象（TLB 回调）
      icachePort(this),             // 指令缓存端口
      dcachePort(this),             // 数据缓存端口
      ifetch_pkt(NULL),
      dcache_pkt(NULL),
      previousCycle(0),
      fetchEvent([this]{ fetch(); }, name())  // fetchEvent：调度时会调用 fetch()
{
    _status = Idle;
}
```

关键初始化内容：
- 创建 `fetchTranslation`（`FetchTranslation` 类型，TLB 翻译完成后回调 `sendFetch()`）
- 创建 `icachePort`（`IcachePort`）和 `dcachePort`（`DcachePort`），用于与内存子系统通信
- 创建 `fetchEvent`（`EventFunctionWrapper`），被调度时会调用 `this->fetch()`
- 状态为 `Idle`，CPU 暂时不执行任何操作

### 3.3 `init()` —— SimObject 初始化

> 源码：`src/cpu/simple/timing.cc:63-67`

```cpp
void TimingSimpleCPU::init()
{
    BaseSimpleCPU::init();
    // BaseSimpleCPU::init() → BaseCPU::init()
    // 完成线程上下文注册、端口连接验证等
}
```

`init()` 由 `m5.instantiate()` 框架在所有 C++ 对象创建完成后自动调用，确保所有端口连接已建立、线程上下文已在 System 中注册。

### 3.4 `m5.simulate()` —— 启动仿真循环

> 源码：`src/python/m5/simulate.py:247-281`

```python
def simulate(*args, **kwargs):
    if need_startup:
        root = Root.getInstance()
        for obj in root.descendants():
            obj.startup()             # ← 调用每个 SimObject 的 startup()
        need_startup = False
    ...
    sim_out = _m5_event.simulate()    # ← 进入 C++ 事件驱动主循环
```

在 `startup()` 阶段，`BaseCPU::startup()` 会通过 `ThreadContext::activate()` 激活线程，最终调用到 `TimingSimpleCPU::activateContext()`。

### 3.5 `activateContext()` —— 启动执行的关键入口

> 源码：`src/cpu/simple/timing.cc:208-228`

```cpp
void TimingSimpleCPU::activateContext(ThreadID thread_num)
{
    DPRINTF(SimpleCPU, "ActivateContext %d\n", thread_num);

    assert(thread_num < numThreads);

    threadInfo[thread_num]->execContextStats.notIdleFraction = 1;
    if (_status == BaseSimpleCPU::Idle)
        _status = BaseSimpleCPU::Running;   // 状态从 Idle → Running

    // 关键：调度 fetchEvent，在下一个时钟沿触发 fetch()
    if (!fetchEvent.scheduled())
        schedule(fetchEvent, clockEdge(Cycles(0)));

    if (std::find(activeThreads.begin(), activeThreads.end(), thread_num)
         == activeThreads.end()) {
        activeThreads.push_back(thread_num);
    }

    BaseCPU::activateContext(thread_num);
}
```

**这是整个执行循环的起点**：
- 状态从 `Idle` → `Running`
- 调用 `schedule(fetchEvent, clockEdge(Cycles(0)))` 把 `fetchEvent` 调度到当前时钟沿
- 当事件循环执行到该时钟沿时，`fetchEvent` 触发，调用 `fetch()`

---

## 4. 指令执行循环详解

TimingSimpleCPU 是**事件驱动**的，没有传统的 `tick()` 循环。整个执行流程由以下事件和回调驱动：

```
activateContext()
  └── schedule(fetchEvent) ──→ fetch()
                                  └── translateTiming() ──→ FetchTranslation::finish()
                                                              └── sendFetch()
                                                                    └── icachePort.sendTimingReq()
                                                                          ──(memory system)──→
                                                                          IcachePort::recvTimingResp()
                                                                            └── schedule(ITickEvent)
                                                                                  └── ITickEvent::process()
                                                                                        └── completeIfetch()
                                                                                              ├── preExecute()   [译码]
                                                                                              ├── execute()      [执行]
                                                                                              ├── postExecute()  [提交统计]
                                                                                              └── advanceInst()
                                                                                                    ├── advancePC()   [更新 PC]
                                                                                                    └── fetch()       [再次取指]
```

### 4.1 取指（Fetch）

> 源码：`src/cpu/simple/timing.cc:679-717`

```cpp
void TimingSimpleCPU::fetch()
{
    swapActiveThread();                              // 多线程切换

    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;

    // 检查中断和 PC 事件
    if (!curStaticInst || !curStaticInst->isDelayedCommit()) {
        checkForInterrupts();
        checkPcEventQueue();
    }

    if (_status == Idle) return;

    MicroPC upc = thread->pcState().microPC();
    bool needToFetch = !isRomMicroPC(upc) && !curMacroStaticInst;

    if (needToFetch) {
        _status = BaseSimpleCPU::Running;
        RequestPtr ifetch_req = std::make_shared<Request>();
        ifetch_req->taskId(taskId());
        ifetch_req->setContext(thread->contextId());
        setupFetchRequest(ifetch_req);               // 设置取指地址

        // 发起 TLB 翻译（异步），完成后回调 FetchTranslation::finish()
        thread->mmu->translateTiming(ifetch_req, thread->getTC(),
                &fetchTranslation, BaseMMU::Execute);
    } else {
        // ROM 微码指令，不需要从 ICache 取指
        _status = IcacheWaitResponse;
        completeIfetch(NULL);
    }
}
```

`setupFetchRequest()` 设置取指请求：

> 源码：`src/cpu/simple/base.cc:322-337`

```cpp
void BaseSimpleCPU::setupFetchRequest(const RequestPtr &req)
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;
    auto &decoder = thread->decoder;
    Addr instAddr = thread->pcState().instAddr();
    Addr fetchPC = (instAddr & decoder->pcMask()) + t_info.fetchOffset;

    req->setVirt(fetchPC, decoder->moreBytesSize(), Request::INST_FETCH,
                 instRequestorId(), instAddr);
}
```

### 4.2 TLB 翻译完成回调 → sendFetch()

TLB 翻译完成后，回调 `FetchTranslation::finish()`：

> 源码：`src/cpu/simple/timing.hh:128-133`

```cpp
void finish(const Fault &fault, const RequestPtr &req, ThreadContext *tc,
            BaseMMU::Mode mode)
{
    cpu->sendFetch(fault, req, tc);    // 回调到 TimingSimpleCPU::sendFetch()
}
```

**`sendFetch()`** 构造取指包并发送到指令缓存：

> 源码：`src/cpu/simple/timing.cc:721-751`

```cpp
void TimingSimpleCPU::sendFetch(const Fault &fault, const RequestPtr &req,
                                ThreadContext *tc)
{
    if (fault == NoFault) {
        // 创建取指数据包
        ifetch_pkt = new Packet(req, MemCmd::ReadReq);
        ifetch_pkt->dataStatic(decoder->moreBytesPtr());

        // 通过 icachePort 向内存系统发送 timing 请求
        if (!icachePort.sendTimingReq(ifetch_pkt)) {
            _status = IcacheRetry;       // 发送失败，等待重试
        } else {
            _status = IcacheWaitResponse; // 发送成功，等待响应
            ifetch_pkt = NULL;
        }
    } else {
        // 取指翻译出错，直接进入故障处理
        _status = BaseSimpleCPU::Running;
        advanceInst(fault);
    }
}
```

### 4.3 ICache 响应 → 触发译码和执行

内存系统返回取指数据后，通过端口回调通知 CPU：

> 源码：`src/cpu/simple/timing.cc:911-930`

```cpp
bool TimingSimpleCPU::IcachePort::recvTimingResp(PacketPtr pkt)
{
    assert(!tickEvent.scheduled());
    // 延迟到下一个 CPU 时钟沿处理
    tickEvent.schedule(pkt, cpu->clockEdge());
    return true;
}
```

在下一个时钟沿，`ITickEvent::process()` 被执行：

> 源码：`src/cpu/simple/timing.cc:905-908`

```cpp
void TimingSimpleCPU::IcachePort::ITickEvent::process()
{
    cpu->completeIfetch(pkt);   // 进入指令完成处理
}
```

### 4.4 译码（Decode）+ 执行（Execute）+ 提交（Commit）

`completeIfetch()` 是核心处理函数，包含译码、执行和提交三个阶段：

> 源码：`src/cpu/simple/timing.cc:821-902`

```cpp
void TimingSimpleCPU::completeIfetch(PacketPtr pkt)
{
    assert(_status == IcacheWaitResponse);
    _status = BaseSimpleCPU::Running;

    // ========== 译码阶段 ==========
    preExecute();
    // preExecute() 内部：
    //   1. decoder->moreBytes() 将取回的字节送入 decoder
    //   2. decoder->decode() 解码出 StaticInst
    //   3. 设置 curStaticInst 为解码后的指令对象
    //   4. 记录 trace 信息
    //   5. 如果有分支预测器，做分支预测

    if (curStaticInst && curStaticInst->isMemRef()) {
        // ========== 内存指令：分两步执行 ==========
        Fault fault = curStaticInst->initiateAcc(&t_info, traceData);
        // initiateAcc() 发起内存访问请求
        // 后续在 completeDataAccess() 中通过 completeAcc() 完成

        if (_status == BaseSimpleCPU::Running) {
            if (fault == NoFault)
                postExecute();     // 提交统计
            advanceInst(fault);    // 推进到下一条指令
        }
        // 如果是正常的 load/store，_status 会变为 DcacheWaitResponse
        // 需要等待 dcache 响应后才继续
    } else if (curStaticInst) {
        // ========== 非内存指令：一次完成执行 ==========
        Fault fault = curStaticInst->execute(&t_info, traceData);
        // execute() 直接执行指令（读寄存器、ALU 运算、写寄存器）

        if (fault == NoFault) {
            postExecute();         // ========== 提交阶段 ==========
            countInst();
        }
        advanceInst(fault);        // 推进到下一条指令
    } else {
        advanceInst(NoFault);
    }

    if (pkt) delete pkt;
}
```

#### 4.4.1 `preExecute()` 详解（译码）

> 源码：`src/cpu/simple/base.cc:347-428`

```cpp
void BaseSimpleCPU::preExecute()
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;

    t_info.setPredicate(true);
    t_info.setMemAccPredicate(true);

    set(preExecuteTempPC, thread->pcState());
    auto &pc_state = *preExecuteTempPC;
    auto &decoder = thread->decoder;

    if (isRomMicroPC(pc_state.microPC())) {
        // ROM 微码
        curStaticInst = decoder->fetchRomMicroop(pc_state.microPC(),
                                                  curMacroStaticInst);
    } else if (!curMacroStaticInst) {
        // 普通指令：将取回的字节送入 decoder 并解码
        Addr fetch_pc = (pc_state.instAddr() & decoder->pcMask())
                        + t_info.fetchOffset;
        decoder->moreBytes(pc_state, fetch_pc);          // 送入字节
        StaticInstPtr instPtr = decoder->decode(pc_state); // 解码

        if (instPtr) {
            t_info.stayAtPC = false;
            thread->pcState(pc_state);
        } else {
            t_info.stayAtPC = true;       // 需要更多字节
            t_info.fetchOffset += decoder->moreBytesSize();
        }

        if (instPtr && instPtr->isMacroop()) {
            curMacroStaticInst = instPtr;
            curStaticInst = curMacroStaticInst->fetchMicroop(
                                pc_state.microPC());
        } else {
            curStaticInst = instPtr;      // ← 这就是解码后的指令对象
        }
    }

    // 记录 trace 数据
    if (curStaticInst) {
        traceData = tracer->getInstRecord(curTick(), thread->getTC(),
                curStaticInst, thread->pcState(), curMacroStaticInst);
    }

    // 分支预测
    if (branchPred && curStaticInst && curStaticInst->isControl()) {
        const InstSeqNum cur_sn(0);
        set(t_info.predPC, thread->pcState());
        branchPred->predict(curStaticInst, cur_sn, *t_info.predPC, curThread);
    }

    // 统计取指数
    if (curStaticInst)
        countFetchInst();
}
```

#### 4.4.2 `execute()` 详解（执行）

对于非内存指令（如 `add`），直接调用 `curStaticInst->execute()`：

```cpp
Fault fault = curStaticInst->execute(&t_info, traceData);
```

`execute()` 是 `StaticInst` 的虚函数，每条 ISA 指令都有其具体实现。以 RISC-V `add` 指令为例，其 `execute()` 方法会：
1. 从 `ExecContext`（即 `SimpleExecContext`）读取源寄存器 rs1、rs2
2. 执行 ALU 加法运算
3. 将结果写入目标寄存器 rd

#### 4.4.3 `postExecute()` 详解（提交/统计）

> 源码：`src/cpu/simple/base.cc:431-521`

```cpp
void BaseSimpleCPU::postExecute()
{
    // 更新各类统计计数器
    // - numMemRefs（内存引用数）
    // - numIntAluAccesses（整数ALU访问数）
    // - numBranches（分支数）
    // - numCallsReturns（调用/返回数）
    // - committedInstType（已提交指令类型统计）
    // 等

    countCommitInst();           // 增加已提交指令计数

    if (traceData) {
        traceData->dump();       // 输出 trace 记录
        delete traceData;
        traceData = NULL;
    }

    probeInstCommit(curStaticInst, instAddr);  // 触发提交探针
}
```

### 4.5 推进到下一条指令（advanceInst → 再次取指）

> 源码：`src/cpu/simple/timing.cc:755-817`

```cpp
void TimingSimpleCPU::advanceInst(const Fault &fault)
{
    if (_status == Faulting) return;

    if (fault != NoFault) {
        // 故障处理：调用 advancePC(fault) 跳转到异常处理程序
        advancePC(fault);
        if (_status != Idle) {
            reschedule(fetchEvent, stall, true);
            _status = Faulting;
        }
        return;
    }

    // 正常情况：推进 PC
    if (!t_info.stayAtPC)
        advancePC(fault);      // PC += 4（对 RISC-V 而言）

    if (tryCompleteDrain()) return;

    serviceInstCountEvents();

    // 关键：如果状态仍然是 Running，立即发起下一次取指
    if (_status == BaseSimpleCPU::Running) {
        fetch();               // ← 递归调用 fetch()，开始下一条指令
    }
}
```

`advancePC()` 的实现：

> 源码：`src/cpu/simple/base.cc:524-561`

```cpp
void BaseSimpleCPU::advancePC(const Fault &fault)
{
    SimpleExecContext &t_info = *threadInfo[curThread];
    SimpleThread* thread = t_info.thread;

    t_info.fetchOffset = 0;
    if (fault != NoFault) {
        curMacroStaticInst = nullStaticInstPtr;
        fault->invoke(threadContexts[curThread], curStaticInst);
        thread->decoder->reset();
    } else {
        if (curStaticInst) {
            if (curStaticInst->isLastMicroop())
                curMacroStaticInst = nullStaticInstPtr;
            curStaticInst->advancePC(thread);   // 更新 PC 状态
        }
    }

    // 分支预测器更新
    if (branchPred && curStaticInst && curStaticInst->isControl()) {
        // ...
    }
}
```

---

## 5. 具体示例：RISC-V `add x3, x1, x2` 的完整执行流程

假设 PC = 0x10074，内存中有一条 RISC-V `add x3, x1, x2` 指令（机器码 `0x002080B3`），x1 = 5，x2 = 7。

### 时间线

```
Tick 0: activateContext(0) 被调用
  ├── _status: Idle → Running
  └── schedule(fetchEvent, clockEdge(0))

Tick T1 (时钟沿): fetchEvent 触发 → fetch() 被调用
  ├── swapActiveThread()    — 选择 curThread = 0
  ├── checkForInterrupts()  — 无中断
  ├── checkPcEventQueue()   — 无 PC 事件
  ├── needToFetch = true    — 不是 ROM 微码
  ├── setupFetchRequest()   — 设置取指请求：vaddr=0x10074, size=4, INST_FETCH
  └── mmu->translateTiming(ifetch_req, tc, &fetchTranslation, Execute)
      ├── TLB 查找：vaddr 0x10074 → paddr 0x10074（SE 模式下通常直接映射）
      └── 翻译完成，回调 fetchTranslation.finish(NoFault, req, tc)

      fetchTranslation.finish() 调用 sendFetch(NoFault, req, tc)
        ├── 创建 Packet：ifetch_pkt = new Packet(req, ReadReq)
        ├── ifetch_pkt->dataStatic(decoder->moreBytesPtr())
        ├── icachePort.sendTimingReq(ifetch_pkt) → 发送到 ICache/内存系统
        ├── 发送成功：_status = IcacheWaitResponse
        └── ifetch_pkt = NULL

(内存系统处理取指请求... ICache 命中或 miss 后最终返回数据)

Tick T2: IcachePort::recvTimingResp(pkt) 被内存系统调用
  ├── pkt 包含取回的指令字节 0x002080B3
  └── tickEvent.schedule(pkt, cpu->clockEdge())
      （延迟到下一个 CPU 时钟沿）

Tick T3 (时钟沿): ITickEvent::process() 被调用
  └── cpu->completeIfetch(pkt)

      completeIfetch(pkt):
        ├── _status = Running
        │
        ├── ====== preExecute() [译码阶段] ======
        │   ├── decoder->moreBytes(pcState, 0x10074)
        │   │     将取回的 4 字节 (0x002080B3) 送入 decoder 缓冲区
        │   ├── decoder->decode(pcState)
        │   │     解码 0x002080B3 → RiscvStaticInst: "add x3, x1, x2"
        │   │     返回 StaticInstPtr，即解码后的指令对象
        │   ├── curStaticInst = 解码后的 add 指令对象
        │   ├── stayAtPC = false（解码成功，不需要更多字节）
        │   └── countFetchInst()  — 取指计数 +1
        │
        ├── curStaticInst->isMemRef() == false （add 不是内存指令）
        │
        ├── ====== execute() [执行阶段] ======
        │   curStaticInst->execute(&t_info, traceData)
        │   ├── 读 rs1 (x1): t_info.getRegOperand(0) → 5
        │   ├── 读 rs2 (x2): t_info.getRegOperand(1) → 7
        │   ├── ALU 计算：5 + 7 = 12
        │   ├── 写 rd (x3): t_info.setRegOperand(0, 12)
        │   └── 返回 NoFault
        │
        ├── fault == NoFault
        │
        ├── ====== postExecute() [提交阶段] ======
        │   ├── commitStats[tid]->numInsts++      — 已提交指令数 +1
        │   ├── numIntAluAccesses++                — 整数 ALU 访问数 +1
        │   ├── committedInstType[IntAlu]++        — 整数 ALU 类型指令 +1
        │   ├── traceData->dump()                  — 输出 trace（如果开启）
        │   └── probeInstCommit()                  — 触发提交探针
        │
        ├── countInst()  — numInst++, numOp++
        │
        └── ====== advanceInst(NoFault) [推进 PC] ======
            ├── stayAtPC == false
            ├── advancePC(NoFault)
            │   └── curStaticInst->advancePC(thread)
            │       └── PC: 0x10074 → 0x10078  （+4）
            │
            ├── serviceInstCountEvents()
            │
            └── _status == Running, 所以：
                fetch()  ← 立即开始取下一条指令（递归调用）
                （流程回到上面的 "取指" 阶段）
```

---

## 6. 内存指令的特殊流程（load/store）

对于内存指令（如 `lw x3, 0(x1)`），执行流程与非内存指令不同，分为两步：

### 6.1 发起内存访问

在 `completeIfetch()` 中：

```cpp
Fault fault = curStaticInst->initiateAcc(&t_info, traceData);
```

`initiateAcc()` 内部会调用 `TimingSimpleCPU::initiateMemRead()`，通过 `dcachePort` 发送读请求到数据缓存。此时 `_status` 变为 `DcacheWaitResponse`。

### 6.2 数据缓存响应

当数据缓存返回响应后：

```
DcachePort::recvTimingResp(pkt)
  └── tickEvent.schedule(pkt, cpu->clockEdge())  — 延迟到下一时钟沿

DTickEvent::process()
  └── cpu->completeDataAccess(pkt)
```

`completeDataAccess()` 中：

> 源码：`src/cpu/simple/timing.cc:947-1080`

```cpp
void TimingSimpleCPU::completeDataAccess(PacketPtr pkt)
{
    _status = BaseSimpleCPU::Running;

    // 完成内存访问，将数据写入寄存器
    fault = curStaticInst->completeAcc(pkt, t_info, traceData);

    if (fault == NoFault) {
        postExecute();     // 提交统计
        countInst();
    }

    delete pkt;
    advanceInst(fault);    // 推进到下一条指令 → fetch()
}
```

---

## 7. CPU 状态机

TimingSimpleCPU 通过 `_status` 状态变量跟踪当前执行阶段：

```
                        ┌──────────────────────────────────────────────────────────────────┐
                        │                                                                  │
                        v                                                                  │
  ┌──────┐    activateContext()   ┌─────────┐    translateTiming()    ┌──────────────────┐  │
  │ Idle │ ──────────────────────→│ Running │ ──────────────────────→ │ ITBWaitResponse  │  │
  └──────┘                        └─────────┘                         └──────────────────┘  │
                                       │                                      │             │
                                       │  sendFetch()                         │ finish()    │
                                       │  成功                                 │             │
                                       v                                      v             │
                              ┌────────────────────┐                  ┌───────────┐         │
                              │ IcacheWaitResponse  │←────────────────│  (回调)    │         │
                              └────────────────────┘                  └───────────┘         │
                                       │                                                    │
                                       │ recvTimingResp()                                   │
                                       v                                                    │
                              ┌────────────────────┐                                        │
                              │ Running (执行指令)  │                                       │
                              └────────────────────┘                                        │
                                    │           │                                           │
                          非内存指令 │           │ 内存指令(initiateAcc)                     │
                                    │           v                                           │
                                    │   ┌────────────────────┐                              │
                                    │   │ DcacheWaitResponse  │                             │
                                    │   └────────────────────┘                              │
                                    │           │                                           │
                                    │           │ recvTimingResp()                           │
                                    │           │ → completeDataAccess()                     │
                                    │           v                                           │
                                    │   ┌───────────┐                                      │
                                    │   │  Running   │                                      │
                                    │   └───────────┘                                      │
                                    │           │                                           │
                                    └─────┬─────┘                                           │
                                          │ advanceInst() → advancePC() → fetch()           │
                                          └─────────────────────────────────────────────────┘
```

如果 `sendTimingReq()` 失败（端口忙），状态变为 `IcacheRetry` 或 `DcacheRetry`，等待 `recvReqRetry()` 回调后重新发送请求。

---

## 8. 关键设计要点

1. **事件驱动而非时钟驱动**：TimingSimpleCPU 没有 `tick()` 循环，完全由内存系统的响应事件驱动执行。CPU 发出请求后"等待"，直到内存系统通过端口回调通知响应到达。

2. **同一 tick 内可以连续执行多条非内存指令**：因为 `advanceInst()` 在状态仍为 `Running` 时会直接递归调用 `fetch()`，如果 ICache 立即返回（或是 ROM 微码），不需要等待下一个 tick。

3. **内存指令分两阶段执行**：`initiateAcc()`（发起请求）和 `completeAcc()`（完成访问），中间等待内存系统响应。

4. **ITickEvent / DTickEvent 延迟到时钟沿**：即使内存系统在任意时刻返回响应，CPU 也会将处理延迟到下一个 CPU 时钟沿（`clockEdge()`），以模拟真实的时序行为。

---

## 9. 源码文件索引

| 文件 | 说明 |
|------|------|
| `src/cpu/simple/timing.hh` | TimingSimpleCPU 类定义、IcachePort/DcachePort 内部类 |
| `src/cpu/simple/timing.cc` | TimingSimpleCPU 实现：fetch, sendFetch, completeIfetch, advanceInst, completeDataAccess 等 |
| `src/cpu/simple/base.hh` | BaseSimpleCPU 类定义、Status 枚举 |
| `src/cpu/simple/base.cc` | BaseSimpleCPU 实现：preExecute, postExecute, advancePC, 构造函数等 |
| `src/cpu/simple/exec_context.hh` | SimpleExecContext，指令执行时访问寄存器/内存的上下文 |
| `src/cpu/simple/BaseTimingSimpleCPU.py` | Python SimObject 定义，`cxx_class = "gem5::TimingSimpleCPU"` |
| `src/cpu/simple/BaseSimpleCPU.py` | Python 父类 SimObject 定义 |
| `src/cpu/simple/TimingSimpleCPU.py` | 架构特定的 TimingSimpleCPU 包装（选择 Riscv/Arm/X86 等变体） |
| `src/python/m5/simulate.py` | `m5.instantiate()` 和 `m5.simulate()` 实现 |
| `configs/learning_gem5/part1/simple-riscv.py` | RISC-V SoC 配置示例脚本 |
