#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h> // not included for some reason?!

#include <vector>
#include <map>
#include <unordered_map>
#include <array>
// todo: find use cases for all the other STL containers :-P

#include <string>
#include <cstdio>
#include <assert.h>


#define EXT_DATA_CLIENT_INTERNALS
#include <ext_data_client.h>


#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include <numpy/arrayobject.h>
#include <numpy/arrayscalars.h>

#define CHECK( expr, fail, fmt, ...) \
	do { if(!(expr)) { \
	     fprintf(stderr,"%s:%d in %s: ", 	__FILE__, __LINE__, __FUNCTION__); \
	     fprintf(stderr, fmt, __VA_ARGS__);	\
	     fprintf(stderr, "\n");	\
	     return fail; \
	 }} while (0)

#define CHECK_EXT(expr, fail, name) \
	CHECK(expr, fail, "%s failed with %d. ext_data error %s. errno=%d:%s\n", \
	     name, res, ext_data_last_error(self->client), errno, strerror(errno))

#define RFAIL 1

#define noerrno errno=0 // Python keeps errno=25 around. not very polite. 

static PyModuleDef h101module =
{
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "h101",
    .m_doc = "",
    .m_size = -1,
};


struct base_iteminfo
{
   uint32_t max_values;
   std::vector<PyObject*> value_list; // contains preconstructed items, then other stuff we still have a reference to. 
   const char* name=nullptr;

   base_iteminfo(int max_values_, bool is_signed)
    :   max_values(max_values_)
    ,   value_list(max_values_)
   {
	for (auto& v: this->value_list)
	{
	   if (!is_signed)
    	      v=PyArrayScalar_New(UInt32);
	   else
           
    	      v=PyArrayScalar_New(Int32);
	   Py_XINCREF(v);
	}
   }

   template<typename T>
   T* register_obj(T* obj)
   {
       PyObject* p=reinterpret_cast<PyObject*>(obj);
       Py_XINCREF(p);
       this->value_list.push_back(p);
       return obj;
   }

   virtual ~base_iteminfo()
   {
	for (auto& v: this->value_list)
	    Py_XDECREF(v);
   }
   virtual int map_event() = 0;
   virtual PyObject* get_obj() = 0;
};


struct xint32_iteminfo: public base_iteminfo
{
    uint32_t* src;
    PyUInt32ScalarObject* dest;

    xint32_iteminfo(uint32_t* src_, bool is_signed)
	    : base_iteminfo(1, is_signed)
	    , src(src_)
	    ,  dest(reinterpret_cast<PyUInt32ScalarObject*>
		(this->value_list.at(0)))
    {
    }
    int map_event() override
    {
	this->dest->obval = *this->src; 
	return 0;
    }

    PyObject* get_obj() override
    {
         return reinterpret_cast<PyObject*>(dest);
    }

};

struct vector_iteminfo: public base_iteminfo
{
   uint32_t* length;
   uint32_t* data;
   PyObject* list;

   vector_iteminfo(int maxlen, uint32_t* length_,
  	 	   uint32_t* data_, bool is_signed)
   : base_iteminfo(maxlen, is_signed)
	, length(length_)
	, data(data_)
	, list(register_obj(PyList_New(0)))
   {
   }
   int map_event() override
   {
        uint32_t len=*length;
	uint32_t last_len = PyList_Size(this->list);
	CHECK( len<=max_values, RFAIL, "Illegal length: %d > %d",len, max_values);

	int res {};
	//first, resize the output list by removing or 
	// adding payload objects:
	if (len<last_len)
	     res|=PyList_SetSlice(this->list, len, last_len, nullptr);
	else if (last_len<len)
	   for(uint32_t i=last_len; i<len && ! res; i++)
	     res|=PyList_Append(this->list, this->value_list[i]);
        CHECK(res==0, RFAIL, "PyList ops: res=%d", res);

        // now, simply copy the data to the python objects
	for (uint32_t i=0; i<len; i++)
	    {
		auto* p=reinterpret_cast<PyUInt32ScalarObject*>(this->value_list[i]);
		p->obval = this->data[i];
	    }
	return 0;
   }
   PyObject* get_obj() override
   {
      return list;
   }
};

