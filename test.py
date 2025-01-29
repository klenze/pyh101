#!/usr/bin/env python3
import numpy
import h101
f=open("events2.struct", "br")


h=h101.H101(fd=f.fileno())



d=h.getdict()
# this puts all the variables in our namespace. 
globals().update(d)
# so if a field EVENTNO exists, we can simply say
print(EVENTNO) # zero, because no event is loaded yet.

print(d)

def logfield(name):
    print("%s = %s"%(name, globals()[name]))
n=0
while(h.getevent()):
   print("--------------------")
   logfield("EVENTNO")
   logfield("TIMESTAMP_MASTER_REL")
   logfield("TIMESTAMP_LOS_REL")
   logfield("TIMESTAMP_CHIMERA_REL")
   logfield("TIMESTAMP_KRAB_REL")
   logfield("KRAB_SAMP_TBOX")
   n+=1
   if (n>100):
       break
   #for k,v in d.items():
   #    if type(v)==dict and len(v)!=0:
   #        print(k, v)
#


