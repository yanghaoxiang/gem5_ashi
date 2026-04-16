import m5.defines

arch_vars = [
    "USE_ARM_ISA",
    "USE_MIPS_ISA",
    "USE_POWER_ISA",
    "USE_RISCV_ISA",
    "USE_SPARC_ISA",
    "USE_X86_ISA",
]

enabled = list(filter(lambda var: m5.defines.buildEnv[var], arch_vars))

if len(enabled) == 1:
    arch = enabled[0]
    if arch == "USE_RISCV_ISA":
        from m5.objects.RiscvCPU import RiscvAshiCPU as AshiCPU