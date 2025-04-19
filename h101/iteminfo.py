import numpy
import traceback

class custom_iteminfo:
    def __init__(self, name, output):
        self.name=name
        self.output=output
        self.buggy=False
    def __call__(self):
        if (self.buggy):
            return False
        try:
            res=self.map_event()
            return res==None or res==True
        except Exception as ex:
            print("Error when trying to calculate %s -- disabled." % self.name)
            print("Exception was: %s"%ex)
            traceback.print_tb(ex.__traceback__)
            print("-------")
            self.buggy=True
            try: 
                self.output.clear()
            except Exception:
                pass
            exit(1)
            return False
    def map_event(self):
        raise RuntimeError("%s: __call__ not overridden."%self)
    def register(self, myh101, hidden=False):
        myh101.addfield(self.name, [self.output, None][hidden], self)

class test_iteminfo(custom_iteminfo):
    def __init__(self):
        super().__init__("test", numpy.array(0))
    def map_event(self):
        self.output+=1


