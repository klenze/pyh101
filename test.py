#!/usr/bin/python3
import numpy
import h101
f=open("events.struct", "br")
h=h101.H101(fd=f.fileno())

locals().update(h.getevent())
for i in range(5):
   h.getevent()
   print(EVENTNO)