struct dict_iteminfo: public base_iteminfo
{
   dict_iteminfo(int maxlen, 
		 uint32_t* length_,
 	         uint32_t* keys_, 
		 uint32_t* data_, 
		 bool is_signed)
  : base_iteminfo(2*maxlen, is_signed)
  , length(length_)
  , keys(keys_)
  , data(data_)
  , maxlen(maxlen)
  , dict(register_obj(PyDict_New()))
  {
      //the first half of value_list are simply the keys:
      for (int i=0; i<maxlen; i++)
	reinterpret_cast<PyUInt32ScalarObject*>(this->value_list[i])->obval=i;
  }
   int map_event() override
   {
	PyDict_Clear(this->dict);
	auto len=*this->length;
	CHECK( len<=maxlen, RFAIL, "Illegal length: %d > %d",len, maxlen);
	for (uint32_t i=0; i<len; i++)
	{
	   uint32_t key=keys[i];
	   uint32_t d=data[i];
	   auto pyKey=this->value_list.at(key);
	   CHECK(PyDict_Contains(this->dict, pyKey)==0, RFAIL, "Duplicate key %d or broken dict!", key);
	   auto pyVal=this->value_list.at(key+maxlen);
	   reinterpret_cast<PyUInt32ScalarObject*>(pyVal)->obval=d;
	   int res=PyDict_SetItem(this->dict, pyKey, pyVal);
	   CHECK(res==0, RFAIL, "PyDict_SetItem returned %d", res);
	   //printf("dict item in %s: %d!\n", name, d);
	}
	return 0;
   }

   PyObject* get_obj() override
   {
      return dict;
   }


   uint32_t* length;
   uint32_t* keys;
   uint32_t* data;
   uint32_t maxlen; // 
   PyObject* dict;
};



struct mult_iteminfo: public base_iteminfo
{
    uint32_t *length, *keys, *data;
    using dictentry_t=std::pair<PyObject*, PyObject*>; // first is key (in outer dict), 2nd list
    std::unordered_map<uint32_t, dictentry_t> key2list;
    PyObject* dict;
    std::vector<PyObject*> current_lists;
    mult_iteminfo(uint32_t maxlen,
		   uint32_t* length_, uint32_t* keys_, uint32_t* data_, bool is_signed)
	    : base_iteminfo(maxlen, is_signed)
	    , length(length_)
	    , keys(keys_)
	    , data(data_)
	    , dict(register_obj(PyDict_New()))
    {
    }

    PyObject* find_or_make_list(uint32_t key)
    {
	auto& m=this->key2list;
	auto it=m.find(key); 
	if (it==m.end())
	{
	  PyObject* pykey=register_obj(PyArrayScalar_New(UInt32));
	  reinterpret_cast<PyUInt32ScalarObject*>(pykey)->obval=key;
	  auto* list=register_obj(PyList_New(0));
	  m.emplace(key, std::make_pair(pykey,list));
	  it=m.find(key);
	  assert(it!=m.end());
	}
	PyObject* pykey=it->second.first;
	PyObject* list=it->second.second;
	PyDict_SetItem(this->dict, pykey, list);
	this->current_lists.push_back(list);
	return list;
    }

    int map_event() override
    {
	// always clear all nonempty lists in the beginning
	// if the user has a previously obtained reference to any particular list
	// there should not be data from prior events in it. 
	for (auto p:this->current_lists)
	    PyList_SetSlice(p, 0, PY_SSIZE_T_MAX, NULL); // aka PyList_Clear
	this->current_lists.clear();
	PyDict_Clear(this->dict);

        uint32_t len=*length;
	CHECK( len<=max_values, RFAIL, "Illegal length: %d > %d",len, max_values);
	uint32_t last_key=~keys[0];
	PyObject* list{};
	for (uint32_t i=0; i<len; i++)
	{
	   auto k=keys[i];
	   if (k!=last_key)
	   {
		   list=find_or_make_list(k);
	   }
	   PyObject* pyVal=this->value_list.at(i);
	   reinterpret_cast<PyUInt32ScalarObject*>(pyVal)->obval=data[i];
	   PyList_Append(list, pyVal);
	}
	return 0;
    }

