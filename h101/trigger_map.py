#!/usr/bin/env python3
import subprocess, sys, os, re, os.path
from h101.namespace_magic import namespace_magic

reg=""
reg+="^SIGNAL[(]"
reg+="(?P<mapname>[A-Za-z0-9_]*[A-Z])"
reg+="(?P<mapidx>[0-9]*),"
reg+="(?P<module>[^,]+)[.]"
reg+="(?P<channel>[^,.]+),"
reg+="(?P<datatype>[^,]+)"
reg+="[)];?$"
signal=re.compile(reg)

def parse_channels(directory):
    #f=subprocess.check_output("gcc -I gen_%s -E mapping.hh"%(os.path.basename(directory)),
    #                          cwd=directory, shell=True, encoding='utf-8')
    f=subprocess.check_output("gcc -I %s -E - <run_all.spec"%(os.environ.get("UCESB_DIR", "/")),
                              cwd=directory, shell=True, encoding='utf-8')
    # we can not modify locals() because python is a party pooper. 
    mod2trig=dict()   # module names to trigger (mapname, index)
    mapped2mod=dict() # (mapped name, idx) to module name

    with namespace_magic(globals()) as g:
        for l in f.split(";"):
            l=l.replace(" ", "").replace('\n', "")
            m=signal.match(l)
            if not m:
                continue
            g.update(m.groupdict())
            if channel=="trig_fine[0]":
                mod2trig[module]=(mapname, int(mapidx))
            elif channel.startswith("time_fine"):
                mapped2mod[(mapname, int(mapidx))]=module
            # TODO: handle VFTX. typically, one of the 16 channels is used
            # for the trigger fine time. Test for mapname containing TRIG?
            # Will add this once we have data+unpacker with VFTX fine time triggers. 
            #elif module.startswith("los_vme"):
            #    print(mapname, module, ".", channel)
    mapped2trig={} # (channel mapname, idx) -> (trigger mapname, idx)
    whined=set()
    for k, v in mapped2mod.items():
        if not v in mod2trig:
            if not v in whined: # only whine for ch 1
                print("%s: Found no trigger channels for module %s"%(k, v))
                whined.add(v)
            continue
        mapped2trig[k]=mod2trig[v]
    return mapped2trig

if __name__=='__main__':
    mapped2trig=parse_channels("/u/land/fake_cvmfs/11/extra_jan24p1/upexps/202503_s122")
    #print(mapped2trig)
