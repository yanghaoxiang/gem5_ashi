from m5.defines import buildEnv
from m5.objects.BaseCPU import BaseCPU
from m5.params import *
from m5.proxy import *
from m5.SimObject import *

class BaseAshiCPU(BaseCPU):
    type = "BaseAshiCPU"
    cxx_class = "gem5::ashi::CPU"
    cxx_header = "cpu/ashi/cpu.hh"

    @classmethod
    def memory_mode(cls):
        return "timing"

    @classmethod
    def require_caches(cls):
        return True
    
    @classmethod
    def support_take_over(cls):
        return False