    PyObject* get_obj() override
    {
       return dict;
    }
};

struct wrts_iteminfo: public base_iteminfo
{
	uint32_t *id;
	using tn_t = std::array<uint32_t*, 4>;
	tn_t tn; // pointers to t1..t4
	uint64_t* rel; // if non-zero, global relative offset
        PyObject* dest;
	PyTypeObject* np64;
	wrts_iteminfo(uint32_t* id_, tn_t tn_, uint64_t* rel_)
	: base_iteminfo(0, false)
	, id(id_)
	, tn(tn_)
	, rel(rel_)
	, dest(register_obj(PyArrayScalar_New(UInt64)))
	, np64(dest->ob_type)
	{
	}

       void nanify()
       {
	   static PyObject* np_nan;
	   if (!np_nan)
	   {
		np_nan=PyArrayScalar_New(Float32);
		reinterpret_cast<PyFloat32ScalarObject*>(np_nan)->obval=nanf("");
	   }
	   dest->ob_type=np_nan->ob_type;
	   reinterpret_cast<PyFloat32ScalarObject*>(dest)->obval=nanf("");
       }

       int map_event() override
       {
	    if (*id==0)
	    {
		nanify();
		return 0;
	    }
	    dest->ob_type=np64; //valid value
	    uint64_t res{};
	    for (int i=0; i<4; i++)
		 res+=uint64_t(*(tn[i]))<<(8*i);
	    if (rel) // we want to use relative white rabbits
	    {
		if (*rel==0) // base has not been set yet
		{
		     *rel=res-10000; // start from counting 10us ago
		}
		res-=*rel;
	    }
	    reinterpret_cast<PyUInt64ScalarObject*>(dest)->obval=res;
	    return 0;
       }

       PyObject* get_obj() override
       {
	  return dest;
       }
};

struct H101
{
  PyObject ob_base;
//   PyObject_HEAD;
   int fd;
   std::vector<std::string> fieldnames{};
   PyObject* dict {};
   ext_data_client *client;
   std::map<std::string, ext_data_structure_item*> itemmap;
   char* buf{};
   size_t buflen{};
   std::vector<base_iteminfo*> items;
   uint64_t relwr_base{}; // offset for 'relative white rabbit'. 
};

static ext_data_structure_item* getIfPresent(std::map<std::string, ext_data_structure_item*> m, std::string key)
{
   auto it=m.find(key);
   if (it==m.end())
	  return nullptr;
   //printf("%s: %s ->%p\n", __FUNCTION__, key.c_str(), it->second);
   return it->second;
}


#define GETPTR(name) (reinterpret_cast<uint32_t*>(self->buf + name->_offset))


static void
pythonize_reg_item(H101* self, const char* str, base_iteminfo* mapped)
{
	PyObject* name = PyUnicode_FromString(strdup(str));
        //Py_XINCREF(name);
    	mapped->name=str;
	PyDict_SetItem(self->dict, name, mapped->get_obj());
	self->items.push_back(mapped);
}

static void pythonize_wrts(H101* self, const std::string& base)
{
   auto& m = self->itemmap;
   std::string idname=base+"_ID";
   auto* id=getIfPresent(m, idname);
   if (!id)
   {
       printf("%s not found!\n", idname.c_str());
       return;
   }
   uint32_t* idptr=GETPTR(id);
   wrts_iteminfo::tn_t tn;
   const static std::string suffixes[]={"1", "2", "3", "4"};
   for (int i=0; i<4; i++)
   {
     auto* timeword=getIfPresent(m, base+"_WR_T"+suffixes[i]);
     if (!timeword)
	return;
     tn[i]=GETPTR(timeword);
   }

   std::string wrtsname=base;
   pythonize_reg_item(self, base.c_str(), new wrts_iteminfo(idptr, tn, nullptr));
   
   std::string relname=base+"_REL";
   pythonize_reg_item(self, relname.c_str(), new wrts_iteminfo(idptr, tn, &(self->relwr_base)));
}



