The lifetime of all of the values in the top dictionary is the lifetime of the h101 object. If you have

```
h=h101.mkh101(...)
d=h.getdict()
```

Then the following will refer to the same object over the lifetime of ``h`` (i.e. calling the id function on them will alway return the same value):
```
  h.getdict()        # aka d
  d["TRIGGER"]       # single value, some np uint32 tyle
  d["TPAT"]          # variable-sized array converted to a list
  d["LOS1VT"]        # time-calibrated LOS VFTX data
  d["TIMESTAMP_BUS"] # assembled white rabbit timestamp
```
By contrast, keeping reference to any of the following over a ``h.getevent()`` is undefined behavior:
```
  d["TPAT"][0]
  d["LOS1VT"][1]    # list of VFTX hits for channel 1
  d["LOS1VT"][1][3] # fourth VFTX hit for channel 1
```
They might still contain the value of a previous hit, contain some unrelated information for the current hit or make demons come out of your nose when dereferenced.[^1] 

If you are used to programming python, beware that integer objects such as ``d["TRIGGER"]`` behave differently form normal python integers. Under the hood, normal Python ints are references to constant integers. If you have
```
 x=42   # L1
 l=[x]  # L2
 x=0    # L3
```
then l will be ``[42]``, not ``[0]``, because (L1) ``x`` was set to point to a constant integer object containing 42, then (L2) that reference to the constant integer object 42 was copied into the list, then (L3) ``x`` was set to point to another integer constant, which did not affect ``l`` in the least. Even if we replace L3 with ``x+=1``, this will just make ``x`` point to another integer object which contains the incremented int.

This means that Python ints will behave in a similar way as C ints, even though the precise mechanism is different: C will just copy the value into the int on assignment, while Python copies a reference, but to an immutable int object. Also note that other objects, such as lists, are mutable in Python:

```
 x=[]         # L1
 l=[]         # L2
 l.append(x)  # L3 
 l.append(x)  # L4
 x.append(42) # L3
```
will of course yield ``l==[[42], [42]]``, because ``l`` just contains two references to the list allocated in L1.

The other day, I tried to store trigger types in a list for multiple events.
I did something like:
```
   l.append(d["TRIGGER"])
```
Naturally, that resulted in l containing identical values, e.g. ``l==[1, 1, 1, ...]`` or ``l==[3, 3, 3, ...]``, because l just had a zillion references to the mutable integer which was pointed to by ``d["TRIGGER"]``.

What I should have done is
```
     from copy import deepcopy
     ...
     l.append(deepcopy(d["TRIGGER"])
```
(In this case, ``copy.copy`` would also suffice, but if you want to keep lists or dicts over events you need ``copy.deepcopy``).

[^1]: In the future, I plan to make ``d["LOS1VT"][1]`` defined over the lifetime of h. The idea would be that before the first ``h.getevent()``, ``d["LOS1VT"]`` is ``{1:[], 2:[], ...}``, so the user can pick the list containing the hits in their channel of interest. Once ``h.getevent()`` is executed, the dictionary will still only contain the channels with have non-empty hitlists, but for all the empty channels the corresponding lists would be guaranteed to be empty (if you kept a reference to them).
