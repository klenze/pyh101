PyH101 - a python module for reading the hbook (or h101) STRUCT format. 

[ucesb](https://fy.chalmers.se/~f96hajo/ucesb/) is a very powerful tool for unpacking binary detector data. It is used in several nuclear physics experiments. 

After the data is parsed, it can be mapped. A uint32 which before had a name like ``bus_ts.ts.subsystem_id``, which reflects the parsing tree of the unpacker can be assigned a shorthand name like ``TIMESTAMP_LOS_ID``. 

This mapped data can then be exported from the unpacker in a variety of formats, including ROOT TTree and a custom self-describing STRUCT format.

While there are [experiment frameworks](https://github.com/R3BRootGroup/R3BRoot) which can read data for specific detectors using the STRUCT format, they also tend to introduce a significant complexity overhead.

Thus, for smaller tasks, it is customary to use the unpacker to write a root tree directly. 

Often, the resulting tree is directly loaded with the ROOT interpreter and TTree::Draw is employed to visualize quantities of interest. 

While this has a low complexity overhead in terms of software dependencies, it does not appear to be quite the optimal way to accomplish this task. 

Some shortcomings include:
* There is no mechanism to transmit the events of a TTree piecewise over a Fifo. This means that if one wants to do online processing, one will have to create a file which will grow large over time (plus having a TFile open for writing in one process and reading in another might be non-trivial). 
* TTree::Draw is kind of terrible. For simple cases like ``h101->Draw("EVENTNO")``, it works well enough. In more complex cases, it will fail. For example, if Av and Bv are ``uint32_t`` arrays of different and variable sizes, it seems that ``Av-Bv`` and ``-(Bv-Av)`` will not yield the same results.
* The h101 tree created by rootwriter employs encodes all data into 32bit fields and 32 bit arrays. While this is a very reasonable choice, it does not make the data very convenient to work with. For example, one might not want to write out all the channels for which no data was recorded. ucesb features zero suppression (which I think is really "suppression of data for which no raw data existed"). Using this in your h101 tree will give you three fields:
  * FOO - a uint32 which just contains the number of actual entries in the following arrays (e.g. FOO=2)
  * FOOI - a uint32 array which contains the indices (e.g. FOOI=[23, 42])
  * FOOv - an array of equal size which contains the actual data for each index (e.g. FOOv=[344, 2063], indicating that channel 23 saw a value of 334 and ch 42 saw 2063)
* White Rabbit timestamps have a size of 64 bits, and thus have to be split into multiple parts. This leads to a lot of ``(uint64_t(MY_TIME_STAMP_T4)<<48 | uint64_t(MY_TIME_STAMP_T3)<<32 | MY_TIME_STAMP_T2<<16 | MY_TIME_STAMP_T1)`` which do little to improve the readability of the strings passed to TTree::Draw.

Therefore, PyH101, which aims to read an arbitrary STRUCT source and transform it to high level Python structures. 

At the moment, the project is in the proof-of-concept state. Here is what works:
* Reading all fields from the STRUCT header, and providing a dictionary mapping ``_var_name`` to Python objects where supported. 
* Converting single uint32 (and hopefully int32, to be tested)
* Converting variable length arrays to python lists. 
* Converting zero suppressed data to Python dictionaries. For example, the example above would create a key "FOO" in the main dictionary. For that event, the value associated with that key would be a Python dict {23:334, 42: 2063}
* Converting zero suppressed multi data to a dict of lists. Note that I have not yet checked for the relevant case where a multiplicity is larger than one. 
* Converting white rabbit timestamps. The following rules apply:
  * If the ``TIMESTAMP_FOO_ID`` zero, the timestamp is presumed absent and set to nan. 
  * Otherwise, it will be a numpy.uint64 which hopefully contains the correct WR time. 
  * There is also ``TIMESTAMP_FOO_REL`` which provides a relative timestamp. The first timestamp encountered is set to 10000 (i.e., 10us), and all other relative timestamps are relative to that. The idea is to enable people to always use the same histogram ranges, e.g. [0, 1e9] for one second (from start of data), instead of [1.738111856e18, 1.738111857e18] or so. 

The following is not yet implemented:
* The same 'relative' trick for EVENTNO
* Any features of ``struct_writer`` not encountered in R3B unpackers
   * Floating point data
* Reading from TCP
* Reading from a filename instead of a file descriptor
* Executing an unpacker (with arguments) as a subprocess
* Closing and reopening a h101 object, starting at the first event of a file again

Furthermore, to be really useful for interactive use, a one line "draw quantities to histogram" trick in python would have to be implemented. 

One possible idea would be to use list comprehension (I heard it is ok with python3):
``  myhist=hist((1000, -500, 500), (10, 0, 10]).Fill(lambda: [(k1, k2, SOMEDICT[k1]-OTHERDICT[k2]) for k1 in SOMEDICT.keys() for k2 in OTHERDICT.keys()], myh101, max=100)``

While I would like this better than the omitted indices of TTree::Draw, I still think that there is too much boilerplate. The lambda alone is seven characters, and the ranges of the indices could be implied instead of being explicit. With [MacroPy](https://pypi.org/project/MacroPy/), this could possibly be boiled down to:

``myhist=myh101.mkhist(x=[k1, 1000, -500, 500], y=[k2, 0, 0, 10], w=[SOMEDICT[k1]-OTHERDICT[k2]])``

Still seems likely non-trivial to accomplish. 


Implementation notes:
* This was my first project interacting with hbook ``ext_data_client``
* The ucesb ``ext_data_client`` does not expose the functionality I required ("Map everything, give me the mapping"), so I forked ``ext_data_client.c``. I will try to negotiate about getting my changes merged into ucesb, in which case I would link against ``lib_ext_data_clnt.so``.
* This was also my first Python extension.
* This was also the first time I wrote a setup.py. (shortly before it is deprecated)
* My goal was not to leak memory per event. There are likely some leaks during setup, probably that strdup.
* I use numpy scalars of fixed size (uint32, int32, uint64(rabbits), float32(nan for invalid rabbits))
* Almost all Python structures are allocated before the event loop
   * For zero suppressed multi, I create new keys and the lists associated with them when encountering that key
* I keep a permanent reference to all the python objects created for the lifetime of the H101 object.
* I tried to keep as much structure between events as I could. 
   * The PyObjects pointed to by the main dictionary (mapping from ``_var_name``) stay valid between events. 
   * For everything except the entries of zero-suppressed multi, the component objects stay valid as well. 
   * e.g. dict("MYVEC")[2] will always contain the numpy scalar which holds the second (base zero) value of that vector if the current vector length is at least three. 
   * Yes, the type of the Python object representing a timestamp will alternate between np.float32 (nan) and np.uint64. This is probably a health and safety violation and not really in the spirit of the language. I don't care. 
* I used C++ for the STL classes. Mixing PyObjects with C++ seems nontrivial, the section in the [docs](https://docs.python.org/3/extending/extending.html#writing-extensions-in-c) is rather short. For example, the example C code uses custom allocators. These do not play well with C++ new and delete. I ended up using the python allocator for the creating the object, saving ``ob_base`` on the stack, calling placement new and restoring ``ob_base``. Zero points for elegance there.
* A lot of abstraction breaking, both with regard to h101 and Python. From what I can tell, changing the value of a scalar object is not well supported and requires a lot of ``reinterpret_cast`` operators. 
* Yes, I use one virtual call per variable name per event. If you think that is terrible, you will likely not enjoy the overhead on the Python side of things much. 
* I wish the documentation had been a bit more explicit about always calling ``Py_XINCREF`` if you return a pointer to Python to anything you would prefer to keep around, because the caller discarding the return value will decrease the reference count.
* Tested with a debug version of Python (before I figured out the above fact), compiled with valgrind support. Strongly recommended for any Python.h projects. 

