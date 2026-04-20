#ifndef __CPU_ASHI_CPU_HH__
#define __CPU_ASHI_CPU_HH__

#include "cpu/base.hh"
#include "mem/port.hh"
#include "params/BaseAshiCPU.hh"


namespace gem5
{

namespace ashi
{

class CPU : public BaseCPU
{
  protected:
    RequestPort icachePort;
    RequestPort dcachePort;

    Counter committedInsts;
    Counter committedOps;

  public:
    enum Status
    {
        Running,
        Idle
    };  

    Status _status;

    CPU(const BaseAshiCPUParams &params);

    void wakeup(ThreadID tid) override;

    Counter totalInsts() const override;
    Counter totalOps() const override;

    void verifyMemoryMode() const override;
    void activateContext(ThreadID tid) override;

    void fetch();

  private:
    EventFunctionWrapper fetchEvent;


  protected:
    Port &getDataPort() override;
    Port &getInstPort() override;

};

} // namespace ashi

} // namespace gem5
#endif // __CPU_ASHI_CPU_HH__