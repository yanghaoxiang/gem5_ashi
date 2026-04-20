#include "cpu/ashi/cpu.hh"
#include "cpu/simple_thread.hh"

#include "base/logging.hh"
#include "params/BaseAshiCPU.hh"
#include "sim/system.hh"

namespace gem5
{

namespace ashi
{

CPU::CPU(const BaseAshiCPUParams &params)
    : BaseCPU(params),
      icachePort(name() + ".icache_port"),
      dcachePort(name() + ".dcache_port"),
      fetchEvent([this]{fetch();},name()),
      committedInsts(0),
      committedOps(0)
{

    SimpleThread *thread;
    gem5::ThreadContext *tc = thread -> getTC();
    threadContexts.push_back(tc);
    _status = Idle;
}


void startup()
{

    BaseCPU::startup();
}

void
CPU::wakeup(ThreadID tid)
{
    panic("%s wakeup(tid=%u) is not implemented yet", name(), tid);
}

Counter
CPU::totalInsts() const
{
    return committedInsts;
}

Counter
CPU::totalOps() const
{
    return committedOps;
}

void
CPU::verifyMemoryMode() const
{
    if (!params().switched_out && system->getMemoryMode() != enums::timing) {
        fatal("The Ashi CPU requires the memory system to be in 'timing' mode.\n");
    }
}

void
CPU::activateContext(ThreadID tid)
{
    schedule(fetchEvent, clockEdge(Cycles(0)));
    _status = Running;
    BaseCPU::activateContext(tid);
}

Port &
CPU::getDataPort()
{
    return dcachePort;
}

Port &
CPU::getInstPort()
{
    return icachePort;
}

} // namespace ashi

} // namespace gem5