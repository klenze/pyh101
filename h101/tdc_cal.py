from h101.iteminfo import *
import math

nan=float("nan")

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
            self.mincount=2000
            self.update_interval=2000

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
            cal=self.cals.setdefault(k, finetime_cal())
            ck=tdc_iteminfo._listify(self.coarse[k])
            fk=tdc_iteminfo._listify(self.fine[k])
            for c, f in zip(ck, fk):
                h=tdc_hit((c-cal.cal(f))*self.coarse_period, is_trailing=self.is_trailing)
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
                    prev.trig=trig
                    out.append(prev)
                prev=it
            if len(out)>0: 
                self.res[k]=out
    @staticmethod
    def addFields(myh101):
        d=myh101.getdict()
        added={}
        for k in list(d.keys()):
            if k[-2:]!="CL":
                continue
            if k[0]=='N': # neuland
                continue
            base=k[:-2]
            cl=d.get(base+"CL") 
            fl=d.get(base+"FL")
            if cl==None or fl==None:
                continue
            leading=tdc_iteminfo(base+"_lead", cl, fl)
            ct=d.get(base+"CT")
            ft=d.get(base+"FT")
            if ct==None or ft==None:
                leading.register(myh101, hidden=False)
                added.append[base+"FT"]=leading
                continue
            leading.register(myh101, hidden=True)
            trailing=tdc_iteminfo(base+"_trail", ct, ft, is_trailing=True)
            trailing.register(myh101, hidden=True)
            tot=tot_iteminfo(base+"_tot", leading, trailing)
            tot.register(myh101, hidden=False)
            added[base+"FT"]=tot
        for name, info in added.items():
            if name in myh101.triggermap and myh101.triggermap[name] in added:
                info.trig=added[myh101.triggermap[name]]





