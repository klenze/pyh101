from _h101 import *

import numpy, math, sys, os, os.path, subprocess
import traceback, copy
import h101.trigger_map
from  h101.tdc_cal import *

if not 'UCESB_DIR' in os.environ:
    raise RuntimeError("Environment UCESB_DIR not set.")

ucesb=os.environ['UCESB_DIR']


def mkh101(inputs, unpacker=None, options=""):
        if unpacker == None:
            if not 'EXP_NAME' in os.environ:
                raise RuntimeError("No unpacker specified, and EXP_NAME is not set.")
            unpacker=os.environ["EXP_NAME"]
        if unpacker.find("/") == -1:
            upexps="%s/../upexps/%s/%s"%(ucesb, unpacker, unpacker)
        else:
            upexps=unpacker
        upexpscall=upexps+" %s %s --quiet --ntuple=RAW,STRUCT,-"%(options, inputs)
        print("Running unpacker: %s"%upexpscall)
        sp=subprocess.Popen(upexpscall, shell=True,
                            stdout=subprocess.PIPE)
        res=H101(fd=sp.stdout.fileno())
        res.triggermap=trigger_map.parse_channels(upexps)
        res.unpacker=sp
        t=test_iteminfo()
        t.register(res)
        n=tdc_iteminfo.addFields(res)
        print("Added %d single edge TDC arrays"%n)
        n=tot_iteminfo.addFields(res)
        print("Added %d dual edge TDC arrays"%n)
        return res
 

