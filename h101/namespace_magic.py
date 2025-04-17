#!/usr/bin/env python3

class namespace_magic:
    def __init__(self, yourglobals):
        self.g=None
        self.globals=yourglobals
    def update(self, *args, **kwargs):
        assert self.g!=None, "only modify globals in context!"
        self.globals.update(*args, **kwargs)
    def __enter__(self):
        self.g=self.globals.copy()
        return self
    def __exit__(self, *args, **kwargs):
        self.globals.clear()
        self.globals.update(self.g)
        self.g=None

