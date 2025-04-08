from _h101 import *

import numpy, math, sys, os, subprocess
import traceback, copy

if not 'UCESB_DIR' in os.environ:
    raise RuntimeError("Environment UCESB_DIR not set.")

ucesb=os.environ['UCESB_DIR']

nan=float("nan")

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
            print("Error when trying to calculate %s -- disabled.")
            traceback.print_exception(ex)
            self.buggy=True
            try: 
                self.output.clear()
            except Exception:
                pass
            raise ex
            return False


    def map_event(self):
        raise RuntimeError("%s: __call__ not overridden."%self)
    def register(self, myh101):
        myh101.addfield(self.name, self.output, self)


class test_iteminfo(custom_iteminfo):
    def __init__(self):
        super().__init__("test", numpy.array(0))
    def map_event(self):
        self.output+=1

class finetime_cal:
    def __init__(self, finebins=1024):
        # probability mass functions: finebins entries, initially zero
        self.pmf=numpy.array([0.0         ]*finebins, dtype=numpy.float32)
        # cumulative distribution function: finebins entries, initially nan
        self.cdf=numpy.array([float("nan")]*finebins, dtype=numpy.float32)
        self.count=0
        self.finebins=finebins
        self.mincount=1e4           # statistics required before we apply the calibration
        self.update_interval = 2000 # how often should we recalculate the CDF?
        self.stat_lifetime   = 1e5  # how long does a count contribute to the calibration?

        if True:
            self.mincount=500
            self.update_interval=500

    def updateCDF(self):
        """Update the cumulative distribution function from the PMF"""
        if self.count<self.mincount:
            # we do not have enough stats for a calibration. keep returning nans.
            return
        tot=sum(self.pmf)
        s=0
        scale=math.exp(-self.update_interval/self.stat_lifetime)
        for i,p in enumerate(self.pmf):
            s+=p/2/tot
            self.cdf[i]=s
            s+=p/2/tot
            self.pmf[i]*=scale # decay
    def cal(self, raw):
        if raw>=self.finebins:
            return float("nan")
        self.pmf[raw]+=1
        self.count+=1
        if self.count % self.update_interval == 0:
            self.updateCDF()
        return self.cdf[raw]


class tdc_hit:
    def __init__(self, time, tot=nan, is_trailing=False):
        self.time=time
        self.tot=tot
        self.is_trailing=is_trailing
        self.weight=1
    def getTime(self):
        return self.time/self.weight
    def getTot(self):
        return self.tot/self.weight
    def getNorm(self):
        return tdc_hit(self.getTime(), self.GetTot())
    def __iadd__(self, rhs):
        self.time+=rhs.time
        self.tot+=rhs.tot
        self.weight+=rhs.weight
        return self
    def __add__(self, rhs):
        res=copy.copy(self)
        res+=rhs
        return res
    def __sub__(self, rhs):
        return tdc_hit(self.getTime() - rhs.getTime())
    def __str__(self):
        return "tdc_hit%s"%self.__dict__
    def __repr__(self):
        return self.__str__()



class tdc_iteminfo(custom_iteminfo):
    def __init__(self, name, coarse, fine, is_trailing=False):
        self.res=type(coarse)()
        super().__init__(name, self.res)
        self.cals=dict()
        self.coarse=coarse
        self.fine=fine
        self.is_trailing=is_trailing
    @staticmethod
    def _listify(x):
        if type(x)==list:
            return x
        return [x]

    def map_event(self):
        self.res.clear()
        for k in self.coarse.keys():
            out=self.res[k]=[]
            cal=self.cals.setdefault(k, finetime_cal())
            ck=tdc_iteminfo._listify(self.coarse[k])
            fk=tdc_iteminfo._listify(self.fine[k])
            for c, f in zip(ck, fk):
                h=tdc_hit(c-cal.cal(f), is_trailing=self.is_trailing)
                h.coarse=c
                h.fine=f
                out.append(h)
    @staticmethod
    def addFields(myh101):
        d=myh101.getdict()
        for k in list(d.keys()):
            base=k[0:-1]
            if k[-1]=="C" and base+"F" in d:
                c=d[k]
                f=d[base+"F"]
                tdc_iteminfo(base, c, f).register(myh101)


class tot_iteminfo(custom_iteminfo):
    def __init__(self, name, leading, trailing):
        self.res=type(leading.res)()
        super().__init__(name, self.res)
        self.leading=leading
        self.trailing=trailing
    def map_event(self):
        self.res.clear()
        for k, le in self.leading.res.items():
            out=self.res[k]=[]
            tr=self.trailing.res.get(k, [])
            items=sorted(le+tr, key=lambda a:a.time)
            last=None
            for it in items:
                if it.is_trailing and last and not last.is_trailing:
                    # also changes object in leading array, is fine.
                    last.tot=it.getTime()-last.getTime()
                    out.append(last)
                last=it
    @staticmethod
    def addFields(myh101):
        d=myh101.getdict()
        for k in list(d.keys()):
            if k[-2:]!="CL":
                continue
            base=k[:-2]
            cl=d.get(base+"CL") 
            fl=d.get(base+"FL")
            ct=d.get(base+"CT")
            ft=d.get(base+"FT")
            if cl==None or fl==None or ct==None or ft==None:
                print("Ignored %s, [cl,fl,ct,ft]=%s"%(base, [cl, fl, ct,ft]))
                continue
            leading=tdc_iteminfo(base+"_lead", cl, fl)
            leading.register(myh101)
            trailing=tdc_iteminfo(base+"_trail", ct, ft, is_trailing=True)
            trailing.register(myh101)
            tot=tot_iteminfo(base+"_tot", leading, trailing)
            tot.register(myh101)

def mkh101(inputs, unpacker=None):
        if unpacker == None:
            if not 'EXP_NAME' in os.environ:
                raise RuntimeError("No unpacker specified, and EXP_NAME is not set.")
            unpacker=os.environ["EXP_NAME"]
        if unpacker.find("/") == -1:
            upexps="%s/../upexps/%s/%s"%(ucesb, unpacker, unpacker)
        else:
            upexps=unpacker
        upexps+=" %s --quiet --ntuple=RAW,STRUCT,-"%inputs
        print("Running unpacker: %s"%upexps)
        sp=subprocess.Popen(upexps, shell=True,
                            stdout=subprocess.PIPE)
        res=H101(fd=sp.stdout.fileno())
        t=test_iteminfo()
        t.register(res)
        tdc_iteminfo.addFields(res)
        tot_iteminfo.addFields(res)
        return res
 