static void pythonize(H101* self)
{
	auto& m = self->itemmap;
        for (auto& it: m)
	{
	     auto* item = it.second;
	     base_iteminfo* mapped{};

	     if (strcmp("", item->_var_ctrl_name)) // we handle the var length arrays by their length param
		continue;
             if (!item->_length%4)
	     {
		fprintf(stderr, "Item %s has a length of %d, which is not a multiple of 4. Ignored.\n", item->_var_name,
			       	item->_length);
		continue;
	     }
	     auto* suffixI=getIfPresent(m, it.first+"I");
	     auto* suffixE=getIfPresent(m, it.first+"E");
	     auto* suffixv=getIfPresent(m, it.first+"v");
	     auto* payload=item;
	     if (suffixv)
		     payload=suffixv;
	     else if (suffixE)
		     payload=suffixE;

	     uint32_t type = payload->_var_type &EXT_DATA_ITEM_TYPE_MASK ;
	     bool is_signed;
	     if (type==EXT_DATA_ITEM_TYPE_INT32)
		is_signed=1;
	     else if (type==EXT_DATA_ITEM_TYPE_UINT32)
		is_signed=0;
	     else
	     {
		  fprintf(stderr, "Item %s has type %d, which is not supported yet. Ignored.\n", payload->_var_name,
				  type);
		  continue;
	     }
             if (payload==item && item->_length==4)
	     {
		  int len=it.first.size();
		  if (it.first.substr(0, 9)=="TIMESTAMP" && it.first.substr(len-3, len)=="_ID")
			pythonize_wrts(self, it.first.substr(0, len-3));

	          // a single field. 
	          mapped=new xint32_iteminfo(GETPTR(item), is_signed);
	     }
	     else if (payload==suffixv && !suffixI)
	     {
		 // a simple variable length array. 
		 mapped=new vector_iteminfo(payload->_length, GETPTR(item), GETPTR(payload), is_signed);
	     }
	     else if (payload==suffixv && suffixI)
	     {
		 //continue;
		 mapped=new dict_iteminfo(payload->_length, GETPTR(item), GETPTR(suffixI), GETPTR(suffixv), is_signed);
	     }
	     else if (payload==suffixE && suffixI)
	     {
		mapped=new mult_iteminfo(payload->_length, GETPTR(item), GETPTR(suffixI), GETPTR(suffixE), is_signed);
	     }
	     else // unhandled for now
	     {
		     continue;
		  printf("Ignored object %s with len=%d I=%d E=%d v=%d\n", item->_var_name, payload->_length,
			  !! suffixI, !! suffixE, !! suffixv);
		  continue;
	     }
	     pythonize_reg_item(self, item->_var_name, mapped);
	     //
	     //printf("Mapped %s\n", item->_var_name);
	}

}

#undef GETPTR

static void
H101_dealloc(H101* self)
{
    printf("%s entered\n", __FUNCTION__ );
    if (self->dict)
       Py_XDECREF(self->dict);
    if (self->buf) free(self->buf);
    for (auto v: self->items)
	   delete v;
    // todo: who owns the ext_data_structure_item?
    if (self->client) free(self->client);

    PyObject saved=self->ob_base;
    self->~H101();  // call the destructor.  
    self->ob_base=saved;
    Py_TYPE(self)->tp_free((PyObject *) self);
    printf("%s done\n", __FUNCTION__ );
}

static PyObject *
H101_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
	printf("%s entered\n", __FUNCTION__ );
   	H101 *self = (H101 *) type->tp_alloc(type, 0);
	if (!self)
	    return nullptr;
	assert((void*)self == (void*)(&self->ob_base)); // no vtable plz

	// call the C++ constructor without destroying the python header
	// there has to be a better way for this
	PyObject saved=self->ob_base;
	new (self) H101(); // default init
	self->ob_base=saved;
        printf("%s exiting\n", __FUNCTION__ );
	return (PyObject*) self;
}

/* PyObject* VarPyLong_alloc()
{
   PyObject* res=malloc(MAX_PYLONG_SIZE);
   PyLong_assign
}*/


