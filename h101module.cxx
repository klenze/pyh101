#define PY_SSIZE_T_CLEAN
#include <Python.h>

#include <vector>
#include <string>
#include <cstdio>

#include <ext_data_clnt.hh>

static PyModuleDef h101module =
{
    .m_base = PyModuleDef_HEAD_INIT,
    .m_name = "h101",
    .m_doc = "",
    .m_size = -1,
};

struct H101
{
   PyObject_HEAD;
   std::vector<std::string> fieldnames{};
   PyObject* dict {};
   int fd{-1};
   ext_data_clnt *clnt {};
};

static void
H101_dealloc(H101* self)
{
    if (self->dict)
       Py_XDECREF(self->dict);
    Py_TYPE(self)->tp_free((PyObject *) self);
}

static PyObject *
H101_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
   	H101 *self = (H101 *) type->tp_alloc(type, 0);
	if (!self)
	    return nullptr;
	new (self) H101(); // default init
	return (PyObject*) self;
}

static int
H101_init(H101 *self, PyObject *args, PyObject *kwds)
{
	if (!PyArgParse(args, "i", &(self->fd)))
	    return -1;
	self->dict = PyDict_New();
	PyXINCREF(self->dict);
        self->clnt = new ext_data_clnt();
	self->clnt->connect(self->fd);
	return 0;
}

static PyObject *
H101_getevent(H101* self, PyObject *Py_UNUSED(ignored))
{
	
}

static PyMemberDef H101_members[] = {
	{nullptr, 0, 0},
}



static PyTypeObject H101_type
{
// fields initialized in PyInit_h101, because C++
};

PyMODINIT_FUNC
PyInit_h101(void)
{
//    H101_type.ob_base.ob_base =  PyObject{_PyObject_EXTRA_INIT, 1, nullptr};
//    H101_type.ob_base.ob_size = 0;
//  C++ really contstraints struct initialization even within an extern "C" block, so
//  we do it the stupid way:
//    printf("Hello, world!\n");
    memset(&H101_type, 0, sizeof(H101_type));
    H101_type.tp_basicsize = sizeof(H101);
    H101_type.tp_itemsize = 0;
    H101_type.tp_name = "h101.H101";
    H101_type.tp_doc = PyDoc_STR("A h101 object created from a STRUCT");
    H101_type.tp_flags = Py_TPFLAGS_DEFAULT;
    H101_type.tp_new = PyType_GenericNew;

    if (PyType_Ready(&H101_type)<0) return nullptr;

    PyObject *m = PyModule_Create(&h101module);
    if (!m) return nullptr;

   if (PyModule_AddObject(m, "H101", reinterpret_cast<PyObject*>(&H101_type))<0)
    {
	Py_DECREF(m);
	return nullptr;
    }
	
    return m;
}
