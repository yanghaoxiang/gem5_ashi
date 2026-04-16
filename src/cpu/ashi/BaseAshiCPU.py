from m5.defines import buildEnv
from m5.objects.BaseCPU import BaseCPU
from m5.params import *
from m5.proxy import *
from m5.SimObject import *

class BaseAshiCPU(BaseCPU):
    type = "BaseAshiCPU"
    cxx_class = "gem5::AshiCPU"
    cxx_header = "cpu/ashi/cpu.hh"