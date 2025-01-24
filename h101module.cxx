#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <structmember.h> // not included for some reason?!
#include <vector>
#include <map>
#include <string>
#include <cstdio>

#define EXT_DATA_CLIENT_INTERNALS
#include <ext_data_client.h>


#define NPY_NO_DEPRECATED_API NPY_1_7_API_VERSION

#include "numpy/arrayobject.h"
#if 0

#include <numpy/npy_common.h>
#include <numpy/ndarrayobject.h>

#endif

#include <numpy/arrayscalars.h>

static PyModuleDef h101module =
{
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "h101",
    .m_doc = "",
    .m_size = -1,
};




struct iteminfo
{
   uint32_t* length{}; // if zero, indicates a single [u]int32
   bool is_signed{};
   bool is_multi;   // set iff indices are non-unique. 
   uint32_t* src;
   uint32_t* indices;
   PyObject* dest{};
};

struct H101
{
   PyObject_HEAD;
   std::vector<std::string> fieldnames{};
   PyObject* dict {};
   int fd;
   ext_data_client *client;
   std::map<std::string, ext_data_structure_item*> itemmap;
   char* buf{};
   size_t buflen{};
   std::vector<iteminfo> items;
};

static void
H101_dealloc(H101* self)
{
    printf("%s entered\n", __FUNCTION__ );
    if (self->dict)
       Py_XDECREF(self->dict);
    Py_TYPE(self)->tp_free((PyObject *) self);
    printf("%s done\n", __FUNCTION__ );
}

static PyObject * // crashes, unused.
H101_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    printf("%s entered\n", __FUNCTION__ );
   	H101 *self = (H101 *) type->tp_alloc(type, 0);
	if (!self)
	    return nullptr;
	new (self) H101(); // default init
	return (PyObject*) self;
}

static ext_data_structure_item* getIfPresent(std::map<std::string, ext_data_structure_item*> m, std::string key)
{
   auto it=m.find(key);
   if (it==m.end())
	  return nullptr;
   return it->second;
}

#define MAX_PYLONG_SIZE 40 // for storing 32bit signed/unsigned values


// For convenience, we want to change the value of the int pointed to by
// the dictionary. This seems to be a bit messy. 
// PyLong objects are not assignable. 
// ctype are not implicitly convertable
// So we try to copy the actual pylong in a pre-allocated buffer. 

/*
void PyLong_assign(PyObject* dest, PyLongObject src)
{
    if (Py_SIZE(src) < 
}

PyObject* VarPyLong_alloc()
{
   PyObject* res=malloc(MAX_PYLONG_SIZE);
   PyLong_assign
}*/

static int
H101_init(H101 *self, PyObject *args, PyObject *kwds)
{
    printf("%s entered\n", __FUNCTION__ );
    // at some point, we would like to call our c++ constructor.
    auto base=self->ob_base; // don't mess with python
    new (self) H101(); 
    self->ob_base=base;
    char* keywordlist[]={"fd", nullptr};
	if (!PyArg_ParseTupleAndKeywords(args, kwds, "I", keywordlist, &(self->fd)))
		return -1;

        //new (&self->itemmap) decltype(self->itemmap);
	self->dict = PyDict_New();
	Py_XINCREF(self->dict);
	self->client = ext_data_from_fd(self->fd);
	int res{};
#define CHECK(name, exp)\
       	if (res!=exp) \
	{\
		fprintf(stderr,"%s failed with %d, error %s. errno=%d:%s\n", name, \
				res, ext_data_last_error(self->client), errno, strerror(errno));\
		return 1;\
	}
	uint32_t map_success = 0;
	ext_data_structure_info* info = ext_data_struct_info_alloc();
	res=ext_data_setup(self->client, NULL, 0, info, &map_success, 0, "", nullptr); CHECK("setup", 0);

	struct ext_data_structure_item* items=ext_data_struct_info_get_items(info);
	int tot=0;

	while(items)
	{
		if (items->_var_name[0]!='N')
			printf("%s 0x%x, %d\n", items->_var_name, items->_var_type, items->_length);
		self->itemmap[items->_var_name]=items;
		tot+=items->_length;
		items=items->_next_off_item;
	}
	self->buf=(char*)malloc(tot);
	self->buflen=tot;
        auto& m=self->itemmap;

        for (auto& it: m)
	{
	     auto* item = it.second;
	     iteminfo ii;
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
	     if (type==EXT_DATA_ITEM_TYPE_INT32)
		ii.is_signed=1;
	     else if (type==EXT_DATA_ITEM_TYPE_UINT32)
		ii.is_signed=0;
	     else
	     {
		  fprintf(stderr, "Item %s has type %d, which is not supported yet. Ignored.\n", payload->_var_name,
				  type);
		  continue;
	     }
             if (payload==item && item->_length==4)
	     {
	          // a single field. 
		  ii.dest=PyArrayScalar_New(UInt32);
		  ii.src=reinterpret_cast<uint32_t*>(self->buf + item->_offset);
		  Py_XINCREF(ii.dest);
	     }
	     else // unhandled for now
	     {
		  continue;
	     }
	     PyObject* name = PyUnicode_FromString(strdup(item->_var_name));
	     Py_XINCREF(name);
	     PyDict_SetItem(self->dict, name, ii.dest);
	     self->items.push_back(ii);
	     //
	     printf("Mapped %s\n", item->_var_name);
	}

/*	for (int i=0; i<50; i++)
	{
		memset(buf, 0xff, tot);
		res=ext_data_fetch_event(cl, buf, tot, 0); CHECK("fetch", 1);
		printf("%s=%x\n", target,  targetval);
	}

*/

         printf("%s done\n", __FUNCTION__);
	return 0;
}

static PyObject *
H101_getevent(H101* self, PyObject *Py_UNUSED(ignored))
{
     int res=ext_data_fetch_event(self->client, self->buf, self->buflen, 0); 
     if (res!=1)
        return nullptr;
     for (auto& ii: self->items)
     {
	if (!ii.length) // single item
	{
	// for signed, this is not the right data type, we don't care.
	auto p=reinterpret_cast<PyUInt32ScalarObject*>(ii.dest);
	p->obval = *ii.src; 
	}
     }
     return self->dict;
}



static PyMemberDef H101_members[] = {
	{nullptr, 0, 0}
};

static PyMethodDef H101_methods[] =
{
	{"getevent", (PyCFunction)H101_getevent, METH_NOARGS, "Reads the next event."},
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
    H101_type.tp_new = PyType_GenericNew;
#define SetH101(name, cast) H101_type.tp_ ## name = cast H101_ ## name ;
    //SetH101(new,);
    SetH101(init,(initproc));
    //SetH101(dealloc, (destructor));
    SetH101(methods,);
}    


PyMODINIT_FUNC
PyInit_h101(void)
{
    printf("%s entered\n", __FUNCTION__ );
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
