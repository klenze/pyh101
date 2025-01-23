#!/usr/bin/python3

from setuptools import setup, Extension
setup(name="h101", ext_modules=[Extension(name="h101", 
                                          sources=["h101module.cxx"],
                                          extra_link_args=["-L${UCESB_DIR}/hbook -lext_data_clnt.so"])])