static int
H101_init(H101 *self, PyObject *args, PyObject *kwds)
{
    noerrno;
    printf("%s entered\n", __FUNCTION__ );
    // at some point, we would like to call our c++ constructor.
    auto base=self->ob_base; // don't mess with python
    //new (self) H101(); 
    self->ob_base=base;
    char* keywordlist[]={"fd", nullptr};
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "I", keywordlist, &(self->fd)))
		return -1;

        //new (&self->itemmap) decltype(self->itemmap);
	self->dict = PyDict_New();
	Py_XINCREF(self->dict);
	self->client = ext_data_from_fd(self->fd);
	int res{};
	uint32_t map_success = 0;
	printf("errno=%d\n", errno);
	ext_data_structure_info* info = ext_data_struct_info_alloc();
	printf("errno=%d\n", errno);
	res=ext_data_setup(self->client, NULL, 0, info, &map_success, 0, "", nullptr);
       	CHECK_EXT(res==0, RFAIL, "setup");

	struct ext_data_structure_item* items=ext_data_struct_info_get_items(info);
	int tot=0;

	while(items)
	{
		if (0 && items->_var_name[0]!='N')
			printf("%s 0x%x, %d\n", items->_var_name, items->_var_type, items->_length);
		self->itemmap[items->_var_name]=items;
		tot+=items->_length;
		items=items->_next_off_item;
	}
	self->buf=(char*)malloc(tot);
	self->buflen=tot;
        auto& m=self->itemmap;

        pythonize(self);
        printf("%s done\n", __FUNCTION__);
	return 0;
}

static PyObject *
H101_getevent(H101* self, PyObject *Py_UNUSED(ignored))
{
     noerrno;
     int res=ext_data_fetch_event(self->client, self->buf, self->buflen, 0); 
     if (res==0)
     {
	Py_XINCREF(Py_False);
	return Py_False;
     }
     
     CHECK_EXT(res==1, nullptr, "fetch_event");
     for (auto& ii: self->items)
     {
	ii->map_event();
     }
     Py_XINCREF(Py_True);
     return Py_True;
}
static PyObject *
H101_getdict(H101* self, PyObject *Py_UNUSED(ignored))
{
	//printf("ref count: %p -> %d\n", self->dict, Py_REFCNT(self->dict));
	Py_XINCREF(self->dict);
	return self->dict;
}

static PyMemberDef H101_members[] = {
	{nullptr, 0, 0}
};

static PyMethodDef H101_methods[] =
{
	{"getevent", (PyCFunction)H101_getevent, METH_NOARGS, "Reads the next event."},
	{"getdict", (PyCFunction)H101_getdict, METH_NOARGS, "Get the dictionary of parsed h101 fields"},
	{nullptr}
};
static PyTypeObject H101_type
{
	// fields initialized in PyInit_h101, because C++
};

void mkH101_type()
{
//    H101_type.ob_base.ob_base =  PyObject{_PyObject_EXTRA_INIT, 1, nullptr};
//    H101_type.ob_base.ob_size = 0;
//  C++ really contstraints struct initialization even within an extern "C" block, so
//  we do it the stupid way:
    memset(&H101_type, 0, sizeof(H101_type));
    H101_type.tp_basicsize = sizeof(H101);
    H101_type.tp_itemsize = 0;
    H101_type.tp_name = "h101.H101";
    H101_type.tp_doc = PyDoc_STR("A h101 object created from a STRUCT");
    H101_type.tp_flags = Py_TPFLAGS_DEFAULT;
    //H101_type.tp_new = H101_new ; //PyType_GenericNew;
#define SetH101(name, cast) H101_type.tp_ ## name = cast H101_ ## name ;
    SetH101(new,);
    SetH101(init,(initproc));
    SetH101(dealloc, (destructor));
    SetH101(methods,);
}    


PyMODINIT_FUNC
PyInit_h101(void)
{
    noerrno;
    printf("%s entered :-)\n", __FUNCTION__ );
    import_array();
    mkH101_type();
    if (PyType_Ready(&H101_type)<0) return nullptr;

    PyObject *m = PyModule_Create(&h101module);
    if (!m) return nullptr;

   if (PyModule_AddObject(m, "H101", reinterpret_cast<PyObject*>(&H101_type))<0)
    {
	Py_DECREF(m);
	return nullptr;
    }
    printf("initialized module\n");   
    return m;
}
