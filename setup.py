#!/usr/bin/python3
import os.path, os
from setuptools import setup, Extension

toplevel=os.path.dirname(__file__)

if not 'UCESB_DIR' in os.environ:
    raise RuntimeError("Environment UCESB_DIR not set.")

ucesb=os.environ['UCESB_DIR']

   

setup(name="h101", ext_modules=[Extension(name="h101", 
                                          sources=["h101module.cxx", "ext_data_client.c"],
                                          extra_compile_args=["-O0","-fno-inline-small-functions", "-g", "-Wno-unused-function", "-Wno-unused-variable", 
                                                      "-Wno-write-strings" # PyArg kw
                                                      ],
                                          include_dirs=[toplevel, ucesb+"/hbook"]
#                                          extra_link_args=["-L${UCESB_DIR}/hbook -lext_data_clnt.so"]
                                          )])


