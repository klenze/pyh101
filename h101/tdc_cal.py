from h101.iteminfo import *
import math
import random
import re
import json
nan=float("nan")

mincount=1e4           # statistics required before we apply the calibration
update_interval = 2000 # how often should we recalculate the CDF?
stat_lifetime   = 1e5  # how long does a count contribute to the calibration?

allcals={}

def readcals(filename):
    global allcals
    with open(filename, "r") as f:
        allcals=json.load(f)


def writecals(filename):
    with open(filename, "w") as f:
        json.dump(allcals, f)

calinfo=[False, False] # what have we already whined about?

class finetime_cal:
    def __init__(self, name, finebins=1024):
        global calinfo
        # probability mass functions: finebins entries, initially zero
        self.pmf=numpy.array([0.0         ]*finebins, dtype=numpy.float32)
        # cumulative distribution function: finebins entries, initially nan
        self.name=name
        if name in allcals:
            self.cdf=numpy.array(allcals[name], dtype=numpy.float32)
            if calinfo[0]:
                  print("Using loaded calibrations for %s"%name)
                  calinfo[0]=True
        else:
            self.cdf=numpy.array([float("nan")]*finebins, dtype=numpy.float32)
            if not calinfo[1]:
                calinfo[1]=True
                print("Starting with a blank calibration for %s. It will take some time before calibrated data will be available"%name)
        print("Calibration for %s is [%s...]"%(name, self.cdf[0:3]))
        self.count=0
        self.finebins=finebins

    def updateCDF(self):
        """Update the cumulative distribution function from the PMF"""
        tot=sum(self.pmf)
        s=0
        scale=math.exp(-update_interval/stat_lifetime)
        for i,p in enumerate(self.pmf):
            s+=p/tot
            self.cdf[i]=s
            self.pmf[i]*=scale # decay
        assert 0.99<s and s<1.01, "bad sum: %f"%s
        allcals[self.name]=list(map(float, self.cdf))
    def cal(self, raw):
        if raw>=self.finebins or raw<=0:
            return float("nan")
        self.pmf[raw]+=1
        self.count+=1
        if self.count<mincount:
            pass # use previous calibration of loaded, but do not update yet
        elif self.count % update_interval == 0:
            self.updateCDF()
        res=random.uniform(self.cdf[raw-1], self.cdf[raw])
        assert math.isnan(res) or (0<=res and res<=1), "Illegal calibrated fine time: %s"%res
        return res

class tdc_hit:
    def __init__(self, time=None, tot=nan, trig=nan, is_trailing=False):
        if time!=None:
            self.time=time
            self.weight=1
        else:
            self.time=0.0
            self.weight=0
        self.tot=tot
        self.is_trailing=is_trailing
        self.trig=trig
    def getTime(self):
        return self.time/self.weight
    def getTrig(self):
        return self.trig/self.weight
    def getTot(self):
        return self.tot/self.weight
    def normalize(self):
        self.time/=self.weight
        self.tot/=self.weight
        self.trig/=self.weight
    def getNorm(self):
        res=self.copy()
        res.normalize()
        return res
    def subTrig(self):
        self.time-=self.trig
        self.trig=0.0
    def getTrigSub(self):
        res=self.copy()
        res.subTrig()
        return res
    def __iadd__(self, rhs):
        self.time+=rhs.time
        self.tot+=rhs.tot
        self.trig+=rhs.trig
        self.weight+=rhs.weight
        return self
    def __add__(self, rhs):
        res=copy.copy(self)
        res+=rhs
        return res
    def __sub__(self, rhs):
        return tdc_hit(self.getTime() - rhs.getTime(), trig=self.getTrig()-rhs.getTrig())
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
        self.coarse_period   = 5 #ns
    @staticmethod
    def _listify(x):
        if type(x)==list:
            return x
        return [x]

    def map_event(self):
        self.res.clear()
        for k in self.coarse.keys():
            out=self.res[k]=[]
            if not k in self.cals:
                self.cals[k]=finetime_cal(self.name+str(k))
            cal=self.cals[k]
            ck=tdc_iteminfo._listify(self.coarse[k])
            fk=tdc_iteminfo._listify(self.fine[k])
            for c, f in zip(ck, fk):
                h=tdc_hit((c-cal.cal(f))*self.coarse_period, is_trailing=self.is_trailing)
                h.coarse=c
                h.fine=f
                out.append(h)
    @staticmethod
    def addFields(myh101):
        count=0
        d=myh101.getdict()
        for k in list(d.keys()):
            base=k[0:-1]
            if not filterre.match(base):
                continue
            if k[-1]=="C" and (f:=d.get(base+"F"))!=None:
                c=d[k]
                tdc_iteminfo(base, c, f).register(myh101)
                count+=1
        return count

filterre=re.compile(".*")

class tot_iteminfo(custom_iteminfo):
    def __init__(self, name, leading, trailing, trig=None):
        self.res=type(leading.res)()
        super().__init__(name, self.res)
        self.leading=leading
        self.trailing=trailing
        self.trig=trig
    def map_event(self):
        self.res.clear()
        for k, le in self.leading.res.items():
            out=[]
            tr=self.trailing.res.get(k, [])
            trig=nan
            if self.trig and len(self.trig.res.get(k, []))==1:
               trig=self.trig.res[k][0]
            items=sorted(le+tr, key=lambda a:a.time)
            prev=None
            for it in items:
                if it.is_trailing and prev and not prev.is_trailing:
                    # also changes object in leading array, is fine.
                    prev.tot=it.getTime()-prev.getTime()
                    prev.totc=5*(it.coarse - prev.coarse)
                    prev.trig=trig
                    out.append(prev)
                prev=it
            if len(out)>0: 
                self.res[k]=out
    @staticmethod
    def addFields(myh101):
        count=0
        d=myh101.getdict()
        added={}
        for k in list(d.keys()):
            if k[-2:]!="CL":
                continue
            base=k[:-2]
            if not filterre.match(base):
                continue
            cl=d.get(base+"CL") 
            fl=d.get(base+"FL")
            if cl==None or fl==None:
                continue
            ct=d.get(base+"CT")
            ft=d.get(base+"FT")
            if ct==None or ft==None:
                leading=tdc_iteminfo(base, cl, fl)
                leading.register(myh101, hidden=False)
                count+=1
                print("registered %s"%sbase)
                added.append[base+"FT"]=leading
                continue
            leading=tdc_iteminfo(base+"_lead", cl, fl)
            leading.register(myh101, hidden=True)
            trailing=tdc_iteminfo(base+"_trail", ct, ft, is_trailing=True)
            trailing.register(myh101, hidden=True)
            tot=tot_iteminfo(base+"_tot", leading, trailing)
            tot.register(myh101, hidden=False)
            count+=1
            added[base+"FT"]=tot
        for name, info in added.items():
            if name in myh101.triggermap and myh101.triggermap[name] in added:
                info.trig=added[myh101.triggermap[name]]
        return count





